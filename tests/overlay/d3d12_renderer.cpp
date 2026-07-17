#include "backends/d3d12/graphics_device.hpp"
#include "backends/d3d12/overlay_renderer.hpp"
#include "overlay/packet.hpp"

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
using saccade::backend::d3d12::OverlayRenderer;
using saccade::backend::d3d12::OverlaySubmission;

constexpr size_t max_packet_size = sizeof(SaccadeOverlayPacketHeader) +
                                   SACCADE_OVERLAY_MAX_TARGETS * sizeof(SaccadeOverlayTarget) +
                                   sizeof(SaccadeOverlayStyle);
constexpr size_t max_instance_count = SACCADE_OVERLAY_MAX_TARGETS * 5U + 1U;

alignas(64) std::array<uint8_t, max_packet_size> packet_bytes{};
alignas(64) std::array<SaccadeOverlayRect, max_instance_count> expected_rects{};
alignas(64) std::array<SaccadeOverlayInstanceMeta, max_instance_count> expected_metadata{};
alignas(64) std::array<SaccadeOverlayRect, max_instance_count> actual_rects{};
alignas(64) std::array<SaccadeOverlayInstanceMeta, max_instance_count> actual_metadata{};

enum class TestResult : int {
    success,
    usage,
    device_unavailable = 77,
    renderer_failed = 2,
    statistics_failed,
    device_loss_failed
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

template <typename T> void store(size_t offset, const T& value) noexcept {
    std::memcpy(packet_bytes.data() + offset, &value, sizeof(value));
}

SaccadeSpanU8 make_packet(uint32_t count, uint64_t scene_epoch, bool animated = false) noexcept {
    const size_t targets_offset = sizeof(SaccadeOverlayPacketHeader);
    const size_t styles_offset = targets_offset + static_cast<size_t>(count) * sizeof(SaccadeOverlayTarget);
    const size_t total = styles_offset + sizeof(SaccadeOverlayStyle);
    SaccadeOverlayPacketHeader header{};
    header.struct_size = sizeof(header);
    header.packet_version = SACCADE_OVERLAY_PACKET_VERSION;
    header.target_count = count;
    header.target_stride = sizeof(SaccadeOverlayTarget);
    header.style_count = 1;
    header.style_stride = sizeof(SaccadeOverlayStyle);
    header.scene_epoch = scene_epoch;
    header.transform_epoch = 7;
    header.targets_offset = targets_offset;
    header.styles_offset = styles_offset;
    store(0, header);
    for (uint32_t index = 0; index < count; ++index) {
        SaccadeOverlayTarget target{};
        target.target_id = static_cast<uint64_t>(index) + 1U;
        target.x_q3 = static_cast<uint16_t>((index % 100U) * 32U + 8U);
        target.y_q3 = static_cast<uint16_t>(((index / 100U) % 100U) * 24U + 8U);
        target.width_q3 = 24;
        target.height_q3 = 16;
        target.label_x_q3 = target.x_q3;
        target.label_y_q3 = static_cast<uint16_t>(target.y_q3 + 18U);
        target.confidence_q16 = UINT16_MAX;
        target.glyphs[0] = static_cast<uint8_t>(index % 32U);
        target.glyphs[1] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyphs[2] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyphs[3] = SACCADE_OVERLAY_GLYPH_NONE;
        target.glyph_count = 1;
        store(targets_offset + static_cast<size_t>(index) * sizeof(target), target);
    }
    SaccadeOverlayStyle style{};
    style.target_outline_rgba8 = UINT32_C(0xff2020ff);
    style.label_background_rgba8 = UINT32_C(0x101010e0);
    style.label_foreground_rgba8 = UINT32_C(0xffffffff);
    style.active_fill_rgba8 = UINT32_C(0x20a0ff60);
    style.active_outline_rgba8 = UINT32_C(0x20a0ffff);
    style.target_stroke_q3 = 4;
    style.target_radius_q3 = 4;
    style.label_height_q3 = 16;
    style.label_radius_q3 = 4;
    style.label_padding_x_q3 = 4;
    style.glyph_width_q3 = 8;
    style.glyph_height_q3 = 8;
    style.glyph_advance_q3 = 10;
    style.active_stroke_q3 = 4;
    style.flags = animated ? SACCADE_OVERLAY_STYLE_ANIMATED : 0;
    store(styles_offset, style);
    return {packet_bytes.data(), total};
}

bool compare_case(OverlayRenderer* renderer, uint32_t count, uint64_t scene_epoch, bool active) noexcept {
    const SaccadeSpanU8 packet = make_packet(count, scene_epoch);
    saccade::overlay::PacketView view{};
    if (saccade::overlay::validate_packet(packet, &view) != SACCADE_OK) {
        return false;
    }
    size_t static_count = 0;
    if (saccade::overlay::expand_static(view, {expected_rects.data(), expected_metadata.data(), expected_rects.size()},
                                        &static_count) != SACCADE_OK) {
        return false;
    }
    size_t active_count = 0;
    const uint32_t active_index = active ? count - 1U : SACCADE_OVERLAY_ACTIVE_TARGET_NONE;
    if (saccade::overlay::expand_active(view, active_index,
                                        {expected_rects.data() + static_count, expected_metadata.data() + static_count,
                                         expected_rects.size() - static_count},
                                        &active_count) != SACCADE_OK) {
        return false;
    }
    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = scene_epoch;
    frame.transform_epoch = 7;
    frame.packet = packet;
    if (active) {
        frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
        frame.active_target_index = active_index;
    }
    OverlaySubmission submission{};
    if (renderer->submit(frame, &submission) != SACCADE_OK ||
        renderer->wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return false;
    }
    size_t actual_count = 0;
    if (renderer->copy_instances(submission, {actual_rects.data(), actual_metadata.data(), actual_rects.size()},
                                 &actual_count) != SACCADE_OK ||
        actual_count != static_count + active_count) {
        return false;
    }
    return std::memcmp(actual_rects.data(), expected_rects.data(), actual_count * sizeof(SaccadeOverlayRect)) == 0 &&
           std::memcmp(actual_metadata.data(), expected_metadata.data(),
                       actual_count * sizeof(SaccadeOverlayInstanceMeta)) == 0;
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

bool texture_nonzero(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* texture,
                     const D3D12_RESOURCE_DESC& texture_desc) noexcept {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t readback_bytes = 0;
    device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_bytes);
    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readback_desc{};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = readback_bytes;
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
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands->ResourceBarrier(1, &barrier);
    if (!execute_and_wait(device, queue, commands.Get())) return false;

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(readback_bytes)};
    if (FAILED(readback->Map(0, &range, &mapped))) return false;
    bool nonzero = false;
    for (uint32_t y = 0; y < static_cast<uint32_t>(texture_desc.Height) && !nonzero; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < static_cast<uint32_t>(texture_desc.Width) * 4U; ++x) {
            if (row[x] != 0) {
                nonzero = true;
                break;
            }
        }
    }
    D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return nonzero;
}

