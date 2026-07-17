#include "backends/d3d12/directml_inference.hpp"
#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/preprocessor.hpp"
#include "backends/d3d12/target_postprocessor.hpp"
#include "kernels/targets/postprocess.hpp"
#include "model/directml_contract.hpp"
#include "model/mapped_artifact.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>
#include <thread>

namespace {

using Microsoft::WRL::ComPtr;
using saccade::backend::d3d12::CandidateInput;
using saccade::backend::d3d12::DirectMlBindingDesc;
using saccade::backend::d3d12::DirectMlInference;
using saccade::backend::d3d12::DirectMlSessionDesc;
using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::ImagePreprocessor;
using saccade::backend::d3d12::TargetPostprocessor;
using saccade::backend::d3d12::TargetPostprocessorSpec;
using saccade::backend::d3d12::TargetPostprocessSubmission;
using saccade::backend::image::PreprocessSubmission;
using saccade::backend::image::TensorFormat;
using saccade::backend::image::TensorSpec;
using saccade::kernels::targets::PostprocessConfig;
using saccade::kernels::targets::PostprocessEpochs;
using saccade::model::MappedArtifact;

constexpr uint32_t default_duration_seconds = 3;
constexpr uint32_t release_duration_seconds = 3'600;
constexpr uint32_t maximum_duration_seconds = release_duration_seconds;
constexpr uint32_t warmup_iterations = 32;
constexpr uint32_t release_frames_per_second = 30;
constexpr uint32_t release_minimum_measured_millihz = 29'000;
constexpr uint32_t maximum_frames_per_second = 1'000;
constexpr uint32_t maximum_measured_millihz = maximum_frames_per_second * 1'000U;
constexpr uint32_t maximum_samples = maximum_duration_seconds + 2;
constexpr uint32_t default_source_width = 3'840;
constexpr uint32_t default_source_height = 2'160;
constexpr uint32_t minimum_source_extent = 16;
constexpr uint32_t maximum_source_extent = UINT16_MAX >> 3U;
constexpr uint64_t wait_timeout_ns = UINT64_C(5'000'000'000);
constexpr DWORD wait_timeout_ms = 5'000;
constexpr uint64_t sample_period_ms = 1'000;
constexpr uint32_t bgra_bytes_per_pixel = 4;
constexpr uint64_t bytes_per_megabyte = UINT64_C(1024) * 1024U;
constexpr uint32_t default_private_budget_mb = 64;
constexpr uint32_t default_handle_budget = 8;
constexpr uint32_t default_video_budget_mb = 64;
constexpr uint32_t default_saccade_budget_mb = 64;
constexpr uint32_t default_monotonic_samples = 8;
constexpr uint64_t minimum_private_monotonic_growth = bytes_per_megabyte;
constexpr uint64_t minimum_video_monotonic_growth = bytes_per_megabyte * 4U;
constexpr uint64_t synthetic_session_epoch = 1;
constexpr uint64_t synthetic_transform_epoch = 1;
constexpr uint64_t synthetic_topology_epoch = 1;
constexpr uint64_t synthetic_source_id = UINT64_C(0x5341434341444501);
constexpr size_t model_name_capacity = 65;

enum class Result : int {
    success,
    usage,
    device_unavailable = 77,
    artifact_failed,
    resource_failed,
    pipeline_failed,
    metric_failed,
    qualification_failed,
    cleanup_failed
};

struct Limits {
    uint32_t duration_seconds_ = default_duration_seconds;
    uint32_t frames_per_second_ = release_frames_per_second;
    uint32_t minimum_measured_millihz_ = release_minimum_measured_millihz;
    uint64_t private_budget_bytes_ = static_cast<uint64_t>(default_private_budget_mb) * bytes_per_megabyte;
    uint32_t handle_budget_ = default_handle_budget;
    uint64_t video_budget_bytes_ = static_cast<uint64_t>(default_video_budget_mb) * bytes_per_megabyte;
    uint64_t saccade_budget_bytes_ = static_cast<uint64_t>(default_saccade_budget_mb) * bytes_per_megabyte;
    uint32_t monotonic_samples_ = default_monotonic_samples;
    uint32_t source_width_ = default_source_width;
    uint32_t source_height_ = default_source_height;
};

struct MemorySample {
    uint64_t private_bytes_ = 0;
    uint32_t handles_ = 0;
    uint64_t local_usage_ = 0;
    uint64_t local_budget_ = 0;
    uint64_t nonlocal_usage_ = 0;
    uint64_t nonlocal_budget_ = 0;
    uint64_t saccade_bytes_ = 0;
    bool video_memory_valid_ = false;
};

struct SampleHistory {
    std::array<MemorySample, maximum_samples> samples_{};
    uint32_t count_ = 0;
};

int code(Result value) noexcept {
    return static_cast<int>(value);
}

SaccadeResult trust_benchmark_artifact(void*, const saccade::model::ArtifactView&) noexcept {
    return SACCADE_OK;
}

bool parse_u32(std::string_view text, uint32_t minimum, uint32_t maximum, uint32_t* output) noexcept {
    if (text.empty() || output == nullptr) return false;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < minimum || value > maximum)
        return false;
    *output = value;
    return true;
}

bool parse_mb(std::string_view text, uint64_t* output) noexcept {
    uint32_t mb = 0;
    if (!parse_u32(text, 1, UINT32_MAX / static_cast<uint32_t>(bytes_per_megabyte), &mb) || output == nullptr)
        return false;
    *output = static_cast<uint64_t>(mb) * bytes_per_megabyte;
    return true;
}

bool parse_args(int argc, char** argv, Limits* limits) noexcept {
    constexpr int required_argument_count = 3;
    constexpr int optional_argument_count = 10;
    if (limits == nullptr || argc < required_argument_count || argc > required_argument_count + optional_argument_count)
        return false;
    for (int index = 3; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const size_t equals = argument.find('=');
        if (equals == std::string_view::npos) return false;
        const std::string_view name = argument.substr(0, equals);
        const std::string_view value = argument.substr(equals + 1U);
        if (name == "--duration" && !parse_u32(value, 1, maximum_duration_seconds, &limits->duration_seconds_))
            return false;
        if (name == "--frames-per-second" &&
            !parse_u32(value, 0, maximum_frames_per_second, &limits->frames_per_second_))
            return false;
        else if (name == "--minimum-measured-millihz" &&
                 !parse_u32(value, 0, maximum_measured_millihz, &limits->minimum_measured_millihz_))
            return false;
        else if (name == "--private-budget-mb" && !parse_mb(value, &limits->private_budget_bytes_))
            return false;
        else if (name == "--handle-budget" && !parse_u32(value, 0, UINT32_MAX, &limits->handle_budget_))
            return false;
        else if (name == "--video-budget-mb" && !parse_mb(value, &limits->video_budget_bytes_))
            return false;
        else if (name == "--saccade-budget-mb" && !parse_mb(value, &limits->saccade_budget_bytes_))
            return false;
        else if (name == "--monotonic-samples" && !parse_u32(value, 2, maximum_samples, &limits->monotonic_samples_))
            return false;
        else if (name == "--source-width" &&
                 !parse_u32(value, minimum_source_extent, maximum_source_extent, &limits->source_width_))
            return false;
        else if (name == "--source-height" &&
                 !parse_u32(value, minimum_source_extent, maximum_source_extent, &limits->source_height_))
            return false;
        else if (name != "--duration" && name != "--frames-per-second" && name != "--minimum-measured-millihz" &&
                 name != "--private-budget-mb" && name != "--handle-budget" && name != "--video-budget-mb" &&
                 name != "--saccade-budget-mb" && name != "--monotonic-samples" && name != "--source-width" &&
                 name != "--source-height")
            return false;
    }
    return true;
}

void write_text(std::string_view text) noexcept {
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

int fail(Result result, std::string_view stage) noexcept {
    write_text("windows_d3d12_memory_soak failure=");
    write_text(stage);
    write_text("\n");
    return code(result);
}

void usage() noexcept {
    write_text("usage: windows_d3d12_memory_soak <artifact> <shader-directory> [--duration=seconds] "
               "[--frames-per-second=count; 0 is uncapped] "
               "[--minimum-measured-millihz=count] "
               "[--private-budget-mb=MB] [--handle-budget=count] [--video-budget-mb=MB] "
               "[--saccade-budget-mb=MB] [--monotonic-samples=count] "
               "[--source-width=pixels] [--source-height=pixels]\n"
               "release lane: --duration=3600 --frames-per-second=30 "
               "--minimum-measured-millihz=29000 with explicit machine budgets\n");
}

bool execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands) noexcept {
    if (FAILED(commands->Close())) return false;
    ID3D12CommandList* lists[]{commands};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))) ||
        FAILED(queue->Signal(fence.Get(), 1)))
        return false;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return false;
    const bool complete = SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                          WaitForSingleObject(event, wait_timeout_ms) == WAIT_OBJECT_0;
    (void)CloseHandle(event);
    return complete;
}

