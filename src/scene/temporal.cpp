#include "scene/temporal.hpp"

#include <cstring>

namespace saccade::scene {
namespace {

constexpr uint8_t full_target_fields = SACCADE_SCENE_DELTA_OWNER | SACCADE_SCENE_DELTA_GEOMETRY |
                                       SACCADE_SCENE_DELTA_CONFIDENCE | SACCADE_SCENE_DELTA_CLASSIFICATION |
                                       SACCADE_SCENE_DELTA_CAPABILITIES | SACCADE_SCENE_DELTA_FLAGS |
                                       SACCADE_SCENE_DELTA_ORDER;
constexpr uint32_t temporal_hash_slot_mask = temporal_hash_slot_count - 1U;
constexpr uint32_t temporal_window_hash_slot_mask = temporal_window_hash_slot_count - 1U;

static_assert((temporal_hash_slot_count & temporal_hash_slot_mask) == 0);
static_assert((temporal_window_hash_slot_count & temporal_window_hash_slot_mask) == 0);

struct DeltaCounts {
    size_t payload_bytes = 0;
    uint32_t operations = 0;
    uint32_t additions = 0;
    uint32_t updates = 0;
    uint32_t removals = 0;
    uint32_t window_transforms = 0;
};

uint64_t mix(uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= UINT64_C(0xFF51AFD7ED558CCD);
    value ^= value >> 33U;
    value *= UINT64_C(0xC4CEB9FE1A85EC53);
    value ^= value >> 33U;
    return value;
}

uint32_t target_hash(uint64_t target_id, uint64_t window_id) noexcept {
    return static_cast<uint32_t>(mix(target_id ^ mix(window_id))) & temporal_hash_slot_mask;
}

uint32_t window_hash(uint64_t window_id) noexcept {
    return static_cast<uint32_t>(mix(window_id)) & temporal_window_hash_slot_mask;
}

bool same_owner(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    return left.parent_id == right.parent_id && left.display_id == right.display_id;
}

bool same_geometry(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    return left.x_q8 == right.x_q8 && left.y_q8 == right.y_q8 && left.width_q8 == right.width_q8 &&
           left.height_q8 == right.height_q8 && left.safe_x_q8 == right.safe_x_q8 && left.safe_y_q8 == right.safe_y_q8;
}

bool same_classification(const SaccadeTargetRecord& left, const SaccadeTargetRecord& right) noexcept {
    return left.role == right.role && left.source_bits == right.source_bits;
}

SaccadeSpanU8 previous_text(const TemporalStorage& storage, const SaccadeTargetRecord& target) noexcept {
    return target.text.size == 0 ? SaccadeSpanU8{}
                                 : SaccadeSpanU8{storage.text.data() + target.text.offset, target.text.size};
}

bool same_text(SaccadeSpanU8 left, SaccadeSpanU8 right) noexcept {
    return left.size == right.size &&
           (left.size == 0 || std::memcmp(left.data, right.data, static_cast<size_t>(left.size)) == 0);
}

uint8_t changed_fields(const SaccadeTargetRecord& previous, SaccadeSpanU8 previous_target_text,
                       const SaccadeTargetRecord& current, SaccadeSpanU8 current_target_text) noexcept {
    uint8_t changed = 0;
    if (!same_owner(previous, current)) changed |= SACCADE_SCENE_DELTA_OWNER;
    if (!same_geometry(previous, current)) changed |= SACCADE_SCENE_DELTA_GEOMETRY;
    if (previous.confidence_q16 != current.confidence_q16) changed |= SACCADE_SCENE_DELTA_CONFIDENCE;
    if (!same_classification(previous, current)) changed |= SACCADE_SCENE_DELTA_CLASSIFICATION;
    if (previous.capability_bits != current.capability_bits) changed |= SACCADE_SCENE_DELTA_CAPABILITIES;
    if (previous.flags != current.flags) changed |= SACCADE_SCENE_DELTA_FLAGS;
    if (previous.order != current.order) changed |= SACCADE_SCENE_DELTA_ORDER;
    if (!same_text(previous_target_text, current_target_text)) changed |= SACCADE_SCENE_DELTA_TEXT;
    return changed;
}

size_t payload_size(uint8_t fields, size_t text_size) noexcept {
    size_t size = 0;
    if ((fields & SACCADE_SCENE_DELTA_OWNER) != 0) size += sizeof(SaccadeSceneDeltaOwner);
    if ((fields & SACCADE_SCENE_DELTA_GEOMETRY) != 0) size += sizeof(SaccadeSceneDeltaGeometry);
    if ((fields & SACCADE_SCENE_DELTA_CONFIDENCE) != 0) size += sizeof(uint32_t);
    if ((fields & SACCADE_SCENE_DELTA_CLASSIFICATION) != 0) size += sizeof(SaccadeSceneDeltaClassification);
    if ((fields & SACCADE_SCENE_DELTA_CAPABILITIES) != 0) size += sizeof(uint32_t);
    if ((fields & SACCADE_SCENE_DELTA_FLAGS) != 0) size += sizeof(uint32_t);
    if ((fields & SACCADE_SCENE_DELTA_ORDER) != 0) size += sizeof(uint32_t);
    if ((fields & SACCADE_SCENE_DELTA_TEXT) != 0) size += text_size;
    return size;
}

uint32_t delta_flags(const SaccadeTargetPacketHeader& current, bool baseline, bool reset) noexcept {
    uint32_t flags = baseline ? SACCADE_SCENE_DELTA_BASELINE : 0;
    if (reset) flags |= SACCADE_SCENE_DELTA_RESET;
    if ((current.flags & SACCADE_TARGET_PACKET_INCOMPLETE) != 0) flags |= SACCADE_SCENE_DELTA_SOURCE_INCOMPLETE;
    if ((current.flags & SACCADE_TARGET_PACKET_TEXT_TRUNCATED) != 0) flags |= SACCADE_SCENE_DELTA_SOURCE_TEXT_TRUNCATED;
    return flags;
}

SaccadeSceneDeltaHeader make_header(const SaccadeTargetPacketHeader& previous, const SaccadeTargetPacketHeader& current,
                                    uint32_t flags, uint32_t operation_count, size_t payload_bytes) noexcept {
    SaccadeSceneDeltaHeader header{};
    header.struct_size = sizeof(header);
    header.delta_version = SACCADE_SCENE_DELTA_VERSION;
    header.operation_count = operation_count;
    header.operation_stride = sizeof(SaccadeSceneDeltaRecord);
    header.flags = flags;
    header.coordinate_space = current.coordinate_space;
    header.operations_offset = sizeof(header);
    header.payload_offset = header.operations_offset + operation_count * sizeof(SaccadeSceneDeltaRecord);
    header.payload_size = static_cast<uint32_t>(payload_bytes);
    header.total_size = header.payload_offset + header.payload_size;
    header.previous_scene_epoch = previous.scene_epoch;
    header.scene_epoch = current.scene_epoch;
    header.previous_frame_id = previous.frame_id;
    header.frame_id = current.frame_id;
    header.previous_session_epoch = previous.session_epoch;
    header.session_epoch = current.session_epoch;
    header.previous_transform_epoch = previous.transform_epoch;
    header.transform_epoch = current.transform_epoch;
    header.previous_topology_epoch = previous.topology_epoch;
    header.topology_epoch = current.topology_epoch;
    header.capture_time_ns = current.capture_time_ns;
    header.model_epoch = current.model_epoch;
    header.source_id = current.source_id;
    return header;
}

uint8_t* write_bytes(uint8_t* output, const void* source, size_t size) noexcept {
    std::memcpy(output, source, size);
    return output + size;
}

uint8_t* write_payload(uint8_t* output, const SaccadeTargetRecord& target, SaccadeSpanU8 text,
                       uint8_t fields) noexcept {
    if ((fields & SACCADE_SCENE_DELTA_OWNER) != 0) {
        const SaccadeSceneDeltaOwner owner{target.parent_id, target.display_id};
        output = write_bytes(output, &owner, sizeof(owner));
    }
    if ((fields & SACCADE_SCENE_DELTA_GEOMETRY) != 0) {
        const SaccadeSceneDeltaGeometry geometry{target.x_q8,      target.y_q8,      target.width_q8,
                                                 target.height_q8, target.safe_x_q8, target.safe_y_q8};
        output = write_bytes(output, &geometry, sizeof(geometry));
    }
    if ((fields & SACCADE_SCENE_DELTA_CONFIDENCE) != 0)
        output = write_bytes(output, &target.confidence_q16, sizeof(target.confidence_q16));
    if ((fields & SACCADE_SCENE_DELTA_CLASSIFICATION) != 0) {
        const SaccadeSceneDeltaClassification classification{target.role, target.source_bits};
        output = write_bytes(output, &classification, sizeof(classification));
    }
    if ((fields & SACCADE_SCENE_DELTA_CAPABILITIES) != 0)
        output = write_bytes(output, &target.capability_bits, sizeof(target.capability_bits));
    if ((fields & SACCADE_SCENE_DELTA_FLAGS) != 0) output = write_bytes(output, &target.flags, sizeof(target.flags));
    if ((fields & SACCADE_SCENE_DELTA_ORDER) != 0) output = write_bytes(output, &target.order, sizeof(target.order));
    if ((fields & SACCADE_SCENE_DELTA_TEXT) != 0 && text.size != 0) output = write_bytes(output, text.data, text.size);
    return output;
}

void write_record(SaccadeSceneDeltaRecord* record, uint8_t** payload, const SaccadeTargetRecord& target,
                  SaccadeSpanU8 text, uint8_t fields, SaccadeSceneDeltaOperation operation,
                  const uint8_t* batch) noexcept {
    record->target_id = target.target_id;
    record->window_id = target.window_id;
    record->changed_fields = fields;
    record->operation = operation;
    if (operation == SACCADE_SCENE_DELTA_REMOVE) {
        record->payload_offset = 0;
        record->payload_size = 0;
        return;
    }
    const uint8_t* begin = *payload;
    *payload = write_payload(*payload, target, text, fields);
    record->payload_offset = static_cast<uint32_t>(begin - batch);
    record->payload_size = static_cast<uint16_t>(*payload - begin);
}

} // namespace

SaccadeResult TemporalCompiler::initialize(TemporalStorage* storage) noexcept {
    if (initialized_) {
        return SACCADE_ERROR_ALREADY_EXISTS;
    }
    if (storage == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    storage_ = storage;
    storage_->hash_slots.fill(0);
    storage_->window_hash_slots.fill(0);
    previous_header_ = {};
    previous_target_count_ = 0;
    previous_text_size_ = 0;
    initialized_ = true;
    return SACCADE_OK;
}

uint16_t TemporalCompiler::find_target(uint64_t target_id, uint64_t window_id, TemporalStats* stats) const noexcept {
    ++stats->target_lookups;
    uint32_t slot = target_hash(target_id, window_id);
    for (;;) {
        ++stats->hash_probes;
        const uint16_t entry = storage_->hash_slots[slot];
        if (entry == 0) {
            return missing_target_;
        }
        const uint16_t index = static_cast<uint16_t>(entry - 1U);
        if (storage_->targets[index].target_id == target_id && storage_->targets[index].window_id == window_id) {
            return index;
        }
        slot = (slot + 1U) & temporal_hash_slot_mask;
    }
}

void TemporalCompiler::insert_target(uint32_t target_index) noexcept {
    const SaccadeTargetRecord& target = storage_->targets[target_index];
    uint32_t slot = target_hash(target.target_id, target.window_id);
    while (storage_->hash_slots[slot] != 0) {
        slot = (slot + 1U) & temporal_hash_slot_mask;
    }
    storage_->hash_slots[slot] = static_cast<uint16_t>(target_index + 1U);
}

uint16_t TemporalCompiler::find_window_transform(uint64_t window_id) const noexcept {
    if (window_id == 0 || window_transform_count_ == 0) {
        return missing_target_;
    }
    uint32_t slot = window_hash(window_id);
    for (;;) {
        const uint16_t entry = storage_->window_hash_slots[slot];
        if (entry == 0) {
            return missing_target_;
        }
        const uint16_t index = static_cast<uint16_t>(entry - 1U);
        if (storage_->window_transforms[index].window_id == window_id) {
            return index;
        }
        slot = (slot + 1U) & temporal_window_hash_slot_mask;
    }
}

void TemporalCompiler::build_window_transforms(const PacketView& current, TemporalStats* stats) noexcept {
    window_transform_count_ = 0;
    if (!has_previous_ || current.header->session_epoch != previous_header_.session_epoch ||
        current.header->transform_epoch == previous_header_.transform_epoch) {
        return;
    }
    storage_->window_hash_slots.fill(0);

    for (uint32_t index = 0; index < current.header->target_count; ++index) {
        const SaccadeTargetRecord& target = current.targets[index];
        if (target.window_id == 0) {
            continue;
        }
        const uint16_t previous_index = find_target(target.target_id, target.window_id, stats);
        if (previous_index == missing_target_) {
            continue;
        }
        const SaccadeTargetRecord& previous = storage_->targets[previous_index];
        uint16_t transform_index = find_window_transform(target.window_id);
        if (transform_index == missing_target_) {
            transform_index = static_cast<uint16_t>(window_transform_count_++);
            TemporalWindowTransform& transform = storage_->window_transforms[transform_index];
            transform = {};
            transform.window_id = target.window_id;
            transform.display_id = target.display_id;
            transform.translation_x_q8 = target.x_q8 - previous.x_q8;
            transform.translation_y_q8 = target.y_q8 - previous.y_q8;
            transform.valid = true;
            uint32_t slot = window_hash(target.window_id);
            while (storage_->window_hash_slots[slot] != 0) {
                slot = (slot + 1U) & temporal_window_hash_slot_mask;
            }
            storage_->window_hash_slots[slot] = static_cast<uint16_t>(transform_index + 1U);
        }

        TemporalWindowTransform& transform = storage_->window_transforms[transform_index];
        const int32_t translation_x = target.x_q8 - previous.x_q8;
        const int32_t translation_y = target.y_q8 - previous.y_q8;
        transform.valid &= translation_x == transform.translation_x_q8 && translation_y == transform.translation_y_q8 &&
                           target.safe_x_q8 - previous.safe_x_q8 == transform.translation_x_q8 &&
                           target.safe_y_q8 - previous.safe_y_q8 == transform.translation_y_q8 &&
                           target.width_q8 == previous.width_q8 && target.height_q8 == previous.height_q8;
        transform.window_role |= target.role == SACCADE_TARGET_ROLE_WINDOW;
        transform.flags |=
            target.display_id != previous.display_id ? SACCADE_SCENE_WINDOW_TRANSFORM_DISPLAY_CHANGED : 0;
        if (target.display_id != transform.display_id) {
            transform.display_id = 0;
        }
        ++transform.target_count;
    }

    for (uint32_t index = 0; index < window_transform_count_; ++index) {
        TemporalWindowTransform& transform = storage_->window_transforms[index];
        transform.valid &= transform.target_count >= 2 || transform.window_role;
        transform.valid &= transform.translation_x_q8 != 0 || transform.translation_y_q8 != 0 ||
                           (transform.flags & SACCADE_SCENE_WINDOW_TRANSFORM_DISPLAY_CHANGED) != 0;
        stats->window_transforms += transform.valid;
    }
}

void TemporalCompiler::adopt(const PacketView& current) noexcept {
    previous_header_ = *current.header;
    previous_target_count_ = current.header->target_count;
    previous_text_size_ = current.text_size;
    std::memcpy(storage_->targets.data(), current.targets,
                static_cast<size_t>(previous_target_count_) * sizeof(SaccadeTargetRecord));
    if (previous_text_size_ != 0) {
        std::memcpy(storage_->text.data(), current.text, previous_text_size_);
    }
    storage_->hash_slots.fill(0);
    for (uint32_t index = 0; index < previous_target_count_; ++index) {
        insert_target(index);
    }
    has_previous_ = true;
}

SaccadeResult TemporalCompiler::compile(const PacketView& current, SaccadeMutableSpanU8 output, size_t* required,
                                        TemporalStats* stats) noexcept {
    if (!initialized_ || current.header == nullptr || current.targets == nullptr || required == nullptr ||
        stats == nullptr || current.header->coordinate_space != SACCADE_COORDINATE_SPACE_DESKTOP_Q8 ||
        current.header->target_count > SACCADE_TARGET_PACKET_MAX_TARGETS) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (has_previous_ && current.header->session_epoch == previous_header_.session_epoch &&
        current.header->scene_epoch <= previous_header_.scene_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }

    *stats = {};
    stats->previous_targets = previous_target_count_;
    stats->current_targets = current.header->target_count;
    const bool baseline = !has_previous_;
    const bool reset = has_previous_ && current.header->session_epoch != previous_header_.session_epoch;

    stats->transform_changes =
        !baseline && current.header->transform_epoch != previous_header_.transform_epoch ? 1U : 0U;
    stats->topology_changes = !baseline && current.header->topology_epoch != previous_header_.topology_epoch ? 1U : 0U;
    build_window_transforms(current, stats);

    DeltaCounts counts{};
    if (reset) {
        counts.removals = previous_target_count_;
        counts.additions = current.header->target_count;
        counts.operations = counts.removals + counts.additions;
        for (uint32_t index = 0; index < current.header->target_count; ++index) {
            const SaccadeSpanU8 text = current.target_text(index);
            counts.payload_bytes +=
                payload_size(full_target_fields | (text.size != 0 ? SACCADE_SCENE_DELTA_TEXT : 0), text.size);
        }
    } else {
        std::memset(storage_->seen.data(), 0, previous_target_count_);
        for (uint32_t index = 0; index < current.header->target_count; ++index) {
            const SaccadeTargetRecord& target = current.targets[index];
            const SaccadeSpanU8 text = current.target_text(index);
            const uint16_t previous_index = find_target(target.target_id, target.window_id, stats);
            if (previous_index == missing_target_) {
                const uint8_t fields = full_target_fields | (text.size != 0 ? SACCADE_SCENE_DELTA_TEXT : 0);
                ++counts.additions;
                counts.payload_bytes += payload_size(fields, text.size);
                continue;
            }
            storage_->seen[previous_index] = 1;
            const SaccadeTargetRecord& previous = storage_->targets[previous_index];
            uint8_t fields = changed_fields(previous, previous_text(*storage_, previous), target, text);
            const uint16_t transform_index = find_window_transform(target.window_id);
            if (transform_index != missing_target_ && storage_->window_transforms[transform_index].valid) {
                fields &= static_cast<uint8_t>(~SACCADE_SCENE_DELTA_GEOMETRY);
            }
            if (fields != 0) {
                ++counts.updates;
                counts.payload_bytes += payload_size(fields, text.size);
            }
        }
        for (uint32_t index = 0; index < previous_target_count_; ++index) {
            counts.removals += storage_->seen[index] == 0;
        }
        counts.window_transforms = stats->window_transforms;
        counts.operations = counts.additions + counts.updates + counts.removals + counts.window_transforms;
        counts.payload_bytes += static_cast<size_t>(counts.window_transforms) * sizeof(SaccadeSceneWindowTransform);
    }

    const size_t total_size = sizeof(SaccadeSceneDeltaHeader) +
                              static_cast<size_t>(counts.operations) * sizeof(SaccadeSceneDeltaRecord) +
                              counts.payload_bytes;
    *required = total_size;
    if (output.data == nullptr || output.size < total_size ||
        (reinterpret_cast<uintptr_t>(output.data) & (alignof(SaccadeSceneDeltaHeader) - 1U)) != 0) {
        return SACCADE_ERROR_CAPACITY;
    }

    uint32_t flags = delta_flags(*current.header, baseline, reset);
    if (counts.window_transforms != 0) flags |= SACCADE_SCENE_DELTA_WINDOW_TRANSFORMS;
    if (stats->transform_changes != 0) flags |= SACCADE_SCENE_DELTA_TRANSFORM_CHANGED;
    if (stats->topology_changes != 0) flags |= SACCADE_SCENE_DELTA_TOPOLOGY_CHANGED;
    SaccadeSceneDeltaHeader header =
        make_header(previous_header_, *current.header, flags, counts.operations, counts.payload_bytes);
    auto* records = reinterpret_cast<SaccadeSceneDeltaRecord*>(output.data + header.operations_offset);
    uint8_t* payload = output.data + header.payload_offset;
    uint32_t operation_index = 0;

    if (reset) {
        for (uint32_t index = 0; index < previous_target_count_; ++index) {
            write_record(&records[operation_index++], &payload, storage_->targets[index], {}, 0,
                         SACCADE_SCENE_DELTA_REMOVE, output.data);
        }
        for (uint32_t index = 0; index < current.header->target_count; ++index) {
            const SaccadeSpanU8 text = current.target_text(index);
            const uint8_t fields = full_target_fields | (text.size != 0 ? SACCADE_SCENE_DELTA_TEXT : 0);
            write_record(&records[operation_index++], &payload, current.targets[index], text, fields,
                         SACCADE_SCENE_DELTA_ADD, output.data);
        }
    } else {
        for (uint32_t index = 0; index < window_transform_count_; ++index) {
            const TemporalWindowTransform& transform = storage_->window_transforms[index];
            if (!transform.valid) {
                continue;
            }
            const SaccadeSceneWindowTransform payload_value{transform.display_id, transform.translation_x_q8,
                                                            transform.translation_y_q8, transform.target_count,
                                                            transform.flags};
            SaccadeSceneDeltaRecord& record = records[operation_index++];
            record = {};
            record.window_id = transform.window_id;
            record.changed_fields = SACCADE_SCENE_DELTA_GEOMETRY;
            record.operation = SACCADE_SCENE_DELTA_WINDOW_TRANSFORM;
            record.payload_offset = static_cast<uint32_t>(payload - output.data);
            record.payload_size = sizeof(payload_value);
            payload = write_bytes(payload, &payload_value, sizeof(payload_value));
        }
        for (uint32_t index = 0; index < current.header->target_count; ++index) {
            const SaccadeTargetRecord& target = current.targets[index];
            const SaccadeSpanU8 text = current.target_text(index);
            const uint16_t previous_index = find_target(target.target_id, target.window_id, stats);
            if (previous_index == missing_target_) {
                const uint8_t fields = full_target_fields | (text.size != 0 ? SACCADE_SCENE_DELTA_TEXT : 0);
                write_record(&records[operation_index++], &payload, target, text, fields, SACCADE_SCENE_DELTA_ADD,
                             output.data);
                continue;
            }
            const SaccadeTargetRecord& previous = storage_->targets[previous_index];
            uint8_t fields = changed_fields(previous, previous_text(*storage_, previous), target, text);
            const uint16_t transform_index = find_window_transform(target.window_id);
            if (transform_index != missing_target_ && storage_->window_transforms[transform_index].valid) {
                fields &= static_cast<uint8_t>(~SACCADE_SCENE_DELTA_GEOMETRY);
            }
            if (fields != 0) {
                write_record(&records[operation_index++], &payload, target, text, fields, SACCADE_SCENE_DELTA_UPDATE,
                             output.data);
            }
        }
        for (uint32_t index = 0; index < previous_target_count_; ++index) {
            if (storage_->seen[index] == 0) {
                write_record(&records[operation_index++], &payload, storage_->targets[index], {}, 0,
                             SACCADE_SCENE_DELTA_REMOVE, output.data);
            }
        }
    }

    std::memcpy(output.data, &header, sizeof(header));
    stats->payload_bytes = counts.payload_bytes;
    stats->operations = counts.operations;
    stats->additions = counts.additions;
    stats->updates = counts.updates;
    stats->removals = counts.removals;
    adopt(current);
    return SACCADE_OK;
}

} // namespace saccade::scene
