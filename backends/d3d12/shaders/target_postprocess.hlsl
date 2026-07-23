struct Parameters {
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
    uint model_width;
    uint model_height;
    uint source_width;
    uint source_height;
    uint64_t frame_id;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
};

struct Candidate {
    uint x;
    uint y;
    uint width;
    uint height;
    uint confidence;
    uint role;
    uint source_bits;
    uint flags;
};

struct RadixEntry {
    uint64_t key;
    uint candidate_index;
    uint reserved;
};

ByteAddressBuffer candidates : register(t0);
ConstantBuffer<Parameters> parameters : register(b0);
RWStructuredBuffer<RadixEntry> entries_a : register(u0);
RWStructuredBuffer<RadixEntry> entries_b : register(u1);
RWStructuredBuffer<uint> block_offsets : register(u2);
RWStructuredBuffer<uint> local_ranks : register(u3);
RWStructuredBuffer<uint> masks : register(u4);
RWStructuredBuffer<uint> suppressed : register(u5);
RWByteAddressBuffer packet : register(u6);
RWStructuredBuffer<uint> counters : register(u7);
RWByteAddressBuffer packed_candidates : register(u8);

static const uint radix_bins = 16;
static const uint64_t invalid_key = uint64_t(-1);
static const uint target_actionable = 1u << 0;
static const uint target_disabled = 1u << 1;
static const uint target_secure = 1u << 3;
static const uint safe_inset_q3 = 8;

Candidate load_candidate(uint index) {
    const uint4 words = candidates.Load4(index * 16);
    Candidate value;
    value.x = words.x & 0xffff;
    value.y = words.x >> 16;
    value.width = words.y & 0xffff;
    value.height = words.y >> 16;
    value.confidence = words.z & 0xffff;
    value.role = (words.z >> 16) & 0xff;
    value.source_bits = words.z >> 24;
    value.flags = words.w & 0xffff;
    return value;
}

float load_half(uint byte_offset) {
    const uint word = candidates.Load(byte_offset & ~3u);
    const uint bits = (byte_offset & 2u) == 0 ? word & 0xffff : word >> 16;
    return f16tof32(bits);
}

uint quantize_q3(float value) {
    return (uint)round(clamp(value * 8.0, 0.0, 65535.0));
}

[numthreads(256, 1, 1)] void targets_pack_normalized(uint index : SV_DispatchThreadID) {
    if (index >= parameters.candidate_count) return;
    const uint offset = index * 12;
    const float x = load_half(offset);
    const float y = load_half(offset + 2);
    const float width = load_half(offset + 4);
    const float height = load_half(offset + 6);
    const float confidence = load_half(offset + 8);
    const float role = load_half(offset + 10);
    const float scale = min((float)parameters.model_width / parameters.source_width,
                            (float)parameters.model_height / parameters.source_height);
    const float2 content_size =
        max(float2(1.0, 1.0), floor(float2(parameters.source_width, parameters.source_height) * scale));
    const float2 content_origin = floor((float2(parameters.model_width, parameters.model_height) - content_size) * 0.5);
    const float2 canvas_size = float2(parameters.model_width, parameters.model_height);
    const float2 source_size = float2(parameters.source_width, parameters.source_height);
    const float2 begin = clamp((float2(x, y) * canvas_size - content_origin) / content_size, 0.0, 1.0) * source_size;
    const float2 end =
        clamp((float2(x + width, y + height) * canvas_size - content_origin) / content_size, 0.0, 1.0) * source_size;
    const uint qx = quantize_q3(begin.x);
    const uint qy = quantize_q3(begin.y);
    const uint qw = quantize_q3(max(0.0, end.x - begin.x));
    const uint qh = quantize_q3(max(0.0, end.y - begin.y));
    const uint qconfidence = (uint)round(saturate(confidence) * 65535.0);
    const uint qrole = (uint)round(clamp(role, 0.0, 10.0));
    packed_candidates.Store4(index * 16,
                             uint4(qx | (qy << 16), qw | (qh << 16), qconfidence | (qrole << 16) | (1u << 24), 1u));
}

uint capabilities_for_role(uint role) {
    const uint pointer_move = 1u << 0;
    const uint button = 1u << 1;
    const uint scroll = 1u << 2;
    const uint drag_source = 1u << 3;
    const uint drop_target = 1u << 4;
    const uint text = 1u << 5;
    const uint invoke = 1u << 6;
    const uint window_activate = 1u << 7;
    const uint text_select = 1u << 8;
    switch (role) {
    case 1:
    case 2:
    case 5:
    case 6:
    case 7:
        return pointer_move | button | invoke;
    case 3:
        return pointer_move | text_select;
    case 4:
        return pointer_move | button | text | text_select;
    case 8:
        return pointer_move | button | scroll;
    case 10:
        return pointer_move | window_activate;
    case 0:
        return pointer_move | button | scroll | drag_source | drop_target | text | text_select;
    case 9:
        return pointer_move;
    }
    return 0;
}

