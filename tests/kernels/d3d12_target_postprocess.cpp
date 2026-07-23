#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/target_postprocessor.hpp"
#include "kernels/targets/postprocess.hpp"
#include "core/stack_string_builder.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;
using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::TargetPacketSpan;
using saccade::backend::d3d12::TargetPostprocessor;
using saccade::backend::d3d12::TargetPostprocessorSpec;
using saccade::backend::d3d12::TargetPostprocessSubmission;
using saccade::kernels::targets::DenseCandidate;
using saccade::kernels::targets::PostprocessConfig;
using saccade::kernels::targets::PostprocessEpochs;
using saccade::kernels::targets::PostprocessStats;
using saccade::kernels::targets::PostprocessWorkspace;

constexpr uint32_t candidate_count = 777;
constexpr uint32_t target_capacity = 96;

enum class TestResult : int {
    success,
    usage,
    device_unavailable = 77,
    candidate_buffer_failed = 3,
    initialization_failed,
    scalar_failed,
    submission_failed,
    packet_failed,
    parity_failed,
    statistics_failed,
    device_loss_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void report_packet_failure(SaccadeResult packet_result, const TargetPacketSpan& packet, ID3D12Device* device) noexcept {
    saccade::core::StackStringBuilder<256> text;
    (void)text.append("packet_result=");
    (void)text.append_signed(packet_result);
    if (packet.data != nullptr && packet.size >= sizeof(SaccadeTargetPacketHeader)) {
        uint32_t structure_size = 0;
        uint32_t target_count_value = 0;
        uint64_t total_size = 0;
        std::memcpy(&structure_size, packet.data, sizeof(structure_size));
        std::memcpy(&target_count_value, packet.data + 8, sizeof(target_count_value));
        std::memcpy(&total_size, packet.data + offsetof(SaccadeTargetPacketHeader, total_size), sizeof(total_size));
        (void)text.append(" header=");
        (void)text.append_unsigned(structure_size);
        (void)text.append(" targets=");
        (void)text.append_unsigned(target_count_value);
        (void)text.append(" total=");
        (void)text.append_unsigned(total_size);
    }
    (void)text.append(" removed=");
    (void)text.append_signed(device->GetDeviceRemovedReason());
    (void)text.append('\n');
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

void report_submission_failure(uint32_t candidate_count_value, SaccadeResult submit_result,
                               SaccadeResult wait_result) noexcept {
    saccade::core::StackStringBuilder<160> text;
    (void)text.append("candidates=");
    (void)text.append_unsigned(candidate_count_value);
    (void)text.append(" submit=");
    (void)text.append_signed(submit_result);
    (void)text.append(" wait=");
    (void)text.append_signed(wait_result);
    (void)text.append('\n');
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

void report_parity_failure(uint32_t candidate_count_value, const TargetPacketSpan& packet, const uint8_t* expected,
                           size_t expected_size) noexcept {
    saccade::core::StackStringBuilder<256> text;
    (void)text.append("candidates=");
    (void)text.append_unsigned(candidate_count_value);
    (void)text.append(" size=");
    (void)text.append_unsigned(packet.size);
    (void)text.append(" expected_size=");
    (void)text.append_unsigned(expected_size);
    const size_t common = std::min(packet.size, expected_size);
    for (size_t index = 0; index < common; ++index) {
        if (packet.data[index] != expected[index]) {
            (void)text.append(" first_byte=");
            (void)text.append_unsigned(index);
            (void)text.append(" actual=");
            (void)text.append_unsigned(packet.data[index]);
            (void)text.append(" expected=");
            (void)text.append_unsigned(expected[index]);
            if (index >= sizeof(SaccadeTargetPacketHeader)) {
                const size_t target_index = (index - sizeof(SaccadeTargetPacketHeader)) / sizeof(SaccadeTargetRecord);
                const auto* actual_targets =
                    reinterpret_cast<const SaccadeTargetRecord*>(packet.data + sizeof(SaccadeTargetPacketHeader));
                const auto* expected_targets =
                    reinterpret_cast<const SaccadeTargetRecord*>(expected + sizeof(SaccadeTargetPacketHeader));
                (void)text.append(" target=");
                (void)text.append_unsigned(target_index);
                (void)text.append(" confidence=");
                (void)text.append_unsigned(actual_targets[target_index].confidence_q16);
                (void)text.append("/");
                (void)text.append_unsigned(expected_targets[target_index].confidence_q16);
                (void)text.append(" xy=");
                (void)text.append_signed(actual_targets[target_index].x_q8);
                (void)text.append("/");
                (void)text.append_signed(expected_targets[target_index].x_q8);
            }
            break;
        }
    }
    (void)text.append('\n');
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.view().data(), static_cast<DWORD>(text.size()), &written,
                    nullptr);
}

void make_candidates(std::array<DenseCandidate, candidate_count>* output) noexcept {
    uint32_t value = 0x12345678U;
    for (uint32_t index = 0; index < output->size(); ++index) {
        value = value * 1664525U + 1013904223U;
        DenseCandidate candidate{};
        candidate.x_q3 = static_cast<uint16_t>((index % 28U) * 2000U);
        candidate.y_q3 = static_cast<uint16_t>((index / 28U) * 2000U);
        candidate.width_q3 = static_cast<uint16_t>(24U + (value & 255U));
        candidate.height_q3 = static_cast<uint16_t>(24U + ((value >> 8) & 255U));
        candidate.confidence_q16 = static_cast<uint16_t>(value >> 16);
        candidate.role = static_cast<uint8_t>(index % 11U);
        candidate.source_bits = SACCADE_TARGET_SOURCE_NEURAL;
        candidate.flags = SACCADE_TARGET_ACTIONABLE;
        (*output)[index] = candidate;
    }
    (*output)[13] = {
        40, 40, 800, 600, 65535, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[29] = {
        80, 80, 100, 100, 65534, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[47] = {
        44, 44, 800, 600, 65533, SACCADE_TARGET_ROLE_BUTTON, SACCADE_TARGET_SOURCE_NEURAL, SACCADE_TARGET_ACTIONABLE,
        0};
    (*output)[5] = {12000,
                    12000,
                    16,
                    16,
                    65532,
                    SACCADE_TARGET_ROLE_BUTTON,
                    SACCADE_TARGET_SOURCE_NEURAL,
                    SACCADE_TARGET_ACTIONABLE,
                    0};
}

ComPtr<ID3D12Resource> candidate_buffer(ID3D12Device* device,
                                        const std::array<DenseCandidate, candidate_count>& candidates) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(candidates);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> output;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(output.GetAddressOf())))) {
        return {};
    }
    void* mapped = nullptr;
    if (FAILED(output->Map(0, nullptr, &mapped))) return {};
    std::memcpy(mapped, candidates.data(), sizeof(candidates));
    output->Unmap(0, nullptr);
    return output;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return result(TestResult::usage);
    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return result(TestResult::device_unavailable);
    }
    std::array<DenseCandidate, candidate_count> candidates{};
    make_candidates(&candidates);
    const ComPtr<ID3D12Resource> candidate_resource = candidate_buffer(graphics.device(), candidates);
    if (candidate_resource == nullptr) {
        return result(TestResult::candidate_buffer_failed);
    }
    TargetPostprocessor postprocessor;
    const TargetPostprocessorSpec spec{candidate_count, target_capacity, candidate_resource.Get(), 0};
    if (postprocessor.initialize(graphics.device(), graphics.queue(), argv[1], spec) != SACCADE_OK) {
        return result(TestResult::initialization_failed);
    }
    PostprocessConfig config{};
    config.maximum_targets = target_capacity;
    config.minimum_confidence_q16 = 12000;
    config.band_minimum_confidence_q16 = 10000;
    config.band_min_short_side_q3 = 96;
    config.band_max_short_side_q3 = 192;
    config.iou_threshold_q16 = 32768;
    const PostprocessEpochs epochs{101, 202, 303, 404, 505, 606};
    static PostprocessWorkspace workspace;
    alignas(8) std::array<uint8_t, sizeof(SaccadeTargetPacketHeader) + target_capacity * sizeof(SaccadeTargetRecord)>
        expected{};
    size_t expected_size = 0;
    PostprocessStats scalar_stats{};
    constexpr std::array<uint32_t, 5> counts{0, 1, 256, 257, candidate_count};
    for (uint32_t active_candidates : counts) {
        if (saccade::kernels::targets::postprocess(candidates.data(), active_candidates, config, epochs, &workspace,
                                                   {expected.data(), expected.size()}, &expected_size,
                                                   &scalar_stats) != SACCADE_OK) {
            return result(TestResult::scalar_failed);
        }
        TargetPostprocessSubmission submission{};
        const SaccadeResult submit_result = postprocessor.submit(active_candidates, config, epochs, &submission);
        const SaccadeResult wait_result =
            submit_result == SACCADE_OK ? postprocessor.wait(submission, UINT64_MAX) : SACCADE_OK;
        if (submit_result != SACCADE_OK || wait_result != SACCADE_OK) {
            report_submission_failure(active_candidates, submit_result, wait_result);
            return result(TestResult::submission_failed);
        }
        TargetPacketSpan packet{};
        const SaccadeResult packet_result = postprocessor.packet(submission, &packet);
        if (packet_result != SACCADE_OK) {
            report_packet_failure(packet_result, packet, graphics.device());
            return result(TestResult::packet_failed);
        }
        if (packet.size != expected_size ||
            std::memcmp(packet.data, expected.data(), std::min(packet.size, expected_size)) != 0) {
            report_parity_failure(active_candidates, packet, expected.data(), expected_size);
            return result(TestResult::parity_failed);
        }
    }
    const auto stats = postprocessor.stats();
    if (stats.submissions != counts.size() || stats.completed != counts.size() || stats.failures != 0 ||
        stats.busy_submissions != 0 || stats.candidate_capacity != candidate_count ||
        stats.target_capacity != target_capacity || stats.radix_passes != 16 || stats.workspace_bytes == 0 ||
        stats.packet_readback_bytes != expected.size()) {
        return result(TestResult::statistics_failed);
    }
    TargetPostprocessSubmission removed_submission{};
    ComPtr<ID3D12Device5> removable;
    if (postprocessor.submit(candidate_count, config, epochs, &removed_submission) != SACCADE_OK ||
        FAILED(graphics.device()->QueryInterface(IID_PPV_ARGS(removable.GetAddressOf())))) {
        return result(TestResult::device_loss_failed);
    }
    removable->RemoveDevice();
    bool complete = true;
    if (postprocessor.poll(removed_submission, &complete) != SACCADE_ERROR_BACKEND || complete ||
        postprocessor.wait(removed_submission, UINT64_C(1'000'000)) != SACCADE_ERROR_BACKEND) {
        return result(TestResult::device_loss_failed);
    }
    return result(TestResult::success);
}