ComPtr<ID3D12Resource> make_texture(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width,
                                    uint32_t height) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(texture.GetAddressOf()))))
        return {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t upload_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_bytes);
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(upload.GetAddressOf()))))
        return {};
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return {};
    auto* pixels = static_cast<uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* pixel = pixels + footprint.Offset + y * footprint.Footprint.RowPitch + x * bgra_bytes_per_pixel;
            pixel[0] = static_cast<uint8_t>(x * 3U);
            pixel[1] = static_cast<uint8_t>(y * 3U);
            pixel[2] = static_cast<uint8_t>(x + y);
            pixel[3] = 255;
        }
    }
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf()))))
        return {};
    D3D12_TEXTURE_COPY_LOCATION destination{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION source{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    source.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);
    return execute_and_wait(device, queue, commands.Get()) ? texture : ComPtr<ID3D12Resource>{};
}

ComPtr<ID3D12Resource> make_candidate_buffer(ID3D12Device* device, uint32_t candidates) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<uint64_t>(candidates) * saccade::model::directml::normalized_target_row_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> buffer;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                     nullptr, IID_PPV_ARGS(buffer.GetAddressOf())))
               ? buffer
               : ComPtr<ID3D12Resource>{};
}

bool process_metrics(IDXGIAdapter3* adapter, MemorySample* output, uint64_t saccade_bytes) noexcept {
    if (output == nullptr) return false;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    DWORD handles = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0 ||
        GetProcessHandleCount(GetCurrentProcess(), &handles) == 0)
        return false;
    output->private_bytes_ = counters.PrivateUsage;
    output->handles_ = handles;
    output->saccade_bytes_ = saccade_bytes;
    if (adapter == nullptr) return true;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
        return true;
    output->local_usage_ = local.CurrentUsage;
    output->local_budget_ = local.Budget;
    output->nonlocal_usage_ = nonlocal.CurrentUsage;
    output->nonlocal_budget_ = nonlocal.Budget;
    output->video_memory_valid_ = true;
    return true;
}

