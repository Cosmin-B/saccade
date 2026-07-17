#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/preprocessor.hpp"

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
using saccade::backend::d3d12::ImagePreprocessor;
using saccade::backend::image::PreprocessSubmission;
using saccade::backend::image::SourceRegion;
using saccade::backend::image::TensorFormat;
using saccade::backend::image::TensorSpec;
using saccade::backend::image::TensorView;

enum class TestResult : int {
    success,
    usage,
    device_unavailable = 77,
    texture_failed = 2,
    fp16_failed,
    int8_failed,
    device_loss_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

bool execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* commands) noexcept {
    if (FAILED(commands->Close())) return false;
    ID3D12CommandList* command_lists[]{commands};
    queue->ExecuteCommandLists(1, command_lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))) ||
        FAILED(queue->Signal(fence.Get(), 1))) {
        return false;
    }
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) return false;
    const bool completed =
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 1'000) == WAIT_OBJECT_0;
    (void)CloseHandle(event);
    return completed;
}

ComPtr<ID3D12Resource> make_texture(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = 2;
    texture_desc.Height = 2;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(texture.GetAddressOf())))) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t upload_bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_bytes);
    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = upload_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(upload.GetAddressOf())))) {
        return {};
    }
    constexpr std::array<uint8_t, 16> pixels{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    void* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, &mapped))) return {};
    auto* destination = static_cast<uint8_t*>(mapped);
    std::memcpy(destination, pixels.data(), 8);
    std::memcpy(destination + footprint.Footprint.RowPitch, pixels.data() + 8, 8);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf())))) {
        return {};
    }
    D3D12_TEXTURE_COPY_LOCATION destination_copy{};
    destination_copy.pResource = texture.Get();
    destination_copy.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source_copy{};
    source_copy.pResource = upload.Get();
    source_copy.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_copy.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination_copy, 0, 0, 0, &source_copy, nullptr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);
    return execute_and_wait(device, queue, commands.Get()) ? texture : ComPtr<ID3D12Resource>{};
}

template <size_t Size>
bool read_buffer(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source,
                 std::array<uint8_t, Size>* output) noexcept {
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = Size;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(readback.GetAddressOf()))) ||
        FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commands.GetAddressOf())))) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = source;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);
    commands->CopyResource(readback.Get(), source);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    if (!execute_and_wait(device, queue, commands.Get())) return false;
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, Size};
    if (FAILED(readback->Map(0, &read_range, &mapped))) return false;
    std::memcpy(output->data(), mapped, Size);
    D3D12_RANGE written_range{0, 0};
    readback->Unmap(0, &written_range);
    return true;
}

bool run_fp16(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture, const char* shaders) noexcept {
    TensorSpec spec{};
    spec.width = 2;
    spec.height = 2;
    spec.format = TensorFormat::planar_fp16;
    ImagePreprocessor preprocessor;
    PreprocessSubmission submission{};
    TensorView view{};
    if (preprocessor.initialize(device, queue, shaders, spec) != SACCADE_OK ||
        preprocessor.submit(texture, 2, 2, {}, 1, 2, &submission) != SACCADE_OK ||
        preprocessor.wait(&submission, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        preprocessor.tensor(&submission, &view) != SACCADE_OK || view.byte_size != 24 || view.plane_stride_bytes != 8 ||
        view.width != 2 || view.height != 2 || view.channels != 3 || view.format != TensorFormat::planar_fp16) {
        return false;
    }
    std::array<uint8_t, 24> bytes{};
    if (!read_buffer(device, queue, static_cast<ID3D12Resource*>(view.buffer), &bytes)) {
        return false;
    }
    std::array<uint16_t, 12> actual{};
    std::memcpy(actual.data(), bytes.data(), bytes.size());
    constexpr std::array<uint16_t, 12> expected{0x3c00, 0, 0, 0x3c00, 0, 0x3c00, 0, 0x3c00, 0, 0, 0x3c00, 0x3c00};
    const auto stats = preprocessor.stats();
    return actual == expected && stats.submissions == 1 && stats.completions == 1 &&
           stats.output_bytes == view.byte_size;
}

bool run_int8(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture, const char* shaders) noexcept {
    TensorSpec spec{};
    spec.width = 2;
    spec.height = 2;
    spec.format = TensorFormat::planar_int8;
    spec.channel_scale = {255.0F, 255.0F, 255.0F};
    spec.channel_bias = {-128.0F, -128.0F, -128.0F};
    ImagePreprocessor preprocessor;
    PreprocessSubmission first{};
    TensorView view{};
    if (preprocessor.initialize(device, queue, shaders, spec) != SACCADE_OK ||
        preprocessor.submit(texture, 2, 2, {}, 9, 10, &first) != SACCADE_OK ||
        preprocessor.wait(&first, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        preprocessor.tensor(&first, &view) != SACCADE_OK || view.byte_size != 12) {
        return false;
    }
    std::array<uint8_t, 12> bytes{};
    if (!read_buffer(device, queue, static_cast<ID3D12Resource*>(view.buffer), &bytes)) {
        return false;
    }
    constexpr std::array<int8_t, 12> expected{127, -128, -128, 127, -128, 127, -128, 127, -128, -128, 127, 127};
    if (std::memcmp(bytes.data(), expected.data(), bytes.size()) != 0) {
        return false;
    }
    PreprocessSubmission second{};
    const SourceRegion right_column{1, 0, 1, 2};
    if (preprocessor.submit(texture, 2, 2, right_column, 10, 10, &second) != SACCADE_OK ||
        preprocessor.wait(&second, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        preprocessor.tensor(&first, &view) != SACCADE_ERROR_INVALID_ARGUMENT ||
        preprocessor.tensor(&second, &view) != SACCADE_OK ||
        !read_buffer(device, queue, static_cast<ID3D12Resource*>(view.buffer), &bytes)) {
        return false;
    }
    constexpr std::array<int8_t, 12> cropped{-128, -128, 127, -128, 127, -128, 127, -128, -128, -128, 127, -128};
    return std::memcmp(bytes.data(), cropped.data(), bytes.size()) == 0;
}

bool run_device_loss(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture,
                     const char* shaders) noexcept {
    TensorSpec spec{};
    spec.width = 2;
    spec.height = 2;
    spec.format = TensorFormat::planar_fp16;
    ImagePreprocessor preprocessor;
    PreprocessSubmission submission{};
    ComPtr<ID3D12Device5> removable;
    if (preprocessor.initialize(device, queue, shaders, spec) != SACCADE_OK ||
        preprocessor.submit(texture, 2, 2, {}, 20, 21, &submission) != SACCADE_OK ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(removable.GetAddressOf())))) {
        return false;
    }
    removable->RemoveDevice();
    return preprocessor.poll(&submission) == SACCADE_ERROR_BACKEND &&
           preprocessor.wait(&submission, UINT64_C(1'000'000)) == SACCADE_ERROR_BACKEND;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return result(TestResult::usage);
    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return result(TestResult::device_unavailable);
    }
    const ComPtr<ID3D12Resource> texture = make_texture(graphics.device(), graphics.queue());
    if (texture == nullptr) return result(TestResult::texture_failed);
    if (!run_fp16(graphics.device(), graphics.queue(), texture.Get(), argv[1])) {
        return result(TestResult::fp16_failed);
    }
    if (!run_int8(graphics.device(), graphics.queue(), texture.Get(), argv[1])) {
        return result(TestResult::int8_failed);
    }
    if (!run_device_loss(graphics.device(), graphics.queue(), texture.Get(), argv[1])) {
        return result(TestResult::device_loss_failed);
    }
    return result(TestResult::success);
}
