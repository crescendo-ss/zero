#include <string.h>

#include <cfloat>
#include <memory>
#include <string>

#include <zero/BotController.h>
#include <zero/ChatQueue.h>
#include <zero/ZeroBot.h>
#include <zero/behavior/BehaviorBuilder.h>
#include <zero/behavior/BehaviorTree.h>
#include <zero/behavior/nodes/AimNode.h>
#include <zero/behavior/nodes/BlackboardNode.h>
#include <zero/behavior/nodes/InputActionNode.h>
#include <zero/behavior/nodes/MapNode.h>
#include <zero/behavior/nodes/MathNode.h>
#include <zero/behavior/nodes/MoveNode.h>
#include <zero/behavior/nodes/PlayerNode.h>
#include <zero/behavior/nodes/ShipNode.h>
#include <zero/behavior/nodes/TimerNode.h>
#include <zero/control/ControlClient.h>
#include <zero/game/GameEvent.h>
#include <zero/game/Logger.h>
#include <zero/zones/ZoneController.h>

namespace zero {
namespace regression {

enum class MatchState { Idle, Staging, Live };

// Finds the nearest enemy (different frequency, in a ship) and stores Player* in output_key.
struct FindNearestEnemyNode : behavior::BehaviorNode {
  FindNearestEnemyNode(const char* output_key) : output_key(output_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self || self->ship >= 8) return behavior::ExecuteResult::Failure;

    // Match-aware targeting: when MatchTeamCount is configured (>0), restrict candidates to the
    // bot's own match. ClashEngine's MatchFreqAllocator gives each concurrent match a freq band
    // [base, base + teamCount*100) where base = 100 + k*(teamCount*100); a participant on freq F is
    // therefore in the band that F falls into. Cross-match players (different band) are skipped so
    // the bot doesn't waste fire on opponents it can't damage. MatchTeamCount=0 → no filter (old
    // behavior; correct for a single match in the arena).
    int team_count = ctx.blackboard.ValueOr<int>("match_team_count", 0);
    int band_lo = -1, band_hi = -1;
    if (team_count > 0) {
      int band = team_count * 100;
      band_lo = ((self->frequency - 100) / band) * band + 100;
      band_hi = band_lo + band;
    }

    Player* nearest = nullptr;
    float nearest_dist_sq = FLT_MAX;

    auto& pm = ctx.bot->game->player_manager;
    for (size_t i = 0; i < pm.player_count; ++i) {
      Player& p = pm.players[i];
      if (p.ship >= 8) continue;
      if (p.frequency == self->frequency) continue;
      if (p.id == self->id) continue;
      if (band_lo >= 0 && (p.frequency < band_lo || p.frequency >= band_hi)) continue;  // other match

      float dist_sq = self->position.DistanceSq(p.position);
      if (dist_sq < nearest_dist_sq) {
        nearest_dist_sq = dist_sq;
        nearest = &p;
      }
    }

    if (!nearest) return behavior::ExecuteResult::Failure;

    ctx.blackboard.Set(output_key, nearest);
    return behavior::ExecuteResult::Success;
  }

  const char* output_key;
};

// During staging, steers the ship slightly off its spawn position to clear the AFK check.
struct StagingJiggleNode : behavior::BehaviorNode {
  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self || self->ship >= 8) return behavior::ExecuteResult::Failure;

    Vector2f target = self->position + Vector2f(3.0f, 0.0f);
    ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, target);

    return behavior::ExecuteResult::Success;
  }
};

// Same play behavior as the Clash soak bot, but each acting phase is gated on a blackboard flag the
// RegressionZoneController toggles from control-channel commands:
//   * staging jiggle runs only when "afk_suppressed" == 0 (idle off)
//   * combat runs only when "fight_enabled" == 1 (fight on)
struct RegressionBehavior : behavior::Behavior {
  void OnInitialize(behavior::ExecuteContext& ctx) override {
    ctx.blackboard.Set("leash_distance", 25.0f);
    ctx.blackboard.Set("clash_state", (int)MatchState::Idle);
    ctx.blackboard.Set("fight_enabled", 1);
    ctx.blackboard.Set("afk_suppressed", 0);
  }