bool exceeds(const MemorySample& baseline, const MemorySample& sample, const Limits& limits) noexcept {
    if (sample.private_bytes_ > baseline.private_bytes_ + limits.private_budget_bytes_ ||
        sample.handles_ > baseline.handles_ + limits.handle_budget_ ||
        sample.saccade_bytes_ > baseline.saccade_bytes_ + limits.saccade_budget_bytes_)
        return true;
    if (sample.video_memory_valid_ && baseline.video_memory_valid_ &&
        (sample.local_usage_ > baseline.local_usage_ + limits.video_budget_bytes_ ||
         sample.nonlocal_usage_ > baseline.nonlocal_usage_ + limits.video_budget_bytes_))
        return true;
    return false;
}

bool monotonic_growth(const SampleHistory& history, const Limits& limits) noexcept {
    if (history.count_ < limits.monotonic_samples_) return false;
    const uint32_t first = history.count_ - limits.monotonic_samples_;
    uint32_t private_growth = 0;
    uint32_t handle_growth = 0;
    uint32_t saccade_growth = 0;
    uint32_t video_growth = 0;
    for (uint32_t index = first + 1U; index < history.count_; ++index) {
        private_growth += history.samples_[index].private_bytes_ > history.samples_[index - 1U].private_bytes_;
        handle_growth += history.samples_[index].handles_ > history.samples_[index - 1U].handles_;
        saccade_growth += history.samples_[index].saccade_bytes_ > history.samples_[index - 1U].saccade_bytes_;
        video_growth += history.samples_[index].video_memory_valid_ &&
                        history.samples_[index - 1U].video_memory_valid_ &&
                        (history.samples_[index].local_usage_ > history.samples_[index - 1U].local_usage_ ||
                         history.samples_[index].nonlocal_usage_ > history.samples_[index - 1U].nonlocal_usage_);
    }
    const uint32_t required = limits.monotonic_samples_ - 1U;
    const MemorySample& first_sample = history.samples_[first];
    const MemorySample& last_sample = history.samples_[history.count_ - 1U];
    const bool private_unbounded =
        private_growth == required &&
        last_sample.private_bytes_ >= first_sample.private_bytes_ + minimum_private_monotonic_growth;
    const bool handles_unbounded = handle_growth == required && last_sample.handles_ > first_sample.handles_;
    const bool saccade_unbounded =
        saccade_growth == required && last_sample.saccade_bytes_ > first_sample.saccade_bytes_;
    const bool video_unbounded =
        video_growth == required &&
        (last_sample.local_usage_ >= first_sample.local_usage_ + minimum_video_monotonic_growth ||
         last_sample.nonlocal_usage_ >= first_sample.nonlocal_usage_ + minimum_video_monotonic_growth);
    return private_unbounded || handles_unbounded || saccade_unbounded || video_unbounded;
}