bool render_case(ID3D12Device* device, ID3D12CommandQueue* queue, OverlayRenderer* renderer) noexcept {
    constexpr uint32_t width = 512;
    constexpr uint32_t height = 512;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                               D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                               IID_PPV_ARGS(texture.GetAddressOf()))) ||
        FAILED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap.GetAddressOf())))) {
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(texture.Get(), nullptr, rtv);
    const SaccadeSpanU8 packet = make_packet(100, 20);
    SaccadeOverlayFrameDesc frame{};
    frame.struct_size = sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.scene_epoch = 20;
    frame.transform_epoch = 7;
    frame.packet = packet;
    frame.flags = SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET;
    frame.active_target_index = 0;
    OverlaySubmission submission{};
    if (renderer->render(frame, {texture.Get(), rtv, 0, width, height}, &submission) != SACCADE_OK ||
        renderer->wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return false;
    }
    if (!texture_nonzero(device, queue, texture.Get(), texture_desc)) return false;

    frame.packet = make_packet(100, 21, true);
    frame.scene_epoch = 21;
    if (renderer->render(frame, {texture.Get(), rtv, UINT64_C(1'000'000'000), width, height}, &submission) !=
            SACCADE_OK ||
        renderer->wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        texture_nonzero(device, queue, texture.Get(), texture_desc)) {
        return false;
    }
    if (renderer->render(frame, {texture.Get(), rtv, UINT64_C(1'200'000'000), width, height}, &submission) !=
            SACCADE_OK ||
        renderer->wait(submission, UINT64_C(1'000'000'000)) != SACCADE_OK ||
        !texture_nonzero(device, queue, texture.Get(), texture_desc)) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return result(TestResult::usage);
    GraphicsDevice graphics;
    if (graphics.initialize() != SACCADE_OK) {
        return result(TestResult::device_unavailable);
    }
    OverlayRenderer renderer;
    if (renderer.initialize(graphics.device(), graphics.queue(), argv[1]) != SACCADE_OK ||
        !compare_case(&renderer, 1, 1, false) || !compare_case(&renderer, 100, 2, true) ||
        !compare_case(&renderer, 10'000, 3, true) || !render_case(graphics.device(), graphics.queue(), &renderer)) {
        return result(TestResult::renderer_failed);
    }
    const auto stats = renderer.stats();
    SaccadeMemoryStats memory{};
    memory.struct_size = sizeof(memory);
    memory.api_version = SACCADE_API_VERSION;
    if (stats.slot_count != 3 || stats.target_capacity != SACCADE_OVERLAY_MAX_TARGETS ||
        stats.instance_capacity != max_instance_count || stats.submissions != 6 || stats.static_dispatches != 6 ||
        stats.active_dispatches != 6 || stats.rendered_frames != 3 || stats.draw_calls != 3 ||
        renderer.memory_stats(&memory) != SACCADE_OK || memory.device_owned == 0) {
        return result(TestResult::statistics_failed);
    }
    SaccadeOverlayFrameDesc removed_frame{};
    removed_frame.struct_size = sizeof(removed_frame);
    removed_frame.api_version = SACCADE_API_VERSION;
    removed_frame.scene_epoch = 30;
    removed_frame.transform_epoch = 7;
    removed_frame.packet = make_packet(100, removed_frame.scene_epoch);
    OverlaySubmission removed_submission{};
    ComPtr<ID3D12Device5> removable;
    if (renderer.submit(removed_frame, &removed_submission) != SACCADE_OK ||
        FAILED(graphics.device()->QueryInterface(IID_PPV_ARGS(removable.GetAddressOf())))) {
        return result(TestResult::device_loss_failed);
    }
    removable->RemoveDevice();
    bool complete = true;
    if (renderer.poll(removed_submission, &complete) != SACCADE_ERROR_BACKEND || complete ||
        renderer.wait(removed_submission, UINT64_C(1'000'000)) != SACCADE_ERROR_BACKEND) {
        return result(TestResult::device_loss_failed);
    }
    return result(TestResult::success);
}