  std::unique_ptr<behavior::BehaviorNode> CreateTree(behavior::ExecuteContext& ctx) override {
    using namespace behavior;

    BehaviorBuilder builder;

    // clang-format off
    builder
      .Selector()
          .Sequence() // Request ship from config if not already in it.
              .InvertChild<ShipQueryNode>("request_ship")
              .Child<ShipRequestNode>("request_ship")
              .End()
          .Sequence() // AFK jiggle during staging (unless idle-suppressed for a no-show).
              .Child<ValueCompareQuery<int>>("clash_state", (int)MatchState::Staging)
              .Child<ValueCompareQuery<int>>("afk_suppressed", 0)
              .InvertChild<BlackboardSetQueryNode>("clash_afk_cleared")
              .Child<StagingJiggleNode>()
              .End()
          .Sequence() // Combat during live phase (unless fight disabled).
              .Child<ValueCompareQuery<int>>("clash_state", (int)MatchState::Live)
              .Child<ValueCompareQuery<int>>("fight_enabled", 1)
              .Child<PlayerPositionQueryNode>("self_position")
              .Child<FindNearestEnemyNode>("nearest_target")
              .Child<PlayerPositionQueryNode>("nearest_target", "nearest_target_position")
              .Child<AimNode>(WeaponType::Bullet, "nearest_target", "aimshot")
              .Parallel()
                  .Selector() // Movement: path if no LOS, otherwise seek.
                      .Sequence()
                          .InvertChild<VisibilityQueryNode>("nearest_target_position")
                          .Child<GoToNode>("nearest_target_position")
                          .End()
                      .Sequence()
                          .Child<FaceNode>("aimshot")
                          .Child<SeekNode>("aimshot", "leash_distance")
                          .End()
                      .End()
                  .Parallel(CompositeDecorator::Success) // Weapons: run all checks independently.
                      .Sequence(CompositeDecorator::Success) // Bullets with trajectory check.
                          .Child<PlayerEnergyPercentThresholdNode>(0.3f)
                          .InvertChild<ShipWeaponCooldownQueryNode>(WeaponType::Bullet)
                          .Child<ShotVelocityQueryNode>(WeaponType::Bullet, "bullet_vel")
                          .Child<RayNode>("self_position", "bullet_vel", "bullet_ray")
                          .Child<PlayerBoundingBoxQueryNode>("nearest_target", "target_bounds", 4.0f)
                          .Child<MoveRectangleNode>("target_bounds", "aimshot", "target_bounds")
                          .Child<RayRectangleInterceptNode>("bullet_ray", "target_bounds")
                          .Child<InputActionNode>(InputAction::Bullet)
                          .End()
                      .Sequence(CompositeDecorator::Success) // Repel when low on energy.
                          .Child<ShipWeaponCapabilityQueryNode>(WeaponType::Repel)
                          .Child<TimerExpiredNode>("repel_timer")
                          .InvertChild<PlayerEnergyPercentThresholdNode>(0.2f)
                          .Child<InputActionNode>(InputAction::Repel)
                          .Child<TimerSetNode>("repel_timer", 150)
                          .End()
                      .End()
                  .End()
              .End()
          .End();
    // clang-format on

    return builder.Build();
  }
};

struct RegressionZoneController : ZoneController, EventHandler<ChatEvent> {
  MatchState match_state = MatchState::Idle;
  bool fight_enabled = true;
  bool afk_suppressed = false;
  int match_team_count = 0;  // [Regression] MatchTeamCount; 0 = no match-aware targeting filter

  ControlClient control;
  bool hello_sent = false;