void emit_sample(uint32_t index, const MemorySample& sample) noexcept {
    char line[512]{};
    const auto append = [](char* begin, char* end, uint64_t value) noexcept {
        return std::to_chars(begin, end, value).ptr;
    };
    char* cursor = line;
    char* end = line + sizeof(line);
    const char prefix[] = "sample=";
    std::memcpy(cursor, prefix, sizeof(prefix) - 1U);
    cursor += sizeof(prefix) - 1U;
    cursor = append(cursor, end, index);
    const char* labels[] = {
        " private=", " handles=", " local=", " local_budget=", " nonlocal=", " nonlocal_budget=", " saccade="};
    const uint64_t values[] = {sample.private_bytes_, sample.handles_,        sample.local_usage_,
                               sample.local_budget_,  sample.nonlocal_usage_, sample.nonlocal_budget_,
                               sample.saccade_bytes_};
    for (uint32_t metric = 0; metric < std::size(labels); ++metric) {
        const size_t size = std::strlen(labels[metric]);
        if (cursor + size >= end) return;
        std::memcpy(cursor, labels[metric], size);
        cursor += size;
        cursor = append(cursor, end, values[metric]);
    }
    if (cursor + 1U < end) *cursor++ = '\n';
    write_text(std::string_view{line, static_cast<size_t>(cursor - line)});
}

