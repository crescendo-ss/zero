# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`zero` is a SubSpace/Continuum network bot written in C++20. It connects to game zones using either Subspace (VIE) or Continuum encryption, simulates a player client, and drives ship behavior via behavior trees. Connecting to SSC zones (Continuum encryption) requires an external `cryptinuum` security solver at `127.0.0.1`; without it only `enc_null`/Subspace zones work.

## Build

Requires C++20. Git submodules must be initialized: `git submodule init && git submodule update` (pulls `lib/glfw`, `lib/glad`, etc.).

- **Windows**: open `zero.sln` and build Release x64. CMake also works.
- **Linux/CMake**: `cmake -DCMAKE_BUILD_TYPE=Release -B build -S . && cmake --build build -j`. GLFW3 is optional — without it the debug render window is disabled (`-DGLFW_AVAILABLE`).
- The CMake build globs all `zero/*.cpp` recursively, so new `.cpp` files are picked up automatically; the Visual Studio `.vcxproj` is a separately-maintained file list and must be updated when adding sources.

There is no test suite.

## Run

- Copy `zero.cfg.dist` → `zero.cfg`, edit Login section. Config search order: argv[1] (if not a flag) → `zero.cfg` → `zero.cfg.dist`.
- Common overrides: `--name`, `--password`, `--server <local|sg|hs|deva|mg|eg|tw|nexus|hz>`, `--ship <1-9>`, `-b <behavior>`, `--arena`, `-l <j|d|i|w|e>`, `--render`.
- `generate.py [count]` produces N config files plus a `run.bat` for spawning a swarm of bots against a local subgame server.
- The debug render window also needs Continuum's `graphics/` folder copied next to the executable.

## Architecture

### Bot lifecycle

`main.cpp` loads config, picks a `ServerInfo` from the hard-coded `kServers[]` table (indexed by the `Zone` enum), then calls `ZeroBot::Initialize` → `JoinZone` → `Run`. `ZeroBot` owns the `Game` (network/simulation), three `MemoryArena`s (perm/trans/work), the `BotController`, and the `CommandSystem`. The main loop ticks `Game` and `BotController` until disconnect.

### Game layer (`zero/game/`)

A from-scratch Continuum client: `net/Connection` speaks the VIE/Continuum protocol over UDP, `PacketDispatcher` routes packet IDs to handlers, `PlayerManager`/`WeaponManager`/`BrickManager`/`Soccer` track world state, `ShipController` applies inputs, `Map` parses `.lvl` files. Rendering (under `game/render/`) is optional and gated on GLFW availability — when off, the game still runs in "Simulation" mode (see `GameInitializeResult`). Custom arena allocators (`Memory.h`) are used pervasively; do not free memory inside an arena, just reset the arena.

### Event bus (`zero/Event.h`)

Singleton-per-type pub/sub. Any struct deriving from `Event` can be dispatched with `Event::Dispatch(event)`; subscribers inherit `EventHandler<EventType>` and override `HandleEvent`. Subscribers self-register in their constructor and unregister on destruction. This is how the game layer surfaces packet-driven state changes (`JoinGameEvent`, `ArenaNameEvent`, `MapLoadEvent`, `PlayerEnterEvent`, etc.) to the bot layer without explicit wiring.

### BotController + behavior trees (`zero/BotController.*`, `zero/behavior/`)

`BotController` is the per-tick brain: it owns the pathfinder, region registry, steering, actuator, chat queue, influence map, and the active behavior tree. Each tick `Update` builds an `ExecuteContext` (containing a `Blackboard` of typed key/value state) and runs `behavior_tree->Execute(ctx)`. Trees are composed of `SequenceNode` / `SelectorNode` / `ParallelNode` / `SuccessNode` / `InvertNode` / `ExecuteNode` plus leaf nodes under `behavior/nodes/` (aim, move, target, threat, region, flag, powerball, attach, ship, timer, etc.). Use `CompositeBuilder` (`BehaviorBuilder.h`) to assemble trees fluently. Behaviors share information across nodes by reading/writing the blackboard, not by direct references.

### Zone controllers (`zero/zones/<zone>/`)

Each zone has a `ZoneController` subclass declared as a `static` global at file scope — its constructor self-registers via the `EventHandler` base classes. On `JoinRequestEvent` it checks `IsZone(zone)` to decide whether it's the active controller for this session; on `ArenaNameEvent` it calls `CreateBehaviors(arena_name)` to register named `Behavior` instances into `BotController::behaviors` (the `BehaviorRepository`) and selects a default via `SetBehavior(name)`. The config key `[ZoneName]::Behavior` (or `--behavior` CLI) picks which named behavior runs. Zone controllers can also stash zone-specific state (e.g. `HockeyZone` rink geometry) onto the blackboard for nodes to consume. To add a new zone: add an entry to the `Zone` enum + `kZoneNames` + `kServers[]` + `kServerMap` in `main.cpp`, then create `zero/zones/<zone>/` with a static controller instance.

### Commands (`zero/commands/CommandSystem.*`)

Players send `!command` in chat; `CommandSystem` parses, checks the operator's security level against the command's required level (configured via `[Operators]` and `[CommandAccess]` config groups), and dispatches to a `CommandExecutor`. Zone controllers register their own commands during `CreateBehaviors`.

### Config (`zero/Config.*`)

INI-style with `[Group]` sections. Many zone controllers use the pattern `GetString({to_string(zone), "General"}, key)` so a zone-specific section overrides `[General]`. Some keys (`Behavior`, `RequestShip`, `Arena`) are read both globally and per-zone — preserve this pattern when adding new tunables.

## Conventions

- C++20, Google-style braces, 120-col, 2-space indent — enforced by `.clang-format` at the repo root. Run clang-format on changes.
- Coordinates are `Vector2f` in tile units (the map is 1024×1024 tiles). `TileId`, `Tick`, `RegionIndex` are typed wrappers in `zero/Types.h`.
- Prefer arena allocation for transient data inside the per-tick path. `trans_arena` is reset each frame; `perm_arena` lives for the bot's lifetime.
- New behavior nodes go under `zero/behavior/nodes/` (generic) or `zero/zones/<zone>/nodes/` (zone-specific). They read/write through the blackboard rather than via constructors so they're composable.
