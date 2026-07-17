#include "backends/d3d12/directml_inference.hpp"
#include "backends/d3d12/graphics_device.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using Microsoft::WRL::ComPtr;
using saccade::backend::d3d12::DirectMlBindingDesc;
using saccade::backend::d3d12::DirectMlInference;
using saccade::backend::d3d12::DirectMlSessionDesc;
using saccade::backend::d3d12::GraphicsDevice;
using saccade::backend::d3d12::TensorElementType;

constexpr std::array<uint8_t, 162> model{
    0x08, 0x07, 0x12, 0x0c, 0x73, 0x61, 0x63, 0x63, 0x61, 0x64, 0x65, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x3a, 0x89,
    0x01, 0x0a, 0x19, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x0a, 0x03, 0x6f, 0x6e, 0x65, 0x12, 0x06, 0x6f,
    0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x03, 0x41, 0x64, 0x64, 0x12, 0x18, 0x73, 0x61, 0x63, 0x63, 0x61, 0x64,
    0x65, 0x5f, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x6d, 0x6c, 0x5f, 0x62, 0x69, 0x6e, 0x64, 0x69, 0x6e, 0x67,
    0x2a, 0x0f, 0x08, 0x01, 0x10, 0x01, 0x22, 0x04, 0x00, 0x00, 0x80, 0x3f, 0x42, 0x03, 0x6f, 0x6e, 0x65, 0x5a,
    0x1f, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x16, 0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02,
    0x08, 0x01, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x20, 0x0a, 0x06,
    0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x16, 0x0a, 0x14, 0x08, 0x01, 0x12, 0x10, 0x0a, 0x02, 0x08, 0x01,
    0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x0d};

enum class TestResult : int {
    success,
    device_unavailable = 77,
    resource_failed = 2,
    session_failed,
    run_failed,
    output_failed,
    statistics_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

void report_runtime_module() noexcept {
    std::array<wchar_t, 1024> wide{};
    const HMODULE module = GetModuleHandleW(L"onnxruntime.dll");
    const DWORD wide_size =
        module == nullptr ? 0 : GetModuleFileNameW(module, wide.data(), static_cast<DWORD>(wide.size()));
    if (wide_size == 0 || wide_size >= wide.size()) return;
    std::array<char, 2048> utf8{};
    const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide_size),
                                              utf8.data(), static_cast<int>(utf8.size() - 2U), nullptr, nullptr);
    if (utf8_size <= 0) return;
    utf8[static_cast<size_t>(utf8_size)] = '\n';
    DWORD written = 0;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), utf8.data(), static_cast<DWORD>(utf8_size + 1), &written, nullptr);
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
    const bool complete =
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 1'000) == WAIT_OBJECT_0;
    (void)CloseHandle(event);
    return complete;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(float) * 4U;
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

bool upload_input(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* input) noexcept {
    const ComPtr<ID3D12Resource> upload =
        create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (upload == nullptr) return false;
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return false;
    constexpr std::array<float, 4> values{1.0F, 2.0F, 3.0F, 4.0F};
    std::memcpy(mapped, values.data(), sizeof(values));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf())))) {
        return false;
    }
    commands->CopyResource(input, upload.Get());
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);
    return execute_and_wait(device, queue, commands.Get());
}

bool read_output(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* output) noexcept {
    const ComPtr<ID3D12Resource> readback =
        create_buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (readback == nullptr) return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf())))) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = output;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyResource(readback.Get(), output);
    if (!execute_and_wait(device, queue, commands.Get())) return false;
    void* mapped = nullptr;
    D3D12_RANGE range{0, sizeof(float) * 4U};
    if (FAILED(readback->Map(0, &range, &mapped))) return false;
    std::array<float, 4> values{};
    std::memcpy(values.data(), mapped, sizeof(values));
    D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    constexpr std::array<float, 4> expected{2.0F, 3.0F, 4.0F, 5.0F};
    return values == expected;
}

} // namespace

int main() {
    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return result(TestResult::device_unavailable);
    }
    const ComPtr<ID3D12Resource> input =
        create_buffer(graphics.device(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const ComPtr<ID3D12Resource> output =
        create_buffer(graphics.device(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (input == nullptr || output == nullptr || !upload_input(graphics.device(), graphics.queue(), input.Get())) {
        return result(TestResult::resource_failed);
    }
    DirectMlBindingDesc input_binding{};
    input_binding.name = "input";
    input_binding.resource = input.Get();
    input_binding.byte_size = sizeof(float) * 4U;
    input_binding.shape = {1, 1, 2, 2};
    input_binding.rank = 4;
    input_binding.element_type = TensorElementType::fp32;
    DirectMlBindingDesc output_binding = input_binding;
    output_binding.name = "output";
    output_binding.resource = output.Get();
    DirectMlInference inference;
    const DirectMlSessionDesc session{{model.data(), model.size()}, &input_binding, &output_binding, 1, 1};
    if (inference.initialize(graphics.device(), graphics.queue(), session) != SACCADE_OK) {
        report_runtime_module();
        return result(TestResult::session_failed);
    }
    if (inference.run() != SACCADE_OK || inference.run() != SACCADE_OK) {
        return result(TestResult::run_failed);
    }
    if (inference.synchronize_outputs() != SACCADE_OK) {
        return result(TestResult::run_failed);
    }
    if (!read_output(graphics.device(), graphics.queue(), output.Get())) {
        return result(TestResult::output_failed);
    }
    const auto stats = inference.stats();
    if (stats.runs != 2 || stats.failures != 0 || stats.model_bytes != model.size() ||
        stats.input_bytes != sizeof(float) * 4U || stats.output_bytes != sizeof(float) * 4U || stats.input_count != 1 ||
        stats.output_count != 1) {
        return result(TestResult::statistics_failed);
    }
    return result(TestResult::success);
}
