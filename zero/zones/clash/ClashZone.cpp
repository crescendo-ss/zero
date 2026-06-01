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
#include <zero/game/GameEvent.h>
#include <zero/game/Logger.h>
#include <zero/zones/ZoneController.h>

namespace zero {
namespace clash {

enum class MatchState { Idle, Staging, Live };

static bool StartsWith(const char* str, const char* prefix) {
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

// Finds the nearest enemy (different frequency, in a ship) and stores Player* in output_key.
struct FindNearestEnemyNode : behavior::BehaviorNode {
  FindNearestEnemyNode(const char* output_key) : output_key(output_key) {}

  behavior::ExecuteResult Execute(behavior::ExecuteContext& ctx) override {
    auto self = ctx.bot->game->player_manager.GetSelf();
    if (!self || self->ship >= 8) return behavior::ExecuteResult::Failure;

    Player* nearest = nullptr;
    float nearest_dist_sq = FLT_MAX;

    auto& pm = ctx.bot->game->player_manager;
    for (size_t i = 0; i < pm.player_count; ++i) {
      Player& p = pm.players[i];
      if (p.ship >= 8) continue;
      if (p.frequency == self->frequency) continue;
      if (p.id == self->id) continue;

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

    // Seek 3 tiles to the right of current position; small movement clears the server-side idle check.
    Vector2f target = self->position + Vector2f(3.0f, 0.0f);
    ctx.bot->bot_controller->steering.Seek(*ctx.bot->game, target);

    return behavior::ExecuteResult::Success;
  }
};

struct ClashBehavior : behavior::Behavior {
  void OnInitialize(behavior::ExecuteContext& ctx) override {
    ctx.blackboard.Set("leash_distance", 25.0f);
    ctx.blackboard.Set("clash_state", (int)MatchState::Idle);
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
          .Sequence() // AFK jiggle during staging phase.
              .Child<ValueCompareQuery<int>>("clash_state", (int)MatchState::Staging)
              .InvertChild<BlackboardSetQueryNode>("clash_afk_cleared")
              .Child<StagingJiggleNode>()
              .End()
          .Sequence() // Combat during live phase.
              .Child<ValueCompareQuery<int>>("clash_state", (int)MatchState::Live)
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

struct ClashZoneController : ZoneController, EventHandler<ChatEvent> {
  MatchState match_state = MatchState::Idle;
  u32 requeue_tick = 0;
  bool requeue_pending = false;
  std::string queue_name;

  bool IsZone(Zone zone) override {
    return zone == Zone::Clash;
  }

  void CreateBehaviors(const char* arena_name) override {
    Log(LogLevel::Info, "Registering Clash behaviors.");

    const char* groups[] = {to_string(bot->server_info.zone), "General"};
    auto q = bot->config->GetString(groups, 2, "Queue");
    queue_name = q ? std::string(*q) : "";

    bot->bot_controller->energy_tracker.estimate_type = EnergyHeuristicType::Average;

    auto& repo = bot->bot_controller->behaviors;
    repo.Add("clash", std::make_unique<ClashBehavior>());
    SetBehavior("clash");

    bot->execute_ctx.blackboard.Set("clash_state", (int)match_state);

    if (match_state == MatchState::Idle && !requeue_pending) {
      requeue_pending = true;
      requeue_tick = GetCurrentTick() + 150;
    }
  }

  void HandleEvent(const BotController::UpdateEvent& event) override {
    if (!in_zone) return;

    bot->execute_ctx.blackboard.Set("clash_state", (int)match_state);

    if (requeue_pending && TICK_GTE(GetCurrentTick(), requeue_tick)) {
      requeue_pending = false;
      std::string cmd = queue_name.empty() ? "?play" : ("?play " + queue_name);
      Event::Dispatch(ChatQueueEvent::Public(cmd.data()));
    }
  }

  // The state machine is driven by ClashEngine's server chat. These matches deliberately key on the
  // shortest *stable* fragment of each message rather than the full line, so wording tweaks on the
  // server side don't silently break the flow. Reference strings (ClashEngine, as of this writing):
  //   Staging start  : "You have N seconds to move or fire to confirm you're here. ..."  (DM to each
  //                     participant, MatchOrchestrator.BeginSetup). Older builds: "Match found! ...".
  //   AFK cleared    : "Got it -- you're ready. Standby for the countdown."  (DM on first movement)
  //   Live           : "GO!"  (arena broadcast w/ Ding on the final countdown tick)
  //   Match ended    : "Match over! ...", "Match cancelled. ...", "Match abandoned."
  void HandleEvent(const ChatEvent& event) override {
    if (!in_zone || !event.message) return;

    const char* msg = event.message;

    if (strstr(msg, "move or fire") || strstr(msg, "Match found")) {
      // "move or fire" is the call-to-action verb phrase, unique to the staging notice.
      if (match_state != MatchState::Staging) {
        Log(LogLevel::Info, "Clash: staging started, moving to clear AFK check.");
        match_state = MatchState::Staging;
        bot->execute_ctx.blackboard.Erase("clash_afk_cleared");
      }
    } else if (strstr(msg, "you're ready")) {
      Log(LogLevel::Info, "Clash: AFK check cleared.");
      bot->execute_ctx.blackboard.Set("clash_afk_cleared", true);
    } else if (StartsWith(msg, "GO!")) {
      // Anchored to the start so the "...ships are locked 5s before GO." / "...then GO." lines (note
      // the period, not "!") don't trip the live transition early.
      Log(LogLevel::Info, "Clash: match live.");
      match_state = MatchState::Live;
    } else if (strstr(msg, "Match over") || strstr(msg, "Match cancelled") || strstr(msg, "Match abandoned")) {
      Log(LogLevel::Info, "Clash: match ended, will re-queue.");
      match_state = MatchState::Idle;
      requeue_pending = true;
      requeue_tick = GetCurrentTick() + 500;
    }

    bot->execute_ctx.blackboard.Set("clash_state", (int)match_state);
  }
};

static ClashZoneController controller;

}  // namespace clash
}  // namespace zero
