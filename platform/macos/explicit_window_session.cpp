#include "platform/macos/explicit_window_session.hpp"

namespace saccade::platform::macos {
namespace {

constexpr uint32_t identity_flag_mask = explicit_window_visible | explicit_window_current_space;

bool same_bounds(const SaccadeRectI32& left, const SaccadeRectI32& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
}

bool same_identity(const ExplicitWindowIdentity& left, const ExplicitWindowIdentity& right) noexcept {
    return left.process_id == right.process_id && left.window_id == right.window_id && left.capture_source_id == right.capture_source_id &&
           same_bounds(left.bounds, right.bounds) && left.flags == right.flags && left.reserved == right.reserved;
}

bool valid_identity(const ExplicitWindowIdentity& identity) noexcept {
    return identity.process_id != 0 && identity.window_id != 0 && identity.capture_source_id != 0 && identity.bounds.width > 0 &&
           identity.bounds.height > 0 && identity.flags == identity_flag_mask && identity.reserved == 0;
}

bool valid_generation(const ExplicitWindowSceneGeneration& generation) noexcept {
    return generation.scene_epoch != 0 && generation.frame_id != 0 && generation.transform_epoch != 0 &&
           generation.topology_epoch != 0 && generation.permission_epoch != 0;
}

bool newer_generation(const ExplicitWindowSceneGeneration& next, const ExplicitWindowSceneGeneration& previous) noexcept {
    return next.scene_epoch > previous.scene_epoch && next.frame_id > previous.frame_id &&
           next.transform_epoch >= previous.transform_epoch && next.topology_epoch >= previous.topology_epoch &&
           next.permission_epoch >= previous.permission_epoch;
}

bool token_matches(const ExplicitWindowActionToken& token, const ExplicitWindowIdentity& identity, uint64_t session_epoch,
                   const ExplicitWindowSceneGeneration& generation) noexcept {
    return token.session_epoch == session_epoch && token.scene_epoch == generation.scene_epoch &&
           token.frame_id == generation.frame_id && token.transform_epoch == generation.transform_epoch &&
           token.topology_epoch == generation.topology_epoch && token.permission_epoch == generation.permission_epoch &&
           token.process_id == identity.process_id && token.window_id == identity.window_id &&
           token.capture_source_id == identity.capture_source_id && same_bounds(token.bounds, identity.bounds);
}

bool valid_activation_key(const ExplicitWindowActivationKey& key) noexcept {
    return key.request_id != 0 && key.process_id != 0 && key.window_id != 0 && key.source_generation != 0 && key.deadline_ns != 0;
}

bool same_activation_key(const ExplicitWindowActivationKey& left, const ExplicitWindowActivationKey& right) noexcept {
    return left.request_id == right.request_id && left.process_id == right.process_id && left.window_id == right.window_id &&
           left.source_generation == right.source_generation && left.deadline_ns == right.deadline_ns;
}

} // namespace

SaccadeResult ExplicitWindowSession::select(const ExplicitWindowIdentity& identity, uint64_t session_epoch) noexcept {
    if (!valid_identity(identity) || session_epoch == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (active_)
        return same_identity(identity_, identity) && session_epoch_ == session_epoch ? SACCADE_OK : SACCADE_ERROR_BUSY;
    if (session_epoch <= session_epoch_)
        return SACCADE_ERROR_STALE_HANDLE;

    identity_ = identity;
    generation_ = {};
    session_epoch_ = session_epoch;
    retirement_reason_ = ExplicitWindowRetirementReason::none;
    active_ = true;
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowSession::publish(const ExplicitWindowIdentity& identity,
                                             const ExplicitWindowSceneGeneration& generation,
                                             ExplicitWindowActionToken* output) noexcept {
    if (output == nullptr || !valid_generation(generation))
        return SACCADE_ERROR_INVALID_ARGUMENT;
    if (!active_)
        return SACCADE_ERROR_STATE;
    if (!same_identity(identity_, identity) || !newer_generation(generation, generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }

    generation_ = generation;
    *output = {
        session_epoch_,
        generation.scene_epoch,
        generation.frame_id,
        generation.transform_epoch,
        generation.topology_epoch,
        generation.permission_epoch,
        identity_.process_id,
        identity_.window_id,
        identity_.capture_source_id,
        identity_.bounds,
    };
    return SACCADE_OK;
}

SaccadeResult ExplicitWindowSession::validate(const ExplicitWindowIdentity& identity,
                                              const ExplicitWindowActionToken& token) const noexcept {
    if (!active_ || !same_identity(identity_, identity) ||
        !token_matches(token, identity_, session_epoch_, generation_)) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    return SACCADE_OK;
}

void ExplicitWindowSession::retire(ExplicitWindowRetirementReason reason) noexcept {
    if (!active_ || reason == ExplicitWindowRetirementReason::none)
        return;
    active_ = false;
    retirement_reason_ = reason;
}

ExplicitWindowActivationAdmission ExplicitWindowActivationState::admit(const ExplicitWindowActivationKey& key) noexcept {
    if (!valid_activation_key(key))
        return ExplicitWindowActivationAdmission::invalid;
    if (phase_ == Phase::waiting)
        return same_activation_key(key_, key) ? ExplicitWindowActivationAdmission::resume
                                              : ExplicitWindowActivationAdmission::busy;
    if (phase_ == Phase::cancelled && same_activation_key(key_, key)) {
        complete();
        return ExplicitWindowActivationAdmission::cancelled;
    }

    key_ = key;
    previous_scene_epoch_ = 0;
    phase_ = Phase::waiting;
    return ExplicitWindowActivationAdmission::start;
}

SaccadeResult ExplicitWindowActivationState::set_previous_scene_epoch(uint64_t scene_epoch) noexcept {
    if (phase_ != Phase::waiting)
        return SACCADE_ERROR_STATE;
    previous_scene_epoch_ = scene_epoch;
    return SACCADE_OK;
}

void ExplicitWindowActivationState::cancel() noexcept {
    if (phase_ == Phase::waiting)
        phase_ = Phase::cancelled;
}

void ExplicitWindowActivationState::complete() noexcept {
    key_ = {};
    previous_scene_epoch_ = 0;
    phase_ = Phase::idle;
}

} // namespace saccade::platform::macos
