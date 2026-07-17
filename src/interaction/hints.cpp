#include "interaction/hints.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace saccade::interaction {
namespace {

uint16_t canonical_symbol(uint16_t symbol) noexcept {
    return symbol >= 'a' && symbol <= 'z' ? static_cast<uint16_t>(symbol - ('a' - 'A')) : symbol;
}

bool config_valid(const HintConfig& config) noexcept {
    if (config.alphabet_count < 2 || config.alphabet_count > maximum_hint_alphabet ||
        config.priority > HintPriority::randomized) {
        return false;
    }

    for (uint32_t index = 0; index < config.alphabet_count; ++index) {
        if (config.alphabet[index] == 0) {
            return false;
        }

        for (uint32_t prior = 0; prior < index; ++prior) {
            if (canonical_symbol(config.alphabet[prior]) == canonical_symbol(config.alphabet[index])) {
                return false;
            }
        }
    }

    return true;
}

uint64_t splitmix64(uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

uint64_t absolute_difference(int32_t left, int32_t right) noexcept {
    const int64_t difference = static_cast<int64_t>(left) - right;
    return static_cast<uint64_t>(difference < 0 ? -difference : difference);
}

uint64_t distance(const SaccadeTargetRecord& target, int32_t x, int32_t y) noexcept {
    return absolute_difference(target.safe_x_q8, x) + absolute_difference(target.safe_y_q8, y);
}

void encode_ordinal(uint64_t ordinal, uint32_t length, const HintConfig& config, HintLabel* label) noexcept {
    label->symbol_count = static_cast<uint16_t>(length);

    for (uint32_t position = length; position != 0; --position) {
        label->symbols[position - 1U] = config.alphabet[ordinal % config.alphabet_count];
        ordinal /= config.alphabet_count;
    }
}

void assign_label(uint32_t rank, uint32_t target_index, const scene::PacketView& scene, const HintConfig& config,
                  uint32_t depth, uint64_t shortest_count, uint32_t partial_children, HintLabel* output) noexcept {
    *output = {};
    output->target_id = scene.targets[target_index].target_id;
    output->target_index = target_index;

    if (scene.header->target_count <= config.alphabet_count) {
        output->symbol_count = 1;
        output->symbols[0] = config.alphabet[rank];
        return;
    }

    if (rank < shortest_count) {
        encode_ordinal(rank, depth, config, output);
        return;
    }

    uint64_t child_rank = rank - shortest_count;
    uint64_t parent = shortest_count;
    uint32_t child = 0;

    if (partial_children != 0 && child_rank < partial_children) {
        child = static_cast<uint32_t>(child_rank);
    } else {
        if (partial_children != 0) {
            child_rank -= partial_children;
            ++parent;
        }
        parent += child_rank / config.alphabet_count;
        child = static_cast<uint32_t>(child_rank % config.alphabet_count);
    }

    encode_ordinal(parent, depth, config, output);
    output->symbols[depth] = config.alphabet[child];
    output->symbol_count = static_cast<uint16_t>(depth + 1U);
}

bool prefix_matches(const HintLabel& label, const uint16_t* symbols, uint32_t symbol_count,
                    uint64_t* comparisons) noexcept {
    if (symbol_count > label.symbol_count) {
        return false;
    }

    for (uint32_t index = 0; index < symbol_count; ++index) {
        ++*comparisons;
        if (canonical_symbol(label.symbols[index]) != canonical_symbol(symbols[index])) {
            return false;
        }
    }

    return true;
}

} // namespace

uint16_t symbol_for_physical_key(const HintConfig& config, uint32_t physical_key) noexcept {
    if (physical_key == 0) return 0;

    for (uint32_t index = 0; index < config.alphabet_count; ++index) {
        if (config.physical_keys[index] == physical_key) return config.alphabet[index];
    }

    return 0;
}

SaccadeResult HintSession::freeze(const scene::PacketView& scene, const HintConfig& config,
                                  HintSessionStorage* storage) noexcept {
    if (frozen_) {
        return SACCADE_ERROR_STATE;
    }

    if (scene.header == nullptr || scene.targets == nullptr || storage == nullptr ||
        scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 || scene.header->scene_epoch == 0 ||
        scene.header->target_count == 0 || !config_valid(config)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    const uint32_t count = scene.header->target_count;

    for (uint32_t index = 0; index < count; ++index) {
        storage->target_indices[index] = index;
    }

    const int32_t priority_x =
        config.priority == HintPriority::pointer ? config.pointer_x_q8 : config.scope_center_x_q8;
    const int32_t priority_y =
        config.priority == HintPriority::pointer ? config.pointer_y_q8 : config.scope_center_y_q8;
    std::sort(storage->target_indices.begin(), storage->target_indices.begin() + count,
              [&](uint32_t left, uint32_t right) noexcept {
                  const SaccadeTargetRecord& a = scene.targets[left];
                  const SaccadeTargetRecord& b = scene.targets[right];
                  uint64_t a_priority = a.order;
                  uint64_t b_priority = b.order;
                  if (config.priority == HintPriority::pointer || config.priority == HintPriority::scope_center) {
                      a_priority = distance(a, priority_x, priority_y);
                      b_priority = distance(b, priority_x, priority_y);
                  } else if (config.priority == HintPriority::randomized) {
                      a_priority = splitmix64(a.target_id ^ config.random_seed);
                      b_priority = splitmix64(b.target_id ^ config.random_seed);
                  }
                  return a_priority != b_priority ? a_priority < b_priority : a.target_id < b.target_id;
              });

    uint32_t depth = 1;
    uint64_t capacity = config.alphabet_count;

    while (capacity <= std::numeric_limits<uint64_t>::max() / config.alphabet_count &&
           capacity * config.alphabet_count <= count) {
        capacity *= config.alphabet_count;
        ++depth;
    }

    const uint64_t needed = count > capacity ? count - capacity : 0;
    const uint64_t full_expansions = needed / (config.alphabet_count - 1U);
    const uint32_t remainder = static_cast<uint32_t>(needed % (config.alphabet_count - 1U));
    const uint64_t expanded_parents = full_expansions + (remainder != 0 ? 1U : 0U);
    const uint64_t shortest_count = capacity - expanded_parents;
    const uint32_t partial_children = remainder == 0 ? 0 : remainder + 1U;

    for (uint32_t rank = 0; rank < count; ++rank) {
        assign_label(rank, storage->target_indices[rank], scene, config, depth, shortest_count, partial_children,
                     &storage->labels[rank]);
    }

    storage_ = storage;
    label_count_ = count;
    scene_epoch_ = scene.header->scene_epoch;
    transform_epoch_ = scene.header->transform_epoch;
    topology_epoch_ = scene.header->topology_epoch;
    frozen_ = true;

    ++stats_.freezes;
    stats_.targets_labeled += count;

    return SACCADE_OK;
}

SaccadeResult HintSession::refresh_scene(const scene::PacketView& scene) noexcept {
    if (!frozen_ || scene.header == nullptr || scene.targets == nullptr ||
        scene.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        scene.header->scene_epoch <= scene_epoch_ || scene.header->transform_epoch != transform_epoch_ ||
        scene.header->topology_epoch != topology_epoch_) {
        ++stats_.refresh_failures;
        return SACCADE_ERROR_STALE_HANDLE;
    }

    for (HintTargetBinding& binding : storage_->target_bindings)
        binding.target_id = 0;
    constexpr uint32_t binding_mask = hint_target_binding_capacity - 1U;
    for (uint32_t target_index = 0; target_index < scene.header->target_count; ++target_index) {
        const uint64_t target_id = scene.targets[target_index].target_id;
        uint32_t slot = static_cast<uint32_t>(splitmix64(target_id)) & binding_mask;
        while (storage_->target_bindings[slot].target_id != 0)
            slot = (slot + 1U) & binding_mask;
        storage_->target_bindings[slot] = {target_id, target_index, 0};
    }

    for (uint32_t label_index = 0; label_index < label_count_; ++label_index) {
        const uint64_t target_id = storage_->labels[label_index].target_id;
        uint32_t slot = static_cast<uint32_t>(splitmix64(target_id)) & binding_mask;
        while (storage_->target_bindings[slot].target_id != 0 &&
               storage_->target_bindings[slot].target_id != target_id) {
            slot = (slot + 1U) & binding_mask;
        }
        if (storage_->target_bindings[slot].target_id == 0) {
            ++stats_.refresh_failures;
            return SACCADE_ERROR_STALE_HANDLE;
        }
        storage_->target_indices[label_index] = storage_->target_bindings[slot].target_index;
    }

    for (uint32_t label_index = 0; label_index < label_count_; ++label_index)
        storage_->labels[label_index].target_index = storage_->target_indices[label_index];
    scene_epoch_ = scene.header->scene_epoch;
    ++stats_.scene_refreshes;
    return SACCADE_OK;
}

SaccadeResult HintSession::resolve_prefix(const uint16_t* symbols, uint32_t symbol_count, HintMatch* output) noexcept {
    if (!frozen_ || symbols == nullptr || symbol_count == 0 || symbol_count > maximum_hint_symbols ||
        output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *output = {};
    ++stats_.prefix_queries;

    uint64_t comparisons = 0;

    for (uint32_t index = 0; index < label_count_; ++index) {
        const HintLabel& label = storage_->labels[index];
        if (!prefix_matches(label, symbols, symbol_count, &comparisons)) {
            continue;
        }
        ++output->candidate_count;
        if (label.symbol_count == symbol_count) {
            output->target_id = label.target_id;
            output->target_index = label.target_index;
            output->exact = true;
        }
    }

    stats_.symbols_compared += comparisons;

    return output->candidate_count == 0 ? SACCADE_ERROR_NOT_FOUND : SACCADE_OK;
}

SaccadeResult HintSession::label_for_target(uint64_t target_id, const HintLabel** output) const noexcept {
    if (!frozen_ || target_id == 0 || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    *output = nullptr;

    for (uint32_t index = 0; index < label_count_; ++index) {
        if (storage_->labels[index].target_id == target_id) {
            *output = &storage_->labels[index];
            return SACCADE_OK;
        }
    }

    return SACCADE_ERROR_NOT_FOUND;
}

SaccadeResult HintSession::cancel() noexcept {
    if (!frozen_) {
        return SACCADE_ERROR_STATE;
    }

    storage_ = nullptr;
    scene_epoch_ = 0;
    transform_epoch_ = 0;
    topology_epoch_ = 0;
    label_count_ = 0;
    frozen_ = false;

    return SACCADE_OK;
}

const HintLabel* HintSession::labels() const noexcept {
    return frozen_ ? storage_->labels.data() : nullptr;
}

} // namespace saccade::interaction
