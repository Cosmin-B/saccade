#include <metal_stdlib>

using namespace metal;

struct DenseCandidate {
    ushort x_q3;
    ushort y_q3;
    ushort width_q3;
    ushort height_q3;
    ushort confidence_q16;
    uchar role;
    uchar source_bits;
    ushort flags;
    ushort reserved;
};

struct RadixEntry {
    ulong key;
    uint candidate_index;
    uint reserved;
};

struct PostprocessParameters {
    uint candidate_count;
    uint maximum_targets;
    uint block_count;
    uint mask_word_count;
    uint minimum_confidence_q16;
    uint band_minimum_confidence_q16;
    uint band_min_short_side_q3;
    uint band_max_short_side_q3;
    uint iou_threshold_q16;
    uint coordinate_space;
    uint radix_shift;
    ulong frame_id;
    ulong model_epoch;
    ulong session_epoch;
    ulong transform_epoch;
    ulong topology_epoch;
    ulong source_id;
};

struct TargetPacketHeader {
    uint struct_size;
    uint packet_version;
    uint target_count;
    uint target_stride;
    uint flags;
    uint coordinate_space;
    ulong scene_epoch;
    ulong frame_id;
    ulong model_epoch;
    ulong session_epoch;
    ulong transform_epoch;
    ulong topology_epoch;
    ulong source_id;
    ulong targets_offset;
    ulong total_size;
};

struct TargetRecord {
    ulong target_id;
    ulong parent_id;
    ulong window_id;
    ulong display_id;
    int x_q8;
    int y_q8;
    int width_q8;
    int height_q8;
    int safe_x_q8;
    int safe_y_q8;
    uint confidence_q16;
    ushort role;
    ushort source_bits;
    uint capability_bits;
    uint flags;
    uint order;
    uint reserved;
};

struct PostprocessCounters {
    uint candidates_above_threshold;
    uint targets_written;
    uint containment_suppressed;
    uint iou_suppressed;
};

constant uint radix_bins = 16;
constant ulong invalid_key = ~ulong(0);
constant uint target_actionable = 1u << 0;
constant uint target_disabled = 1u << 1;
constant uint target_secure = 1u << 3;
constant uint safe_inset_q3 = 8;
constant uint capability_pointer_move = 1u << 0;
constant uint capability_button = 1u << 1;
constant uint capability_scroll = 1u << 2;
constant uint capability_drag_source = 1u << 3;
constant uint capability_drop_target = 1u << 4;
constant uint capability_text = 1u << 5;
constant uint capability_invoke = 1u << 6;
constant uint capability_window_activate = 1u << 7;
constant uint capability_text_select = 1u << 8;
constant uint generic_visual_capabilities = capability_pointer_move | capability_button | capability_scroll |
                                            capability_drag_source | capability_drop_target | capability_text |
                                            capability_text_select;
constant uint role_capabilities[11] = {generic_visual_capabilities,
                                       capability_pointer_move | capability_button | capability_invoke,
                                       capability_pointer_move | capability_button | capability_invoke,
                                       capability_pointer_move | capability_text_select,
                                       capability_pointer_move | capability_button | capability_text |
                                           capability_text_select,
                                       capability_pointer_move | capability_button | capability_invoke,
                                       capability_pointer_move | capability_button | capability_invoke,
                                       capability_pointer_move | capability_button | capability_invoke,
                                       capability_pointer_move | capability_button | capability_scroll,
                                       capability_pointer_move,
                                       capability_pointer_move | capability_window_activate};

bool above_confidence_threshold(const DenseCandidate candidate, constant PostprocessParameters& parameters) {
    if (candidate.confidence_q16 >= parameters.minimum_confidence_q16) {
        return true;
    }
    const uint short_side = min(uint(candidate.width_q3), uint(candidate.height_q3));
    return parameters.band_minimum_confidence_q16 != 0 && short_side >= parameters.band_min_short_side_q3 &&
           short_side < parameters.band_max_short_side_q3 &&
           candidate.confidence_q16 >= parameters.band_minimum_confidence_q16;
}

kernel void saccade_targets_prepare(device const DenseCandidate* candidates [[buffer(0)]],
                                    constant PostprocessParameters& parameters [[buffer(1)]],
                                    device RadixEntry* entries [[buffer(2)]], uint index [[thread_position_in_grid]]) {
    const DenseCandidate candidate = candidates[index];
    const ulong confidence = ulong(ushort(~candidate.confidence_q16));
    const ulong key =
        !above_confidence_threshold(candidate, parameters)
            ? invalid_key
            : (confidence << 48) | (ulong(candidate.y_q3) << 32) | (ulong(candidate.x_q3) << 16) | ulong(index);
    entries[index] = {key, index, 0};
}

