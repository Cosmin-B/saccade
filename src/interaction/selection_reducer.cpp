#include "interaction/selection_reducer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace saccade::interaction {

SaccadeResult SelectionReducer::begin(const scene::PacketView& scene, SelectionMode mode,
                                      const SelectionContext& context, SelectionStorage* storage) noexcept {
    if (state_ != SelectionState::idle || scene.header == nullptr || scene.targets == nullptr || storage == nullptr ||
        mode < SelectionMode::single || mode > SelectionMode::path || context.scene_epoch == 0 ||
        context.transform_epoch == 0 || context.topology_epoch == 0 || context.deadline_ns == 0 ||
        scene.header->scene_epoch != context.scene_epoch || scene.header->transform_epoch != context.transform_epoch ||
        scene.header->topology_epoch != context.topology_epoch) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    scene_header_ = scene.header;
    scene_targets_ = scene.targets;
    storage_ = storage;
    context_ = context;
    target_count_ = 0;
    mode_ = mode;
    state_ = SelectionState::collecting;
    cancel_reason_ = SelectionCancelReason::none;

    ++stats_.sessions_started;

    return SACCADE_OK;
}

const SaccadeTargetRecord* SelectionReducer::find_target(uint64_t target_id) const noexcept {
    for (uint32_t index = 0; index < scene_header_->target_count; ++index) {
        if (scene_targets_[index].target_id == target_id) {
            return &scene_targets_[index];
        }
    }

    return nullptr;
}

bool SelectionReducer::selected(uint64_t target_id) const noexcept {
    for (uint32_t index = 0; index < target_count_; ++index) {
        if (storage_->target_ids[index] == target_id) {
            return true;
        }
    }

    return false;
}

SaccadeResult SelectionReducer::expand_path() noexcept {
    std::array<uint64_t, maximum_selection_targets> anchors{};
    std::copy_n(storage_->target_ids.begin(), target_count_, anchors.begin());
    std::array<uint64_t, maximum_selection_targets> expanded{};
    std::array<const SaccadeTargetRecord*, maximum_selection_targets> matches{};
    uint32_t expanded_count = 0;

    for (uint32_t segment = 1; segment < target_count_; ++segment) {
        const SaccadeTargetRecord* first = find_target(anchors[segment - 1U]);
        const SaccadeTargetRecord* last = find_target(anchors[segment]);
        if (first == nullptr || last == nullptr) return SACCADE_ERROR_STALE_HANDLE;

        const auto before = [](const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
            return left.order < right.order || (left.order == right.order && left.target_id < right.target_id);
        };
        const SaccadeTargetRecord* low = before(*last, *first) ? last : first;
        const SaccadeTargetRecord* high = before(*last, *first) ? first : last;
        uint32_t match_count = 0;

        for (uint32_t index = 0; index < scene_header_->target_count; ++index) {
            const SaccadeTargetRecord* target = &scene_targets_[index];
            if (before(*target, *low) || before(*high, *target)) continue;
            if (match_count == maximum_selection_targets) return SACCADE_ERROR_CAPACITY;

            uint32_t position = match_count;
            while (position != 0 && before(*target, *matches[position - 1U])) {
                matches[position] = matches[position - 1U];
                --position;
            }
            matches[position] = target;
            ++match_count;
        }

        if (match_count < 2) return SACCADE_ERROR_NOT_FOUND;
        const bool forward = first == low;
        for (uint32_t index = 0; index < match_count; ++index) {
            const uint32_t match_index = forward ? index : match_count - index - 1U;
            const uint64_t target_id = matches[match_index]->target_id;
            if (expanded_count != 0 && expanded[expanded_count - 1U] == target_id) continue;
            if (expanded_count == maximum_selection_targets) return SACCADE_ERROR_CAPACITY;
            expanded[expanded_count++] = target_id;
        }
    }

    std::copy_n(expanded.begin(), expanded_count, storage_->target_ids.begin());
    target_count_ = expanded_count;
    stats_.path_targets_expanded += expanded_count;

    return SACCADE_OK;
}

SaccadeResult SelectionReducer::select(uint64_t target_id) noexcept {
    if (state_ != SelectionState::collecting || target_id == 0) {
        return SACCADE_ERROR_STATE;
    }

    if (find_target(target_id) == nullptr) {
        return SACCADE_ERROR_NOT_FOUND;
    }

    if (selected(target_id)) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }

    if (target_count_ == maximum_selection_targets) {
        return SACCADE_ERROR_CAPACITY;
    }

    storage_->target_ids[target_count_++] = target_id;

    ++stats_.targets_selected;

    if (mode_ == SelectionMode::single || (mode_ == SelectionMode::dual && target_count_ == 2)) {
        state_ = SelectionState::complete;
    }

    return SACCADE_OK;
}