void emit_result(uint64_t frames, uint64_t elapsed_ns, uint64_t measured_millihz, uint32_t minimum_measured_millihz,
                 uint32_t configured_frames_per_second, uint32_t source_width, uint32_t source_height,
                 bool passed) noexcept {
    char line[320]{};
    char* cursor = line;
    char* const end = line + sizeof(line);
    constexpr std::string_view prefix = "windows_d3d12_memory_soak frames=";
    constexpr std::string_view elapsed = " elapsed_ns=";
    constexpr std::string_view measured = " measured_millihz=";
    constexpr std::string_view minimum = " minimum_measured_millihz=";
    constexpr std::string_view cadence = " configured_frames_per_second=";
    constexpr std::string_view width = " source_width=";
    constexpr std::string_view height = " source_height=";
    constexpr std::string_view pass_suffix = " result=pass\n";
    constexpr std::string_view fail_suffix = " result=fail\n";
    std::memcpy(cursor, prefix.data(), prefix.size());
    cursor += prefix.size();
    cursor = std::to_chars(cursor, end, frames).ptr;
    std::memcpy(cursor, elapsed.data(), elapsed.size());
    cursor += elapsed.size();
    cursor = std::to_chars(cursor, end, elapsed_ns).ptr;
    std::memcpy(cursor, measured.data(), measured.size());
    cursor += measured.size();
    cursor = std::to_chars(cursor, end, measured_millihz).ptr;
    std::memcpy(cursor, minimum.data(), minimum.size());
    cursor += minimum.size();
    cursor = std::to_chars(cursor, end, minimum_measured_millihz).ptr;
    std::memcpy(cursor, cadence.data(), cadence.size());
    cursor += cadence.size();
    cursor = std::to_chars(cursor, end, configured_frames_per_second).ptr;
    std::memcpy(cursor, width.data(), width.size());
    cursor += width.size();
    cursor = std::to_chars(cursor, end, source_width).ptr;
    std::memcpy(cursor, height.data(), height.size());
    cursor += height.size();
    cursor = std::to_chars(cursor, end, source_height).ptr;
    const std::string_view suffix = passed ? pass_suffix : fail_suffix;
    std::memcpy(cursor, suffix.data(), suffix.size());
    cursor += suffix.size();
    write_text(std::string_view{line, static_cast<size_t>(cursor - line)});
}

} // namespace