bool above_confidence_threshold(Candidate candidate) {
    if (candidate.confidence >= parameters.minimum_confidence_q16) return true;
    const uint short_side = min(candidate.width, candidate.height);
    return parameters.band_minimum_confidence_q16 != 0 && short_side >= parameters.band_min_short_side_q3 &&
           short_side < parameters.band_max_short_side_q3 &&
           candidate.confidence >= parameters.band_minimum_confidence_q16;
}

uint64_t area(Candidate value) {
    return uint64_t(value.width) * value.height;
}

uint64_t intersection(Candidate a, Candidate b) {
    const uint left = max(a.x, b.x);
    const uint top = max(a.y, b.y);
    const uint right = min(a.x + a.width, b.x + b.width);
    const uint bottom = min(a.y + a.height, b.y + b.height);
    return left >= right || top >= bottom ? 0 : uint64_t(right - left) * (bottom - top);
}

uint64_t target_id(uint64_t source_id, Candidate candidate) {
    uint64_t hash = 14695981039346656037ull;
    const uint64_t values[7] = {source_id,
                                uint64_t(candidate.x) << 5,
                                uint64_t(candidate.y) << 5,
                                uint64_t(candidate.width) << 5,
                                uint64_t(candidate.height) << 5,
                                candidate.role,
                                candidate.source_bits};
    for (uint value_index = 0; value_index < 7; ++value_index) {
        for (uint byte_index = 0; byte_index < 8; ++byte_index) {
            hash ^= (values[value_index] >> (byte_index * 8)) & 0xff;
            hash *= 1099511628211ull;
        }
    }
    return hash == 0 ? 1 : hash;
}

groupshared uint wave_counts[8][radix_bins];
groupshared uint bin_totals[radix_bins];

