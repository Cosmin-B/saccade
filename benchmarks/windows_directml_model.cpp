#include "backends/d3d12/directml_inference.hpp"
#include "backends/d3d12/graphics_device.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;
using saccade::backend::d3d12::DevicePreference;
using saccade::backend::d3d12::DirectMlBindingDesc;
using saccade::backend::d3d12::DirectMlInference;
using saccade::backend::d3d12::DirectMlSessionDesc;
using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::TensorElementType;

constexpr uint32_t input_width = 1280;
constexpr uint32_t input_height = 768;
constexpr uint32_t input_channels = 3;
constexpr uint32_t candidate_capacity = 1024;
constexpr uint32_t candidate_components = 6;
constexpr uint32_t default_warmup_runs = 3;
constexpr uint32_t default_measured_runs = 30;
constexpr uint32_t maximum_runs = 1000;
constexpr size_t fp16_bytes = 2;
constexpr size_t input_bytes = static_cast<size_t>(input_width) * input_height * input_channels * fp16_bytes;
constexpr size_t output_bytes = static_cast<size_t>(candidate_capacity) * candidate_components * fp16_bytes;

enum class ExitCode : int {
    success,
    usage,
    file_open,
    file_size,
    file_mapping,
    device,
    resource,
    upload,
    session,
    warmup,
    run,
    timing
};

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

struct MappedFile {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
    const uint8_t* bytes_ = nullptr;
    size_t size_ = 0;

    ~MappedFile() {
        if (bytes_ != nullptr) (void)UnmapViewOfFile(bytes_);
        if (mapping_ != nullptr) (void)CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) (void)CloseHandle(file_);
    }

    ExitCode open(const wchar_t* path) noexcept {
        file_ =
            CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return ExitCode::file_open;

        LARGE_INTEGER size{};
        if (GetFileSizeEx(file_, &size) == 0 || size.QuadPart <= 0 ||
            static_cast<uint64_t>(size.QuadPart) > static_cast<uint64_t>(SIZE_MAX)) {
            return ExitCode::file_size;
        }

        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping_ == nullptr) return ExitCode::file_mapping;

        bytes_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (bytes_ == nullptr) return ExitCode::file_mapping;

        size_ = static_cast<size_t>(size.QuadPart);
        return ExitCode::success;
    }
};

bool parse_count(const wchar_t* text, uint32_t* output) noexcept {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0 || value > maximum_runs) return false;
    *output = static_cast<uint32_t>(value);
    return true;
}

bool parse_device(const wchar_t* text, DevicePreference* output) noexcept {
    if (std::wcscmp(text, L"hardware") == 0) {
        *output = DevicePreference::hardware_only;
    } else if (std::wcscmp(text, L"software") == 0) {
        *output = DevicePreference::software_only;
    } else {
        return false;
    }
    return true;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, size_t bytes, D3D12_HEAP_TYPE heap,
                                     D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(buffer.GetAddressOf())))) {
        return {};
    }
    return buffer;
}

bool execute(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands) noexcept {
    if (FAILED(commands->Close())) return false;

    ID3D12CommandList* lists[]{commands};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))) ||
        FAILED(queue->Signal(fence.Get(), 1))) {
        return false;
    }

    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return false;

    const bool completed =
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 10'000) == WAIT_OBJECT_0;
    (void)CloseHandle(event);
    return completed;
}

bool clear_input(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* input) noexcept {
    const ComPtr<ID3D12Resource> upload = create_buffer(device, input_bytes, D3D12_HEAP_TYPE_UPLOAD,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (upload == nullptr) return false;

    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    std::memset(mapped, 0, input_bytes);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf())))) {
        return false;
    }

    commands->CopyBufferRegion(input, 0, upload.Get(), 0, input_bytes);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);

    return execute(device, queue, commands.Get());
}

uint64_t private_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) != 0
               ? counters.PrivateUsage
               : 0;
}

double milliseconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) noexcept {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

double percentile(const std::array<double, maximum_runs>& values, uint32_t count, uint32_t numerator,
                  uint32_t denominator) noexcept {
    const uint32_t index = std::min(count - 1U, ((count - 1U) * numerator + denominator / 2U) / denominator);
    return values[index];
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 5) return exit_code(ExitCode::usage);

    uint32_t measured_runs = default_measured_runs;
    uint32_t warmup_runs = default_warmup_runs;
    DevicePreference device_preference = DevicePreference::hardware_only;
    if ((argc >= 3 && !parse_count(argv[2], &measured_runs)) || (argc >= 4 && !parse_count(argv[3], &warmup_runs)) ||
        (argc == 5 && !parse_device(argv[4], &device_preference))) {
        return exit_code(ExitCode::usage);
    }

    MappedFile model;
    const ExitCode opened = model.open(argv[1]);
    if (opened != ExitCode::success) return exit_code(opened);

    GraphicsDevice graphics;
    if (graphics.initialize(device_preference) != SACCADE_OK) return exit_code(ExitCode::device);

    const ComPtr<ID3D12Resource> input =
        create_buffer(graphics.device(), input_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const ComPtr<ID3D12Resource> output =
        create_buffer(graphics.device(), output_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (input == nullptr || output == nullptr) return exit_code(ExitCode::resource);
    if (!clear_input(graphics.device(), graphics.queue(), input.Get())) return exit_code(ExitCode::upload);

    DirectMlBindingDesc input_binding{};
    input_binding.name = "input";
    input_binding.resource = input.Get();
    input_binding.byte_size = input_bytes;
    input_binding.shape = {1, input_channels, input_height, input_width};
    input_binding.rank = 4;
    input_binding.element_type = TensorElementType::fp16;

    DirectMlBindingDesc output_binding{};
    output_binding.name = "candidates";
    output_binding.resource = output.Get();
    output_binding.byte_size = output_bytes;
    output_binding.shape = {candidate_capacity, candidate_components};
    output_binding.rank = 2;
    output_binding.element_type = TensorElementType::fp16;

    LARGE_INTEGER frequency{};
    LARGE_INTEGER session_start{};
    LARGE_INTEGER session_end{};
    if (QueryPerformanceFrequency(&frequency) == 0 || QueryPerformanceCounter(&session_start) == 0) {
        return exit_code(ExitCode::timing);
    }

    const uint64_t memory_before = private_bytes();
    DirectMlInference inference;
    const DirectMlSessionDesc session{{model.bytes_, model.size_}, &input_binding, &output_binding, 1, 1};
    if (inference.initialize(graphics.device(), graphics.queue(), session) != SACCADE_OK) {
        return exit_code(ExitCode::session);
    }
    if (QueryPerformanceCounter(&session_end) == 0) return exit_code(ExitCode::timing);

    for (uint32_t index = 0; index < warmup_runs; ++index) {
        if (inference.run() != SACCADE_OK || inference.synchronize_outputs() != SACCADE_OK) {
            return exit_code(ExitCode::warmup);
        }
    }

    std::array<double, maximum_runs> samples{};
    for (uint32_t index = 0; index < measured_runs; ++index) {
        LARGE_INTEGER start{};
        LARGE_INTEGER end{};
        if (QueryPerformanceCounter(&start) == 0 || inference.run() != SACCADE_OK ||
            inference.synchronize_outputs() != SACCADE_OK || QueryPerformanceCounter(&end) == 0) {
            return exit_code(ExitCode::run);
        }
        samples[index] = milliseconds(start, end, frequency);
    }

    const uint64_t memory_after = private_bytes();
    std::sort(samples.begin(), samples.begin() + measured_runs);
    const double median = percentile(samples, measured_runs, 50, 100);
    const double p95 = percentile(samples, measured_runs, 95, 100);
    const double p99 = percentile(samples, measured_runs, 99, 100);
    const auto stats = inference.stats();

    std::printf("{\n"
                "  \"device\": \"%s\",\n"
                "  \"model_bytes\": %zu,\n"
                "  \"input_bytes\": %zu,\n"
                "  \"output_bytes\": %zu,\n"
                "  \"session_ms\": %.3f,\n"
                "  \"warmup_runs\": %u,\n"
                "  \"measured_runs\": %u,\n"
                "  \"minimum_ms\": %.3f,\n"
                "  \"median_ms\": %.3f,\n"
                "  \"p95_ms\": %.3f,\n"
                "  \"p99_ms\": %.3f,\n"
                "  \"maximum_ms\": %.3f,\n"
                "  \"private_bytes_before\": %llu,\n"
                "  \"private_bytes_after\": %llu,\n"
                "  \"runtime_runs\": %llu,\n"
                "  \"runtime_failures\": %llu\n"
                "}\n",
                graphics.software_device() ? "software" : "hardware", model.size_, input_bytes, output_bytes,
                milliseconds(session_start, session_end, frequency), warmup_runs, measured_runs, samples[0], median,
                p95, p99, samples[measured_runs - 1U], static_cast<unsigned long long>(memory_before),
                static_cast<unsigned long long>(memory_after), static_cast<unsigned long long>(stats.runs),
                static_cast<unsigned long long>(stats.failures));

    return exit_code(ExitCode::success);
}