SaccadeResult SelectionReducer::backspace() noexcept {
    if (state_ != SelectionState::collecting || target_count_ == 0) {
        return SACCADE_ERROR_STATE;
    }

    --target_count_;
    ++stats_.backspaces;

    return SACCADE_OK;
}

SaccadeResult SelectionReducer::confirm() noexcept {
    const bool multi_ready = mode_ == SelectionMode::multi && target_count_ != 0;
    const bool path_ready = mode_ == SelectionMode::path && target_count_ >= 2;
    if (state_ != SelectionState::collecting || (!multi_ready && !path_ready)) {
        return SACCADE_ERROR_STATE;
    }

    if (path_ready) {
        const SaccadeResult result = expand_path();
        if (result != SACCADE_OK) return result;
    }

    state_ = SelectionState::complete;
    ++stats_.confirmations;

    return SACCADE_OK;
}

SaccadeResult SelectionReducer::cancel(SelectionCancelReason reason) noexcept {
    if (state_ != SelectionState::collecting || reason <= SelectionCancelReason::none ||
        reason > SelectionCancelReason::permission_lost) {
        return SACCADE_ERROR_STATE;
    }

    target_count_ = 0;
    state_ = SelectionState::cancelled;
    cancel_reason_ = reason;

    ++stats_.cancellations;

    return SACCADE_OK;
}

SaccadeResult SelectionReducer::refresh_scene(const scene::PacketView& scene) noexcept {
    if ((state_ != SelectionState::collecting && state_ != SelectionState::complete) || scene.header == nullptr ||
        scene.targets == nullptr || scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        scene.header->scene_epoch <= context_.scene_epoch ||
        scene.header->transform_epoch != context_.transform_epoch ||
        scene.header->topology_epoch != context_.topology_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }

    std::array<uint64_t, maximum_selection_targets> selected_ids{};
    std::copy_n(storage_->target_ids.begin(), target_count_, selected_ids.begin());
    std::sort(selected_ids.begin(), selected_ids.begin() + target_count_);
    uint32_t matches = 0;
    for (uint32_t index = 0; index < scene.header->target_count; ++index) {
        matches += std::binary_search(selected_ids.begin(), selected_ids.begin() + target_count_,
                                      scene.targets[index].target_id);
    }
    if (matches != target_count_) return SACCADE_ERROR_STALE_HANDLE;

    scene_header_ = scene.header;
    scene_targets_ = scene.targets;
    context_.scene_epoch = scene.header->scene_epoch;
    ++stats_.scene_refreshes;
    return SACCADE_OK;
}

SaccadeResult SelectionReducer::validate(const SelectionContext& current, uint64_t now_ns) noexcept {
    if (state_ != SelectionState::collecting) {
        return SACCADE_ERROR_STATE;
    }

    SelectionCancelReason reason = SelectionCancelReason::none;

    if (now_ns >= context_.deadline_ns) {
        reason = SelectionCancelReason::timeout;
    } else if (current.scene_epoch != context_.scene_epoch) {
        reason = SelectionCancelReason::scene_changed;
    } else if (current.transform_epoch != context_.transform_epoch) {
        reason = SelectionCancelReason::transform_changed;
    } else if (current.topology_epoch != context_.topology_epoch) {
        reason = SelectionCancelReason::topology_changed;
    } else if (current.focus_id != context_.focus_id) {
        reason = SelectionCancelReason::focus_changed;
    }

    if (reason == SelectionCancelReason::none) {
        return SACCADE_OK;
    }

    (void)cancel(reason);

    return SACCADE_ERROR_STALE_HANDLE;
}

SaccadeResult SelectionReducer::reset() noexcept {
    if (state_ == SelectionState::idle) {
        return SACCADE_ERROR_STATE;
    }

    scene_header_ = nullptr;
    scene_targets_ = nullptr;
    storage_ = nullptr;
    context_ = {};
    target_count_ = 0;
    mode_ = SelectionMode::single;
    state_ = SelectionState::idle;
    cancel_reason_ = SelectionCancelReason::none;

    return SACCADE_OK;
}

SelectionView SelectionReducer::view() const noexcept {
    return {storage_ == nullptr ? nullptr : storage_->target_ids.data(), target_count_, mode_, state_, cancel_reason_};
}

} // namespace saccade::interaction