[numthreads(256, 1, 1)] void targets_prepare(uint index : SV_DispatchThreadID) {
    if (index >= parameters.candidate_count) return;
    const Candidate candidate = load_candidate(index);
    const uint64_t confidence = uint64_t((~candidate.confidence) & 0xffff);
    const uint64_t key = !above_confidence_threshold(candidate) ? invalid_key
                                                                : (confidence << 48) | (uint64_t(candidate.y) << 32) |
                                                                      (uint64_t(candidate.x) << 16) | index;
    RadixEntry entry;
    entry.key = key;
    entry.candidate_index = index;
    entry.reserved = 0;
    entries_a[index] = entry;
}

    [numthreads(256, 1, 1)] void targets_radix_histogram(uint index : SV_DispatchThreadID,
                                                         uint local_index : SV_GroupThreadID,
                                                         uint3 group_id : SV_GroupID) {
    if (local_index < 8 * radix_bins) {
        wave_counts[local_index / radix_bins][local_index % radix_bins] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const bool active = index < parameters.candidate_count;
    const uint pass = parameters.radix_shift >> 2;
    RadixEntry entry = (RadixEntry)0;
    if (active) {
        if ((pass & 1) == 0)
            entry = entries_a[index];
        else
            entry = entries_b[index];
    }
    const uint digit = uint((entry.key >> parameters.radix_shift) & 15);
    const uint wave_index = local_index / WaveGetLaneCount();
    uint rank = 0;
    for (uint bin = 0; bin < radix_bins; ++bin) {
        const bool member = active && digit == bin;
        const uint prefix = WavePrefixCountBits(member);
        const uint count = WaveActiveCountBits(member);
        if (member) rank = prefix;
        if (WaveIsFirstLane()) wave_counts[wave_index][bin] = count;
    }
    GroupMemoryBarrierWithGroupSync();
    if (active) {
        for (uint prior = 0; prior < wave_index; ++prior) {
            rank += wave_counts[prior][digit];
        }
        local_ranks[index] = rank;
    }
    if (local_index < radix_bins) {
        uint count = 0;
        const uint wave_total = (256 + WaveGetLaneCount() - 1) / WaveGetLaneCount();
        for (uint wave = 0; wave < wave_total; ++wave) {
            count += wave_counts[wave][local_index];
        }
        block_offsets[group_id.x * radix_bins + local_index] = count;
    }
}

[numthreads(16, 1, 1)] void targets_radix_scan(uint digit : SV_GroupThreadID) {
    uint total = 0;
    for (uint block = 0; block < parameters.block_count; ++block) {
        total += block_offsets[block * radix_bins + digit];
    }
    bin_totals[digit] = total;
    GroupMemoryBarrierWithGroupSync();
    uint offset = 0;
    for (uint prior = 0; prior < digit; ++prior)
        offset += bin_totals[prior];
    for (uint block = 0; block < parameters.block_count; ++block) {
        const uint position = block * radix_bins + digit;
        const uint count = block_offsets[position];
        block_offsets[position] = offset;
        offset += count;
    }
}

    [numthreads(256, 1, 1)] void targets_radix_scatter(uint index : SV_DispatchThreadID, uint3 group_id : SV_GroupID) {
    if (index >= parameters.candidate_count) return;
    const uint pass = parameters.radix_shift >> 2;
    const bool a_to_b = (pass & 1) == 0;
    RadixEntry entry;
    if (a_to_b)
        entry = entries_a[index];
    else
        entry = entries_b[index];
    const uint digit = uint((entry.key >> parameters.radix_shift) & 15);
    const uint output_index = block_offsets[group_id.x * radix_bins + digit] + local_ranks[index];
    if (a_to_b)
        entries_b[output_index] = entry;
    else
        entries_a[output_index] = entry;
}

[numthreads(256, 1, 1)] void targets_suppression_masks(uint index : SV_DispatchThreadID) {
    const uint selected_count = min(parameters.candidate_count, parameters.maximum_targets);
    const uint work_count = selected_count * parameters.mask_word_count;
    if (index >= work_count) return;
    const uint row = index / parameters.mask_word_count;
    const uint word = index - row * parameters.mask_word_count;
    const Candidate earlier = load_candidate(entries_a[row].candidate_index);
    uint mask = 0;
    const uint first = word * 32;
    for (uint bit = 0; bit < 32; ++bit) {
        const uint later_index = first + bit;
        if (later_index <= row || later_index >= selected_count) continue;
        const Candidate later = load_candidate(entries_a[later_index].candidate_index);
        if (!above_confidence_threshold(later)) continue;
        const uint64_t overlap = intersection(later, earlier);
        const uint64_t later_area = area(later);
        const uint64_t union_area = later_area + area(earlier) - overlap;
        if (overlap == later_area || (overlap != 0 && overlap * 65535 >= union_area * parameters.iou_threshold_q16)) {
            mask |= 1u << bit;
        }
    }
    masks[row * parameters.mask_word_count + word] = mask;
}

void store_u64(uint offset, uint64_t value) {
    packet.Store2(offset, uint2(uint(value), uint(value >> 32)));
}

[numthreads(1, 1, 1)] void targets_finalize() {
    for (uint word = 0; word < parameters.mask_word_count; ++word) {
        suppressed[word] = 0;
    }
    packet.Store4(0, uint4(104, 0x00010002, 0, 80));
    packet.Store2(16, uint2(0, parameters.coordinate_space));
    packet.Store2(24, uint2(0, 0));
    store_u64(32, parameters.frame_id);
    store_u64(40, 0);
    store_u64(48, parameters.model_epoch);
    store_u64(56, parameters.session_epoch);
    store_u64(64, parameters.transform_epoch);
    store_u64(72, parameters.topology_epoch);
    store_u64(80, parameters.source_id);
    store_u64(88, 104);
    uint written = 0;
    uint above_threshold = 0;
    const uint selected_count = min(parameters.candidate_count, parameters.maximum_targets);
    for (uint ordered = 0; ordered < selected_count; ++ordered) {
        const Candidate candidate = load_candidate(entries_a[ordered].candidate_index);
        if (!above_confidence_threshold(candidate)) break;
        ++above_threshold;
        if ((suppressed[ordered >> 5] & (1u << (ordered & 31))) != 0) continue;
        for (uint word = 0; word < parameters.mask_word_count; ++word) {
            suppressed[word] |= masks[ordered * parameters.mask_word_count + word];
        }
        const uint base = 104 + written * 80;
        store_u64(base, target_id(parameters.source_id, candidate));
        packet.Store4(base + 8, uint4(0, 0, 0, 0));
        packet.Store4(base + 24, uint4(0, 0, candidate.x << 5, candidate.y << 5));
        const uint width_q8 = candidate.width << 5;
        const uint height_q8 = candidate.height << 5;
        packet.Store4(base + 40, uint4(width_q8, height_q8, (candidate.x << 5) + width_q8 / 2,
                                       (candidate.y << 5) + height_q8 / 2));
        packet.Store2(base + 56, uint2(candidate.confidence, candidate.role | (candidate.source_bits << 16)));
        uint flags = candidate.flags & ~target_actionable;
        uint capabilities = 0;
        const bool safe_interior = candidate.width > safe_inset_q3 * 2 && candidate.height > safe_inset_q3 * 2;
        if ((candidate.flags & target_actionable) != 0 && safe_interior &&
            (flags & (target_disabled | target_secure)) == 0) {
            capabilities = capabilities_for_role(candidate.role);
            if (capabilities != 0) flags |= target_actionable;
        }
        packet.Store4(base + 64, uint4(capabilities, flags, written, 0));
        ++written;
    }
    packet.Store(8, written);
    store_u64(96, 104 + uint64_t(written) * 80);
    counters[0] = above_threshold;
    counters[1] = written;
}