  bool IsZone(Zone zone) override {
    return zone == Zone::Regression;
  }

  void CreateBehaviors(const char* arena_name) override {
    Log(LogLevel::Info, "Registering Regression behaviors.");

    bot->bot_controller->energy_tracker.estimate_type = EnergyHeuristicType::Average;

    auto& repo = bot->bot_controller->behaviors;
    repo.Add("regression", std::make_unique<RegressionBehavior>());
    SetBehavior("regression");

    auto mtc = bot->config->GetInt("Regression", "MatchTeamCount");
    match_team_count = mtc ? *mtc : 0;

    auto& bb = bot->execute_ctx.blackboard;
    bb.Set("clash_state", (int)match_state);
    bb.Set("fight_enabled", fight_enabled ? 1 : 0);
    bb.Set("afk_suppressed", afk_suppressed ? 1 : 0);
    bb.Set("match_team_count", match_team_count);

    ConnectControl(arena_name);
  }

  void ConnectControl(const char* arena_name) {
    if (control.connected) return;

    auto host = bot->config->GetString("Regression", "ControlHost");
    auto port = bot->config->GetInt("Regression", "ControlPort");
    auto slot = bot->config->GetInt("Regression", "Slot");

    std::string host_str = host ? std::string(*host) : std::string("127.0.0.1");
    int port_num = port ? *port : 0;
    int slot_num = slot ? *slot : 0;

    if (port_num <= 0) {
      Log(LogLevel::Warning, "Regression: no [Regression] ControlPort set; running uncontrolled.");
      return;
    }

    control.on_line = [this](const std::string& line) { OnControlLine(line); };

    if (!control.Connect(host_str.c_str(), port_num)) {
      Log(LogLevel::Warning, "Regression: control connect to %s:%d failed.", host_str.c_str(), port_num);
      return;
    }

    // HELLO\t<slot>\t<username>\t<token>
    std::string hello = "HELLO\t" + std::to_string(slot_num) + "\t" + std::string(bot->name) + "\ttok";
    control.SendLine(hello);
    hello_sent = true;

    Emit("login-ok", "");
    Emit("entered-arena", arena_name ? arena_name : "");
    Log(LogLevel::Info, "Regression: control connected (%s:%d, slot %d).", host_str.c_str(), port_num, slot_num);
  }

  void Emit(const char* name, const std::string& detail) {
    if (!control.connected) return;
    control.SendLine(std::string("EVT\t") + name + "\t" + detail);
  }

  void OnControlLine(const std::string& line) {
    // CMD\t<action>\t<arg>
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) return;
    std::string verb = line.substr(0, t1);
    if (verb != "CMD") return;

    size_t t2 = line.find('\t', t1 + 1);
    std::string action = line.substr(t1 + 1, (t2 == std::string::npos) ? std::string::npos : t2 - (t1 + 1));
    std::string arg = (t2 == std::string::npos) ? "" : line.substr(t2 + 1);