int main(int argc, char** argv) {
    Limits limits{};
    if (!parse_args(argc, argv, &limits)) {
        usage();
        return code(Result::usage);
    }

    MappedArtifact artifact;
    const saccade::model::ArtifactVerifier verifier{nullptr, trust_benchmark_artifact};
    if (artifact.initialize(argv[1], verifier) != SACCADE_OK) return fail(Result::artifact_failed, "artifact_map");
    saccade::model::directml::Contract contract{};
    if (saccade::model::directml::parse_contract(artifact.view(), &contract) != SACCADE_OK)
        return fail(Result::artifact_failed, "artifact_contract");

    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) return fail(Result::device_unavailable, "graphics_device");
    const ComPtr<ID3D12Resource> texture =
        make_texture(graphics.device(), graphics.queue(), limits.source_width_, limits.source_height_);
    const ComPtr<ID3D12Resource> candidates = make_candidate_buffer(graphics.device(), contract.candidate_capacity);
    if (texture == nullptr) return fail(Result::resource_failed, "synthetic_texture");
    if (candidates == nullptr) return fail(Result::resource_failed, "candidate_buffer");

    TensorSpec tensor_spec{};
    tensor_spec.width = artifact.view().input_width;
    tensor_spec.height = artifact.view().input_height;
    tensor_spec.format = contract.input_kind == saccade::model::directml::InputKind::planar_fp16
                             ? TensorFormat::planar_fp16
                             : TensorFormat::planar_int8;
    tensor_spec.channel_scale = contract.channel_scale;
    tensor_spec.channel_bias = contract.channel_bias;
    tensor_spec.letterbox_rgb = contract.letterbox_rgb;
    ImagePreprocessor preprocessor;
    if (preprocessor.initialize(graphics.device(), graphics.queue(), argv[2], tensor_spec) != SACCADE_OK) {
        return fail(Result::pipeline_failed, "preprocessor_initialize");
    }
    saccade::backend::image::TensorView tensor{};
    if (preprocessor.tensor_storage(&tensor) != SACCADE_OK) return fail(Result::pipeline_failed, "preprocessor_tensor");
    std::array<char, model_name_capacity> input_name{};
    std::array<char, model_name_capacity> candidate_name{};
    if (contract.input_name.size >= input_name.size() || contract.candidate_name.size >= candidate_name.size()) {
        return fail(Result::pipeline_failed, "binding_names");
    }
    std::memcpy(input_name.data(), contract.input_name.data, contract.input_name.size);
    std::memcpy(candidate_name.data(), contract.candidate_name.data, contract.candidate_name.size);
    DirectMlBindingDesc input{input_name.data(),
                              static_cast<ID3D12Resource*>(tensor.buffer),
                              tensor.byte_size,
                              {1, 3, static_cast<int64_t>(tensor_spec.height), static_cast<int64_t>(tensor_spec.width)},
                              4,
                              tensor_spec.format == TensorFormat::planar_fp16
                                  ? saccade::backend::d3d12::TensorElementType::fp16
                                  : saccade::backend::d3d12::TensorElementType::int8};
    DirectMlBindingDesc output{
        candidate_name.data(),
        candidates.Get(),
        static_cast<size_t>(contract.candidate_capacity) * saccade::model::directml::normalized_target_row_bytes,
        {static_cast<int64_t>(contract.candidate_capacity), saccade::model::directml::target_row_components},
        2,
        saccade::backend::d3d12::TensorElementType::fp16};
    DirectMlInference inference;
    const DirectMlSessionDesc session{contract.graph, &input, &output, 1, 1};
    TargetPostprocessor postprocessor;
    const TargetPostprocessorSpec postprocess_spec{contract.candidate_capacity,
                                                   artifact.view().max_targets,
                                                   candidates.Get(),
                                                   0,
                                                   CandidateInput::normalized_fp16,
                                                   artifact.view().input_width,
                                                   artifact.view().input_height,
                                                   0};
    if (inference.initialize(graphics.device(), graphics.queue(), session) != SACCADE_OK)
        return fail(Result::pipeline_failed, "inference_initialize");
    if (postprocessor.initialize(graphics.device(), graphics.queue(), argv[2], postprocess_spec) != SACCADE_OK)
        return fail(Result::pipeline_failed, "postprocessor_initialize");

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        const uint64_t adapter_luid = graphics.adapter_luid();
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumerated = factory->EnumAdapters1(index, candidate.GetAddressOf());
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) break;
            if (candidate == nullptr) continue;
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(candidate->GetDesc1(&description)) &&
                std::memcmp(&description.AdapterLuid, &adapter_luid, sizeof(LUID)) == 0) {
                (void)candidate.As(&adapter);
                break;
            }
        }
    }

    const auto run_once = [&](uint64_t frame_id) noexcept -> const char* {
        PreprocessSubmission preprocess_submission{};
        TargetPostprocessSubmission postprocess_submission{};
        const PostprocessConfig config{artifact.view().max_targets,          contract.minimum_confidence_q16,
                                       contract.band_minimum_confidence_q16, contract.band_min_short_side_q3,
                                       contract.band_max_short_side_q3,      contract.iou_threshold_q16,
                                       SACCADE_COORDINATE_SPACE_MODEL_Q8,    0};
        const PostprocessEpochs epochs{frame_id,
                                       artifact.view().stable_id,
                                       synthetic_session_epoch,
                                       synthetic_transform_epoch,
                                       synthetic_topology_epoch,
                                       synthetic_source_id};
        if (preprocessor.submit(texture.Get(), limits.source_width_, limits.source_height_, {}, frame_id,
                                synthetic_transform_epoch, &preprocess_submission) != SACCADE_OK)
            return "preprocessor_submit";
        if (preprocessor.wait(&preprocess_submission, wait_timeout_ns) != SACCADE_OK) return "preprocessor_wait";
        if (inference.run() != SACCADE_OK) return "inference_run";
        if (inference.synchronize_outputs() != SACCADE_OK) return "inference_synchronize";
        if (postprocessor.submit(contract.candidate_capacity, limits.source_width_, limits.source_height_, config,
                                 epochs, &postprocess_submission) != SACCADE_OK)
            return "postprocessor_submit";
        if (postprocessor.wait(postprocess_submission, wait_timeout_ns) != SACCADE_OK) return "postprocessor_wait";
        return nullptr;
    };
    for (uint32_t index = 0; index < warmup_iterations; ++index) {
        if (const char* stage = run_once(index + 1U); stage != nullptr) return fail(Result::pipeline_failed, stage);
    }

    static thread_local SampleHistory history{};
    MemorySample baseline{};
    const auto record = [&](MemorySample* sample) noexcept {
        const auto stats = inference.stats();
        const uint64_t owned = ImagePreprocessor::storage_size + DirectMlInference::storage_size + stats.input_bytes +
                               stats.output_bytes + stats.model_bytes + postprocessor.stats().workspace_bytes +
                               postprocessor.stats().packet_readback_bytes;
        return process_metrics(adapter.Get(), sample, owned);
    };
    if (!record(&baseline)) return code(Result::metric_failed);
    history.samples_[0] = baseline;
    history.count_ = 1;
    emit_sample(0, baseline);
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(limits.duration_seconds_);
    const auto sample_period = std::chrono::milliseconds(sample_period_ms);
    const auto frame_period = limits.frames_per_second_ == 0
                                  ? Clock::duration::zero()
                                  : std::chrono::nanoseconds(UINT64_C(1'000'000'000) / limits.frames_per_second_);
    auto next_sample = started + sample_period;
    auto next_frame = started;
    bool pending_frame = false;
    uint64_t frame_id = warmup_iterations + 1U;
    while (true) {
        auto now = Clock::now();
        if (now >= deadline) break;

        if (now >= next_sample) {
            do {
                next_sample += sample_period;
            } while (next_sample <= now);
            if (history.count_ >= maximum_samples) return code(Result::metric_failed);
            MemorySample sample{};
            if (!record(&sample)) return code(Result::metric_failed);
            const uint32_t sample_index = history.count_;
            history.samples_[sample_index] = sample;
            ++history.count_;
            emit_sample(sample_index, sample);
            if (exceeds(baseline, sample, limits) || monotonic_growth(history, limits))
                return code(Result::qualification_failed);
            now = Clock::now();
            if (now >= deadline) break;
        }

        if (limits.frames_per_second_ == 0 || pending_frame || now >= next_frame) {
            if (limits.frames_per_second_ != 0) {
                if (!pending_frame) {
                    next_frame += frame_period;
                }
                pending_frame = false;
                while (next_frame <= now) {
                    next_frame += frame_period;
                }
            }
            if (const char* stage = run_once(frame_id++); stage != nullptr) {
                return fail(Result::pipeline_failed, stage);
            }
            if (limits.frames_per_second_ != 0) {
                now = Clock::now();
                if (next_frame <= now) {
                    pending_frame = true;
                    do {
                        next_frame += frame_period;
                    } while (next_frame <= now);
                }
            }
            continue;
        }

        if (now < next_sample) {
            std::this_thread::sleep_until(std::min(next_frame, next_sample));
            continue;
        }
    }
    const uint64_t frames = frame_id - warmup_iterations - 1U;
    const uint64_t elapsed_ns =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
    const long double measured_rate = elapsed_ns == 0 ? 0.0L
                                                      : static_cast<long double>(frames) * 1'000'000'000'000.0L /
                                                            static_cast<long double>(elapsed_ns);
    const uint64_t measured_millihz =
        measured_rate >= static_cast<long double>(UINT64_MAX) ? UINT64_MAX : static_cast<uint64_t>(measured_rate);
    const bool cadence_passed = measured_millihz >= limits.minimum_measured_millihz_;
    emit_result(frames, elapsed_ns, measured_millihz, limits.minimum_measured_millihz_, limits.frames_per_second_,
                limits.source_width_, limits.source_height_, cadence_passed);
    if (!cadence_passed) return fail(Result::qualification_failed, "measured_cadence");
    return code(Result::success);
}