kernel void saccade_targets_radix_histogram(
    constant PostprocessParameters& parameters [[buffer(1)]], device const RadixEntry* entries_a [[buffer(2)]],
    device const RadixEntry* entries_b [[buffer(3)]], device uint* block_histogram [[buffer(4)]],
    device uint* local_ranks [[buffer(5)]], uint index [[thread_position_in_grid]],
    uint local_index [[thread_index_in_threadgroup]], uint block_index [[threadgroup_position_in_grid]],
    uint simd_lane [[thread_index_in_simdgroup]], uint simd_index [[simdgroup_index_in_threadgroup]],
    uint group_size [[threads_per_threadgroup]]) {
    threadgroup uint simd_counts[8][radix_bins];
    for (uint slot = local_index; slot < 8 * radix_bins; slot += group_size) {
        simd_counts[slot / radix_bins][slot % radix_bins] = 0;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    const uint pass = parameters.radix_shift >> 2;
    const RadixEntry entry = (pass & 1) == 0 ? entries_a[index] : entries_b[index];
    const uint digit = uint((entry.key >> parameters.radix_shift) & 15);
    uint rank = 0;
    for (uint bin = 0; bin < radix_bins; ++bin) {
        const uint member = digit == bin;
        const uint prefix = simd_prefix_exclusive_sum(member);
        const uint count = simd_sum(member);
        if (member != 0) {
            rank = prefix;
        }
        if (simd_lane == 0) {
            simd_counts[simd_index][bin] = count;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (uint prior = 0; prior < simd_index; ++prior) {
        rank += simd_counts[prior][digit];
    }
    local_ranks[index] = rank;
    for (uint bin = local_index; bin < radix_bins; bin += group_size) {
        uint count = 0;
        for (uint group = 0; group < 8; ++group) {
            count += simd_counts[group][bin];
        }
        block_histogram[block_index * radix_bins + bin] = count;
    }
}

kernel void saccade_targets_radix_scan(constant PostprocessParameters& parameters [[buffer(1)]],
                                       device uint* block_offsets [[buffer(4)]],
                                       uint digit [[thread_position_in_grid]]) {
    threadgroup uint totals[radix_bins];
    uint total = 0;
    for (uint block = 0; block < parameters.block_count; ++block) {
        total += block_offsets[block * radix_bins + digit];
    }
    totals[digit] = total;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint offset = 0;
    for (uint prior = 0; prior < digit; ++prior) {
        offset += totals[prior];
    }
    for (uint block = 0; block < parameters.block_count; ++block) {
        const uint position = block * radix_bins + digit;
        const uint count = block_offsets[position];
        block_offsets[position] = offset;
        offset += count;
    }
}

kernel void saccade_targets_radix_scatter(constant PostprocessParameters& parameters [[buffer(1)]],
                                          device RadixEntry* entries_a [[buffer(2)]],
                                          device RadixEntry* entries_b [[buffer(3)]],
                                          device const uint* block_offsets [[buffer(4)]],
                                          device const uint* local_ranks [[buffer(5)]],
                                          uint index [[thread_position_in_grid]],
                                          uint block_index [[threadgroup_position_in_grid]]) {
    const uint pass = parameters.radix_shift >> 2;
    const bool a_to_b = (pass & 1) == 0;
    const RadixEntry entry = a_to_b ? entries_a[index] : entries_b[index];
    const uint digit = uint((entry.key >> parameters.radix_shift) & 15);
    const uint output_index = block_offsets[block_index * radix_bins + digit] + local_ranks[index];
    if (a_to_b) {
        entries_b[output_index] = entry;
    } else {
        entries_a[output_index] = entry;
    }
}

ulong candidate_area(const DenseCandidate candidate) {
    return ulong(candidate.width_q3) * ulong(candidate.height_q3);
}

ulong candidate_intersection(const DenseCandidate a, const DenseCandidate b) {
    const uint left = max(uint(a.x_q3), uint(b.x_q3));
    const uint top = max(uint(a.y_q3), uint(b.y_q3));
    const uint right = min(uint(a.x_q3) + a.width_q3, uint(b.x_q3) + b.width_q3);
    const uint bottom = min(uint(a.y_q3) + a.height_q3, uint(b.y_q3) + b.height_q3);
    if (left >= right || top >= bottom) {
        return 0;
    }
    return ulong(right - left) * ulong(bottom - top);
}

kernel void saccade_targets_suppression_masks(device const DenseCandidate* candidates [[buffer(0)]],
                                              constant PostprocessParameters& parameters [[buffer(1)]],
                                              device const RadixEntry* entries [[buffer(2)]],
                                              device uint* masks [[buffer(6)]],
                                              uint index [[thread_position_in_grid]]) {
    const uint selected_count = min(parameters.candidate_count, parameters.maximum_targets);
    const uint row = index / parameters.mask_word_count;
    const uint word = index - row * parameters.mask_word_count;
    const RadixEntry row_entry = entries[row];
    const DenseCandidate earlier = candidates[row_entry.candidate_index];
    uint mask = 0;
    const uint first = word * 32;
    for (uint bit = 0; bit < 32; ++bit) {
        const uint later_index = first + bit;
        if (later_index <= row || later_index >= selected_count) {
            continue;
        }
        const RadixEntry later_entry = entries[later_index];
        const DenseCandidate later = candidates[later_entry.candidate_index];
        if (!above_confidence_threshold(later, parameters)) {
            continue;
        }
        const ulong overlap = candidate_intersection(later, earlier);
        const ulong later_area = candidate_area(later);
        const ulong union_area = later_area + candidate_area(earlier) - overlap;
        const bool contained = overlap == later_area;
        const bool iou = overlap != 0 && overlap * 65535 >= union_area * parameters.iou_threshold_q16;
        if (contained || iou) {
            mask |= 1u << bit;
        }
    }
    masks[row * parameters.mask_word_count + word] = mask;
}

ulong target_id(ulong source_id, const DenseCandidate candidate) {
    ulong hash = 14695981039346656037ul;
    const ulong values[7] = {source_id,
                             ulong(candidate.x_q3) << 5,
                             ulong(candidate.y_q3) << 5,
                             ulong(candidate.width_q3) << 5,
                             ulong(candidate.height_q3) << 5,
                             ulong(candidate.role),
                             ulong(candidate.source_bits)};
    for (uint value_index = 0; value_index < 7; ++value_index) {
        for (uint byte = 0; byte < 8; ++byte) {
            hash ^= uchar(values[value_index] >> (byte * 8));
            hash *= 1099511628211ul;
        }
    }
    return hash == 0 ? 1 : hash;
}

kernel void saccade_targets_finalize(device const DenseCandidate* candidates [[buffer(0)]],
                                     constant PostprocessParameters& parameters [[buffer(1)]],
                                     device const RadixEntry* entries [[buffer(2)]],
                                     device const uint* masks [[buffer(6)]], device uint* suppressed [[buffer(7)]],
                                     device TargetPacketHeader* header [[buffer(8)]],
                                     device PostprocessCounters* counters [[buffer(9)]]) {
    const uint selected_count = min(parameters.candidate_count, parameters.maximum_targets);
    for (uint word = 0; word < parameters.mask_word_count; ++word) {
        suppressed[word] = 0;
    }
    *counters = {};
    *header = {};
    header->struct_size = 96;
    header->packet_version = 0x00010001;
    header->target_stride = 80;
    header->coordinate_space = parameters.coordinate_space;
    header->frame_id = parameters.frame_id;
    header->model_epoch = parameters.model_epoch;
    header->session_epoch = parameters.session_epoch;
    header->transform_epoch = parameters.transform_epoch;
    header->topology_epoch = parameters.topology_epoch;
    header->source_id = parameters.source_id;
    header->targets_offset = 96;
    device TargetRecord* targets = reinterpret_cast<device TargetRecord*>(reinterpret_cast<device uchar*>(header) + 96);

    for (uint ordered = 0; ordered < selected_count; ++ordered) {
        const RadixEntry entry = entries[ordered];
        const DenseCandidate candidate = candidates[entry.candidate_index];
        if (!above_confidence_threshold(candidate, parameters)) {
            break;
        }
        ++counters->candidates_above_threshold;
        if ((suppressed[ordered >> 5] & (1u << (ordered & 31))) != 0) {
            continue;
        }
        for (uint word = 0; word < parameters.mask_word_count; ++word) {
            suppressed[word] |= masks[ordered * parameters.mask_word_count + word];
        }
        TargetRecord target = {};
        target.target_id = target_id(parameters.source_id, candidate);
        target.x_q8 = int(candidate.x_q3) << 5;
        target.y_q8 = int(candidate.y_q3) << 5;
        target.width_q8 = int(candidate.width_q3) << 5;
        target.height_q8 = int(candidate.height_q3) << 5;
        target.safe_x_q8 = target.x_q8 + target.width_q8 / 2;
        target.safe_y_q8 = target.y_q8 + target.height_q8 / 2;
        target.confidence_q16 = candidate.confidence_q16;
        target.role = candidate.role;
        target.source_bits = candidate.source_bits;
        target.flags = uint(candidate.flags) & ~target_actionable;
        const bool safe_interior = candidate.width_q3 > safe_inset_q3 * 2 && candidate.height_q3 > safe_inset_q3 * 2;
        if ((candidate.flags & target_actionable) != 0 && safe_interior &&
            (target.flags & (target_disabled | target_secure)) == 0) {
            target.capability_bits = candidate.role <= 10 ? role_capabilities[candidate.role] : 0;
            if (target.capability_bits != 0) {
                target.flags |= target_actionable;
            }
        }
        target.order = header->target_count;
        targets[header->target_count++] = target;
    }
    header->total_size = 96 + ulong(header->target_count) * 80;
    counters->targets_written = header->target_count;
}