    if (action == "play") {
      std::string cmd = arg.empty() ? "?play" : ("?play " + arg);
      Event::Dispatch(ChatQueueEvent::Public(cmd.data()));
    } else if (action == "cancel") {
      Event::Dispatch(ChatQueueEvent::Public("?cancel"));
    } else if (action == "return") {
      // Rejoin the match we specced out of. Reset the requested ship first so the autonomous
      // ship-request sequence doesn't immediately re-spec us after the engine places us back.
      bot->execute_ctx.blackboard.Set("request_ship", 0);
      Event::Dispatch(ChatQueueEvent::Public("?return"));
    } else if (action == "accept") {
      Event::Dispatch(ChatQueueEvent::Public("?accept"));
    } else if (action == "decline") {
      Event::Dispatch(ChatQueueEvent::Public("?decline"));
    } else if (action == "forgive") {
      std::string cmd = arg.empty() ? "?forgive" : ("?forgive " + arg);
      Event::Dispatch(ChatQueueEvent::Public(cmd.data()));
    } else if (action == "party") {
      std::string cmd = arg.empty() ? "?party" : ("?party " + arg);
      Event::Dispatch(ChatQueueEvent::Public(cmd.data()));
    } else if (action == "say") {
      Event::Dispatch(ChatQueueEvent::Public(arg.data()));
    } else if (action == "requestship") {
      int ship = arg.empty() ? 1 : atoi(arg.data());
      if (ship >= 1 && ship <= 8) bot->execute_ctx.blackboard.Set("request_ship", ship - 1);
    } else if (action == "spec") {
      // Spectator is ship index 8; the request-ship sequence will move us there and keep us there.
      bot->execute_ctx.blackboard.Set("request_ship", 8);
    } else if (action == "fight") {
      fight_enabled = (arg == "on");
    } else if (action == "idle") {
      afk_suppressed = (arg == "on");
    } else if (action == "disconnect") {
      control.Close();
    } else {
      Log(LogLevel::Warning, "Regression: unknown control action '%s'.", action.c_str());
    }
  }

  void HandleEvent(const BotController::UpdateEvent& event) override {
    if (!in_zone) return;

    control.Poll();

    auto& bb = bot->execute_ctx.blackboard;
    bb.Set("clash_state", (int)match_state);
    bb.Set("fight_enabled", fight_enabled ? 1 : 0);
    bb.Set("afk_suppressed", afk_suppressed ? 1 : 0);
    bb.Set("match_team_count", match_team_count);
  }

  void HandleEvent(const ChatEvent& event) override {
    if (!in_zone || !event.message) return;

    // Party invite (independent of match lifecycle): "<inviter> invited you to a party. ..."
    if (strstr(event.message, "invited you to a party")) {
      Emit("invite", event.sender ? event.sender : "");
    }

    if (strstr(event.message, "Match found!")) {
      match_state = MatchState::Staging;
      bot->execute_ctx.blackboard.Erase("clash_afk_cleared");
      Emit("match-found", "");
    } else if (strstr(event.message, "you're ready")) {
      bot->execute_ctx.blackboard.Set("clash_afk_cleared", true);
      Emit("ready", "");
    } else if (strcmp(event.message, "GO!") == 0) {
      match_state = MatchState::Live;
      Emit("go", "");
    } else if (strstr(event.message, "Match cancelled")) {
      match_state = MatchState::Idle;
      Emit("match-cancelled", "");
    } else if (strstr(event.message, "Match over!")) {
      match_state = MatchState::Idle;
      Emit("match-over", "");
    } else if (strstr(event.message, "All set")) {
      // Staging cleared -> countdown begins. Past this point leaving is an abandon, not a
      // penalty-free cancel ("All set!" / "All set! Pick your final ship -- Ns until lock...").
      Emit("all-set", "");
    } else if (const char* r = strstr(event.message, " returned to the match")) {
      // "<name> returned to the match. [Items ...] [Lives: N]" -- detail carries the returner.
      Emit("returned", std::string(event.message, r - event.message));
    } else if (strstr(event.message, "Locked you to your current ship")) {
      Emit("ship-locked", "");
    } else if (strstr(event.message, "for the rest of the life")) {
      // Denied ship change: "You're locked to your current ship for the rest of the life."
      Emit("ship-denied", "");
    } else if (strstr(event.message, "to change ships before being locked")) {
      // Post-death grace window opened: "You have Ns to change ships before being locked...".
      Emit("ship-window", "");
    } else if (strstr(event.message, "You abandoned a match")) {
      // DM to the player the engine just assessed an abandonment penalty against.
      Emit("abandoned", "");
    }

    bot->execute_ctx.blackboard.Set("clash_state", (int)match_state);
  }
};

static RegressionZoneController controller;

}  // namespace regression
}  // namespace zero
