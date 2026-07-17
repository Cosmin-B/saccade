#include "backends/d3d12/overlay_renderer.hpp"

#include "core/stack_string_builder.hpp"
#include "overlay/packet.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace saccade::backend::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t slot_count = 3;
constexpr uint32_t instances_per_target = 5;
constexpr uint32_t instance_capacity = SACCADE_OVERLAY_MAX_TARGETS * instances_per_target + 1U;
constexpr uint32_t target_bytes = SACCADE_OVERLAY_MAX_TARGETS * sizeof(SaccadeOverlayTarget);
constexpr uint32_t style_bytes = SACCADE_OVERLAY_MAX_STYLES * sizeof(SaccadeOverlayStyle);
constexpr uint32_t rect_bytes = instance_capacity * sizeof(SaccadeOverlayRect);
constexpr uint32_t metadata_bytes = instance_capacity * sizeof(SaccadeOverlayInstanceMeta);
constexpr uint32_t descriptors_per_slot = 10;
constexpr size_t maximum_shader_bytes = 64 * 1024;
constexpr D3D12_RESOURCE_STATES shader_resource_state =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

struct ExpandParameters {
    uint32_t target_count = 0;
    uint32_t active_target_index = 0;
    uint32_t static_instance_count = 0;
    uint32_t has_active_target = 0;
};

struct DisplayConstants {
    float inverse_drawable_width = 0;
    float inverse_drawable_height = 0;
    float animation_time_seconds = 0;
    float scene_age_seconds = 0;
};

static_assert(sizeof(ExpandParameters) == 16);
static_assert(sizeof(DisplayConstants) == 16);

bool frame_valid(const SaccadeOverlayFrameDesc& frame) noexcept {
    if (frame.struct_size != sizeof(frame) || frame.api_version != SACCADE_API_VERSION ||
        (frame.flags & ~SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0) {
        return false;
    }
    for (uint64_t value : frame.reserved) {
        if (value != 0) return false;
    }
    return true;
}

bool load_shader(const char* directory, const char* name, std::array<std::byte, maximum_shader_bytes>* bytes,
                 size_t* byte_count) noexcept {
    core::StackStringBuilder<1024> path;
    if (directory == nullptr || name == nullptr || bytes == nullptr || byte_count == nullptr ||
        !path.append(directory) ||
        (!path.empty() && path.view().back() != '/' && path.view().back() != '\\' && !path.append('\\')) ||
        !path.append(name)) {
        return false;
    }
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool loaded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0 &&
                        size.QuadPart <= static_cast<LONGLONG>(bytes->size()) &&
                        ReadFile(file, bytes->data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) != FALSE &&
                        read == static_cast<DWORD>(size.QuadPart);
    (void)CloseHandle(file);
    if (!loaded) return false;
    *byte_count = read;
    return true;
}

bool create_buffer(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                   D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>* output) noexcept {
    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return SUCCEEDED(device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                     IID_PPV_ARGS(output->GetAddressOf())));
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap* heap, UINT descriptor_size, uint32_t index) noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE result = heap->GetCPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<SIZE_T>(descriptor_size) * index;
    return result;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap* heap, UINT descriptor_size, uint32_t index) noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE result = heap->GetGPUDescriptorHandleForHeapStart();
    result.ptr += static_cast<UINT64>(descriptor_size) * index;
    return result;
}

DWORD timeout_milliseconds(uint64_t timeout_ns) noexcept {
    constexpr uint64_t nanoseconds_per_millisecond = 1'000'000;
    if (timeout_ns == 0) return 0;
    const uint64_t milliseconds =
        timeout_ns / nanoseconds_per_millisecond + (timeout_ns % nanoseconds_per_millisecond != 0 ? 1U : 0U);
    return static_cast<DWORD>(std::min<uint64_t>(milliseconds, INFINITE - 1U));
}

void fill_builtin_atlas(uint8_t* pixels, size_t row_pitch) noexcept {
    constexpr uint64_t glyph_bits[overlay::glyph_atlas_capacity] = {
        0x4631FC62EULL, 0x3E317C62FULL, 0x78210843EULL, 0x3E318C62FULL, 0x7C217843FULL, 0x04217843FULL, 0x7A31E843EULL,
        0x4631FC631ULL, 0x7C842109FULL, 0x19294211CULL, 0x452519531ULL, 0x7C2108421ULL, 0x4631AD771ULL, 0x4631CD671ULL,
        0x3A318C62EULL, 0x04217C62FULL, 0x59358C62EULL, 0x45257C62FULL, 0x3E107043EULL, 0x10842109FULL, 0x3A318C631ULL,
        0x11518C631ULL, 0x2AB5AC631ULL, 0x462A22A31ULL, 0x108422A31ULL, 0x7C222221FULL, 0x3A33AE62EULL, 0x3884210C4ULL,
        0x7C444422EULL, 0x3E107420FULL, 0x211F4A988ULL, 0x3E107843FULL};
    std::memset(pixels, 0, row_pitch * overlay::glyph_atlas_height);
    for (uint32_t glyph = 0; glyph < overlay::glyph_atlas_capacity; ++glyph) {
        const uint32_t cell_x = (glyph % overlay::glyph_atlas_columns) * overlay::glyph_atlas_cell_width;
        const uint32_t cell_y = (glyph / overlay::glyph_atlas_columns) * overlay::glyph_atlas_cell_height;
        for (uint32_t y = 0; y < 56; ++y) {
            const uint32_t row = y * 7U / 56U;
            for (uint32_t x = 0; x < 40; ++x) {
                const uint32_t column = x * 5U / 40U;
                if ((glyph_bits[glyph] & (UINT64_C(1) << (row * 5U + column))) != 0)
                    pixels[static_cast<size_t>(cell_y + y + 4U) * row_pitch + cell_x + x + 12U] = UINT8_MAX;
            }
        }
    }
}

} // namespace

struct OverlayRenderer::Impl {
    struct Slot {
        ComPtr<ID3D12Resource> targets_{};
        ComPtr<ID3D12Resource> styles_{};
        ComPtr<ID3D12Resource> rects_{};
        ComPtr<ID3D12Resource> metadata_{};
        ComPtr<ID3D12Resource> arguments_{};
        ComPtr<ID3D12Resource> rects_readback_{};
        ComPtr<ID3D12Resource> metadata_readback_{};
        ComPtr<ID3D12CommandAllocator> allocator_{};
        ComPtr<ID3D12GraphicsCommandList> commands_{};
        std::byte* targets_mapped_ = nullptr;
        std::byte* styles_mapped_ = nullptr;
        uint64_t sequence_ = 0;
        uint64_t fence_value_ = 0;
        uint64_t scene_epoch_ = 0;
        uint64_t transform_epoch_ = 0;
        uint32_t instance_count_ = 0;
        bool read_state_ = false;
    };

    ComPtr<ID3D12Device> device_{};
    ComPtr<ID3D12CommandQueue> queue_{};
    ComPtr<ID3D12RootSignature> compute_root_{};
    ComPtr<ID3D12RootSignature> render_root_{};
    ComPtr<ID3D12PipelineState> static_pipeline_{};
    ComPtr<ID3D12PipelineState> active_pipeline_{};
    ComPtr<ID3D12PipelineState> render_pipeline_{};
    ComPtr<ID3D12Resource> glyph_atlas_{};
    ComPtr<ID3D12CommandSignature> draw_signature_{};
    ComPtr<ID3D12DescriptorHeap> descriptors_{};
    ComPtr<ID3D12Fence> fence_{};
    HANDLE completion_event_ = nullptr;
    std::array<Slot, slot_count> slots_{};
    OverlayStats stats_{};
    overlay::PacketView validated_packet_{};
    const uint8_t* validated_bytes_ = nullptr;
    size_t validated_size_ = 0;
    DWORD owner_thread_ = 0;
    UINT descriptor_size_ = 0;
    uint64_t next_sequence_ = 1;
    uint64_t next_fence_value_ = 1;
    uint64_t animation_scene_epoch_ = 0;
    uint64_t animation_scene_start_ns_ = 0;
    uint32_t next_slot_ = 0;
    bool glyph_atlas_ready_ = false;

    [[nodiscard]] bool owns_thread() const noexcept { return owner_thread_ == GetCurrentThreadId(); }

    SaccadeResult fence_status(uint64_t value) const noexcept {
        if (value == 0) return SACCADE_OK;
        const uint64_t completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) return SACCADE_ERROR_BACKEND;
        return completed >= value ? SACCADE_OK : SACCADE_ERROR_BUSY;
    }

    SaccadeResult wait_fence(uint64_t value, uint64_t timeout_ns) const noexcept {
        SaccadeResult status = fence_status(value);
        if (status != SACCADE_ERROR_BUSY) return status;
        if (FAILED(fence_->SetEventOnCompletion(value, completion_event_))) {
            return SACCADE_ERROR_BACKEND;
        }
        const DWORD waited = WaitForSingleObject(completion_event_, timeout_milliseconds(timeout_ns));
        if (waited == WAIT_TIMEOUT) return SACCADE_ERROR_TIMEOUT;
        if (waited != WAIT_OBJECT_0) return SACCADE_ERROR_BACKEND;
        status = fence_status(value);
        return status == SACCADE_ERROR_BUSY ? SACCADE_ERROR_BACKEND : status;
    }

    [[nodiscard]] uint32_t descriptor_base(const Slot& slot) const noexcept {
        return static_cast<uint32_t>(&slot - slots_.data()) * descriptors_per_slot;
    }

    bool create_slot(Slot& slot) noexcept {
        const bool buffers =
            create_buffer(device_.Get(), target_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &slot.targets_) &&
            create_buffer(device_.Get(), style_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_GENERIC_READ, &slot.styles_) &&
            create_buffer(device_.Get(), rect_bytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          &slot.rects_) &&
            create_buffer(device_.Get(), metadata_bytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          &slot.metadata_) &&
            create_buffer(device_.Get(), sizeof(D3D12_DRAW_ARGUMENTS), D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          &slot.arguments_) &&
            create_buffer(device_.Get(), rect_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &slot.rects_readback_) &&
            create_buffer(device_.Get(), metadata_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                          D3D12_RESOURCE_STATE_COPY_DEST, &slot.metadata_readback_);

        if (!buffers || FAILED(slot.targets_->Map(0, nullptr, reinterpret_cast<void**>(&slot.targets_mapped_))) ||
            FAILED(slot.styles_->Map(0, nullptr, reinterpret_cast<void**>(&slot.styles_mapped_))) ||
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(slot.allocator_.GetAddressOf()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator_.Get(), nullptr,
                                              IID_PPV_ARGS(slot.commands_.GetAddressOf()))) ||
            FAILED(slot.commands_->Close())) {
            return false;
        }

        const uint32_t base = descriptor_base(slot);
        D3D12_SHADER_RESOURCE_VIEW_DESC raw_srv{};
        raw_srv.Format = DXGI_FORMAT_R32_TYPELESS;
        raw_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        raw_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw_srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        raw_srv.Buffer.NumElements = target_bytes / sizeof(uint32_t);
        device_->CreateShaderResourceView(slot.targets_.Get(), &raw_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base));
        raw_srv.Buffer.NumElements = style_bytes / sizeof(uint32_t);
        device_->CreateShaderResourceView(slot.styles_.Get(), &raw_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 1U));

        D3D12_UNORDERED_ACCESS_VIEW_DESC structured_uav{};
        structured_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        structured_uav.Buffer.NumElements = instance_capacity;
        structured_uav.Buffer.StructureByteStride = sizeof(SaccadeOverlayRect);
        device_->CreateUnorderedAccessView(slot.rects_.Get(), nullptr, &structured_uav,
                                           cpu_handle(descriptors_.Get(), descriptor_size_, base + 2U));
        structured_uav.Buffer.StructureByteStride = sizeof(SaccadeOverlayInstanceMeta);
        device_->CreateUnorderedAccessView(slot.metadata_.Get(), nullptr, &structured_uav,
                                           cpu_handle(descriptors_.Get(), descriptor_size_, base + 3U));
        D3D12_UNORDERED_ACCESS_VIEW_DESC raw_uav{};
        raw_uav.Format = DXGI_FORMAT_R32_TYPELESS;
        raw_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        raw_uav.Buffer.NumElements = sizeof(D3D12_DRAW_ARGUMENTS) / sizeof(uint32_t);
        raw_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device_->CreateUnorderedAccessView(slot.arguments_.Get(), nullptr, &raw_uav,
                                           cpu_handle(descriptors_.Get(), descriptor_size_, base + 4U));

        D3D12_SHADER_RESOURCE_VIEW_DESC structured_srv{};
        structured_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        structured_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        structured_srv.Buffer.NumElements = instance_capacity;
        structured_srv.Buffer.StructureByteStride = sizeof(SaccadeOverlayRect);
        device_->CreateShaderResourceView(slot.rects_.Get(), &structured_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 5U));
        structured_srv.Buffer.StructureByteStride = sizeof(SaccadeOverlayInstanceMeta);
        device_->CreateShaderResourceView(slot.metadata_.Get(), &structured_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 6U));
        raw_srv.Buffer.NumElements = target_bytes / sizeof(uint32_t);
        device_->CreateShaderResourceView(slot.targets_.Get(), &raw_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 7U));
        raw_srv.Buffer.NumElements = style_bytes / sizeof(uint32_t);
        device_->CreateShaderResourceView(slot.styles_.Get(), &raw_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 8U));
        D3D12_SHADER_RESOURCE_VIEW_DESC atlas_srv{};
        atlas_srv.Format = DXGI_FORMAT_R8_UNORM;
        atlas_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        atlas_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        atlas_srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(glyph_atlas_.Get(), &atlas_srv,
                                          cpu_handle(descriptors_.Get(), descriptor_size_, base + 9U));
        return true;
    }

    bool upload_glyph_atlas(const uint8_t* pixels) noexcept {
        for (const Slot& slot : slots_) {
            const SaccadeResult status = fence_status(slot.fence_value_);
            if (status == SACCADE_ERROR_BACKEND ||
                (status == SACCADE_ERROR_BUSY &&
                 wait_fence(slot.fence_value_, UINT64_C(1'000'000'000)) != SACCADE_OK)) {
                return false;
            }
        }

        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = overlay::glyph_atlas_width;
        texture_desc.Height = overlay::glyph_atlas_height;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        if (glyph_atlas_ == nullptr) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        IID_PPV_ARGS(glyph_atlas_.GetAddressOf())))) {
                return false;
            }
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 upload_bytes = 0;
        device_->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_bytes);
        ComPtr<ID3D12Resource> upload;
        if (!create_buffer(device_.Get(), upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, &upload)) {
            return false;
        }
        uint8_t* mapped = nullptr;
        if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) return false;
        if (pixels == nullptr) {
            fill_builtin_atlas(mapped + footprint.Offset, footprint.Footprint.RowPitch);
        } else {
            for (uint32_t row = 0; row < overlay::glyph_atlas_height; ++row) {
                std::memcpy(mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                            pixels + static_cast<size_t>(row) * overlay::glyph_atlas_width, overlay::glyph_atlas_width);
            }
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(allocator.GetAddressOf()))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(commands.GetAddressOf())))) {
            return false;
        }
        if (glyph_atlas_ready_) {
            D3D12_RESOURCE_BARRIER to_copy{};
            to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            to_copy.Transition.pResource = glyph_atlas_.Get();
            to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            commands->ResourceBarrier(1, &to_copy);
        }
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = glyph_atlas_.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER to_shader{};
        to_shader.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_shader.Transition.pResource = glyph_atlas_.Get();
        to_shader.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_shader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_shader.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commands->ResourceBarrier(1, &to_shader);
        if (FAILED(commands->Close())) return false;
        ID3D12CommandList* lists[]{commands.Get()};
        queue_->ExecuteCommandLists(1, lists);
        const uint64_t fence_value = next_fence_value_++;
        if (FAILED(queue_->Signal(fence_.Get(), fence_value)) ||
            wait_fence(fence_value, UINT64_C(1'000'000'000)) != SACCADE_OK) {
            return false;
        }
        glyph_atlas_ready_ = true;
        return true;
    }
};

static_assert(sizeof(OverlayRenderer::Impl) <= OverlayRenderer::storage_size);
static_assert(alignof(OverlayRenderer::Impl) <= 64);

OverlayRenderer::OverlayRenderer() noexcept = default;

OverlayRenderer::~OverlayRenderer() {
    if (!initialized_) return;
    Impl& state = impl();
    if (state.owns_thread()) {
        for (Impl::Slot& slot : state.slots_) {
            if (slot.fence_value_ != 0) {
                (void)state.wait_fence(slot.fence_value_, UINT64_C(1'000'000'000));
            }
            if (slot.targets_mapped_ != nullptr) slot.targets_->Unmap(0, nullptr);
            if (slot.styles_mapped_ != nullptr) slot.styles_->Unmap(0, nullptr);
        }
    }
    if (state.completion_event_ != nullptr) {
        (void)CloseHandle(state.completion_event_);
    }
    state.~Impl();
}

OverlayRenderer::Impl& OverlayRenderer::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const OverlayRenderer::Impl& OverlayRenderer::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult OverlayRenderer::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
                                          const char* shader_directory) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (device == nullptr || queue == nullptr || shader_directory == nullptr || shader_directory[0] == '\0')
        return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl* state = new (storage_.data()) Impl{};
    state->owner_thread_ = GetCurrentThreadId();
    state->device_ = device;
    state->queue_ = queue;

    D3D12_DESCRIPTOR_RANGE compute_ranges[2]{};
    compute_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[0].NumDescriptors = 2;
    compute_ranges[0].BaseShaderRegister = 0;
    compute_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    compute_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[1].NumDescriptors = 3;
    compute_ranges[1].BaseShaderRegister = 0;
    compute_ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER compute_parameters[2]{};
    compute_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compute_parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    compute_parameters[0].DescriptorTable.pDescriptorRanges = compute_ranges;
    compute_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compute_parameters[1].Constants.Num32BitValues = 4;
    compute_parameters[1].Constants.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC compute_root_desc{};
    compute_root_desc.NumParameters = 2;
    compute_root_desc.pParameters = compute_parameters;

    D3D12_DESCRIPTOR_RANGE render_range{};
    render_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    render_range.NumDescriptors = 5;
    render_range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER render_parameters[2]{};
    render_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    render_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    render_parameters[0].DescriptorTable.pDescriptorRanges = &render_range;
    render_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    render_parameters[1].Constants.Num32BitValues = 4;
    render_parameters[1].Constants.ShaderRegister = 0;
    D3D12_ROOT_SIGNATURE_DESC render_root_desc{};
    render_root_desc.NumParameters = 2;
    render_root_desc.pParameters = render_parameters;
    D3D12_STATIC_SAMPLER_DESC atlas_sampler{};
    atlas_sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    atlas_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    atlas_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    atlas_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    atlas_sampler.MaxLOD = D3D12_FLOAT32_MAX;
    atlas_sampler.ShaderRegister = 0;
    atlas_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    render_root_desc.NumStaticSamplers = 1;
    render_root_desc.pStaticSamplers = &atlas_sampler;
    render_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> compute_root_blob;
    ComPtr<ID3DBlob> render_root_blob;
    ComPtr<ID3DBlob> root_error;
    if (FAILED(D3D12SerializeRootSignature(&compute_root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           compute_root_blob.GetAddressOf(), root_error.GetAddressOf())) ||
        FAILED(device->CreateRootSignature(0, compute_root_blob->GetBufferPointer(), compute_root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(state->compute_root_.GetAddressOf()))) ||
        FAILED(D3D12SerializeRootSignature(&render_root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           render_root_blob.GetAddressOf(), root_error.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateRootSignature(0, render_root_blob->GetBufferPointer(), render_root_blob->GetBufferSize(),
                                           IID_PPV_ARGS(state->render_root_.GetAddressOf())))) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }

    std::array<std::byte, maximum_shader_bytes> static_shader{};
    std::array<std::byte, maximum_shader_bytes> active_shader{};
    std::array<std::byte, maximum_shader_bytes> vertex_shader{};
    std::array<std::byte, maximum_shader_bytes> pixel_shader{};
    size_t static_bytes = 0;
    size_t active_bytes = 0;
    size_t vertex_bytes = 0;
    size_t pixel_bytes = 0;
    if (!load_shader(shader_directory, "expand_static.dxil", &static_shader, &static_bytes) ||
        !load_shader(shader_directory, "update_active.dxil", &active_shader, &active_bytes) ||
        !load_shader(shader_directory, "overlay_vertex.dxil", &vertex_shader, &vertex_bytes) ||
        !load_shader(shader_directory, "overlay_pixel.dxil", &pixel_shader, &pixel_bytes)) {
        state->~Impl();
        return SACCADE_ERROR_NOT_FOUND;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_pipeline{};
    compute_pipeline.pRootSignature = state->compute_root_.Get();
    compute_pipeline.CS = {static_shader.data(), static_bytes};
    if (FAILED(device->CreateComputePipelineState(&compute_pipeline,
                                                  IID_PPV_ARGS(state->static_pipeline_.GetAddressOf())))) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    compute_pipeline.CS = {active_shader.data(), active_bytes};
    if (FAILED(device->CreateComputePipelineState(&compute_pipeline,
                                                  IID_PPV_ARGS(state->active_pipeline_.GetAddressOf())))) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC render_pipeline{};
    render_pipeline.pRootSignature = state->render_root_.Get();
    render_pipeline.VS = {vertex_shader.data(), vertex_bytes};
    render_pipeline.PS = {pixel_shader.data(), pixel_bytes};
    render_pipeline.BlendState.RenderTarget[0].BlendEnable = TRUE;
    render_pipeline.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    render_pipeline.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    render_pipeline.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    render_pipeline.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    render_pipeline.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    render_pipeline.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    render_pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    render_pipeline.SampleMask = UINT32_MAX;
    render_pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    render_pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    render_pipeline.RasterizerState.DepthClipEnable = TRUE;
    render_pipeline.DepthStencilState.DepthEnable = FALSE;
    render_pipeline.DepthStencilState.StencilEnable = FALSE;
    render_pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    render_pipeline.NumRenderTargets = 1;
    render_pipeline.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    render_pipeline.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&render_pipeline,
                                                   IID_PPV_ARGS(state->render_pipeline_.GetAddressOf())))) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = slot_count * descriptors_per_slot;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_INDIRECT_ARGUMENT_DESC indirect_argument{};
    indirect_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc{};
    signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &indirect_argument;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(state->descriptors_.GetAddressOf()))) ||
        FAILED(device->CreateCommandSignature(&signature_desc, nullptr,
                                              IID_PPV_ARGS(state->draw_signature_.GetAddressOf()))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(state->fence_.GetAddressOf())))) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    state->completion_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state->completion_event_ == nullptr) {
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    state->descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!state->upload_glyph_atlas(nullptr)) {
        (void)CloseHandle(state->completion_event_);
        state->completion_event_ = nullptr;
        state->~Impl();
        return SACCADE_ERROR_BACKEND;
    }
    for (Impl::Slot& slot : state->slots_) {
        if (!state->create_slot(slot)) {
            (void)CloseHandle(state->completion_event_);
            state->completion_event_ = nullptr;
            state->~Impl();
            return SACCADE_ERROR_BACKEND;
        }
    }
    state->stats_.slot_count = slot_count;
    state->stats_.target_capacity = SACCADE_OVERLAY_MAX_TARGETS;
    state->stats_.instance_capacity = instance_capacity;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult OverlayRenderer::submit(const SaccadeOverlayFrameDesc& frame, OverlaySubmission* output) noexcept {
    return submit_internal(frame, nullptr, output);
}

SaccadeResult OverlayRenderer::set_glyph_atlas(overlay::GlyphAtlasView atlas) noexcept {
    if (!initialized_ || !overlay::glyph_atlas_valid(atlas)) return SACCADE_ERROR_INVALID_ARGUMENT;
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    return state.upload_glyph_atlas(atlas.pixels) ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

SaccadeResult OverlayRenderer::render(const SaccadeOverlayFrameDesc& frame, const OverlayRenderTarget& target,
                                      OverlaySubmission* output) noexcept {
    if (target.texture == nullptr || target.view.ptr == 0 || target.width == 0 || target.height == 0)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    return submit_internal(frame, &target, output);
}

SaccadeResult OverlayRenderer::submit_internal(const SaccadeOverlayFrameDesc& frame,
                                               const OverlayRenderTarget* render_target,
                                               OverlaySubmission* output) noexcept {
    if (!initialized_ || output == nullptr || !frame_valid(frame)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    overlay::PacketView packet{};
    const bool cached = state.validated_bytes_ == frame.packet.data && state.validated_size_ == frame.packet.size &&
                        state.validated_packet_.header.scene_epoch == frame.scene_epoch &&
                        state.validated_packet_.header.transform_epoch == frame.transform_epoch;
    if (cached) {
        packet = state.validated_packet_;
    } else {
        const SaccadeResult validated = overlay::validate_packet(frame.packet, &packet);
        if (validated != SACCADE_OK || frame.scene_epoch != packet.header.scene_epoch ||
            frame.transform_epoch != packet.header.transform_epoch) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        state.validated_packet_ = packet;
        state.validated_bytes_ = frame.packet.data;
        state.validated_size_ = frame.packet.size;
    }
    if ((frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0 &&
        frame.active_target_index >= packet.header.target_count) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    Impl::Slot* slot = nullptr;
    uint32_t slot_index = 0;
    for (uint32_t offset = 0; offset < slot_count; ++offset) {
        const uint32_t index = (state.next_slot_ + offset) % slot_count;
        const SaccadeResult slot_status = state.fence_status(state.slots_[index].fence_value_);
        if (slot_status == SACCADE_ERROR_BACKEND) {
            ++state.stats_.failures;
            return slot_status;
        }
        if (slot_status == SACCADE_OK) {
            slot = &state.slots_[index];
            slot_index = index;
            state.next_slot_ = (index + 1U) % slot_count;
            break;
        }
    }
    if (slot == nullptr) {
        ++state.stats_.busy_submissions;
        return SACCADE_ERROR_BUSY;
    }
    const bool static_update =
        slot->scene_epoch_ != frame.scene_epoch || slot->transform_epoch_ != frame.transform_epoch;
    if (static_update) {
        const size_t targets_size = static_cast<size_t>(packet.header.target_count) * sizeof(SaccadeOverlayTarget);
        const size_t styles_size = static_cast<size_t>(packet.header.style_count) * sizeof(SaccadeOverlayStyle);
        std::memcpy(slot->targets_mapped_, packet.targets, targets_size);
        std::memcpy(slot->styles_mapped_, packet.styles, styles_size);
        slot->scene_epoch_ = frame.scene_epoch;
        slot->transform_epoch_ = frame.transform_epoch;
        state.stats_.packet_upload_bytes += targets_size + styles_size;
    }
    const ExpandParameters parameters{packet.header.target_count, frame.active_target_index,
                                      packet.header.target_count * instances_per_target,
                                      (frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0 ? 1U : 0U};
    if (FAILED(slot->allocator_->Reset()) || FAILED(slot->commands_->Reset(slot->allocator_.Get(), nullptr))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    if (slot->read_state_) {
        D3D12_RESOURCE_BARRIER barriers[3]{};
        ID3D12Resource* resources[]{slot->rects_.Get(), slot->metadata_.Get(), slot->arguments_.Get()};
        for (uint32_t index = 0; index < 3; ++index) {
            barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[index].Transition.pResource = resources[index];
            barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[index].Transition.StateBefore =
                index < 2 ? shader_resource_state : D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        slot->commands_->ResourceBarrier(3, barriers);
    }
    ID3D12DescriptorHeap* heaps[]{state.descriptors_.Get()};
    slot->commands_->SetDescriptorHeaps(1, heaps);
    slot->commands_->SetComputeRootSignature(state.compute_root_.Get());
    slot->commands_->SetComputeRootDescriptorTable(
        0, gpu_handle(state.descriptors_.Get(), state.descriptor_size_, slot_index * descriptors_per_slot));
    slot->commands_->SetComputeRoot32BitConstants(1, 4, &parameters, 0);
    if (static_update && packet.header.target_count != 0) {
        slot->commands_->SetPipelineState(state.static_pipeline_.Get());
        slot->commands_->Dispatch((packet.header.target_count + 63U) / 64U, 1, 1);
        ++state.stats_.static_dispatches;
    }
    slot->commands_->SetPipelineState(state.active_pipeline_.Get());
    slot->commands_->Dispatch(1, 1, 1);
    ++state.stats_.active_dispatches;
    D3D12_RESOURCE_BARRIER read_barriers[3]{};
    ID3D12Resource* resources[]{slot->rects_.Get(), slot->metadata_.Get(), slot->arguments_.Get()};
    for (uint32_t index = 0; index < 3; ++index) {
        read_barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        read_barriers[index].Transition.pResource = resources[index];
        read_barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        read_barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        read_barriers[index].Transition.StateAfter =
            index < 2 ? shader_resource_state : D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    slot->commands_->ResourceBarrier(3, read_barriers);
    slot->read_state_ = true;

    if (render_target != nullptr) {
        D3D12_RESOURCE_BARRIER target_barrier{};
        target_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        target_barrier.Transition.pResource = render_target->texture;
        target_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        target_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        target_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        slot->commands_->ResourceBarrier(1, &target_barrier);
        constexpr std::array<float, 4> clear{};
        slot->commands_->ClearRenderTargetView(render_target->view, clear.data(), 0, nullptr);
        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(render_target->width);
        viewport.Height = static_cast<float>(render_target->height);
        viewport.MaxDepth = 1.0F;
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(render_target->width),
                                 static_cast<LONG>(render_target->height)};
        slot->commands_->RSSetViewports(1, &viewport);
        slot->commands_->RSSetScissorRects(1, &scissor);
        slot->commands_->SetGraphicsRootSignature(state.render_root_.Get());
        slot->commands_->SetPipelineState(state.render_pipeline_.Get());
        slot->commands_->SetGraphicsRootDescriptorTable(
            0, gpu_handle(state.descriptors_.Get(), state.descriptor_size_, slot_index * descriptors_per_slot + 5U));
        float animation_time = 0.0F;
        float scene_age = 1.0F;
        if (render_target->timestamp_ns != 0) {
            if (state.animation_scene_epoch_ != packet.header.scene_epoch) {
                state.animation_scene_epoch_ = packet.header.scene_epoch;
                state.animation_scene_start_ns_ = render_target->timestamp_ns;
            }
            constexpr uint64_t animation_period_ns = UINT64_C(4'000'000'000);
            animation_time = static_cast<float>(render_target->timestamp_ns % animation_period_ns) * 1.0e-9F;
            scene_age = static_cast<float>(render_target->timestamp_ns - state.animation_scene_start_ns_) * 1.0e-9F;
        }
        const DisplayConstants display{1.0F / static_cast<float>(render_target->width),
                                       1.0F / static_cast<float>(render_target->height), animation_time, scene_age};
        slot->commands_->SetGraphicsRoot32BitConstants(1, 4, &display, 0);
        slot->commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        slot->commands_->OMSetRenderTargets(1, &render_target->view, FALSE, nullptr);
        slot->commands_->ExecuteIndirect(state.draw_signature_.Get(), 1, slot->arguments_.Get(), 0, nullptr, 0);
        std::swap(target_barrier.Transition.StateBefore, target_barrier.Transition.StateAfter);
        slot->commands_->ResourceBarrier(1, &target_barrier);
        ++state.stats_.rendered_frames;
        ++state.stats_.draw_calls;
    }
    if (FAILED(slot->commands_->Close())) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    ID3D12CommandList* lists[]{slot->commands_.Get()};
    state.queue_->ExecuteCommandLists(1, lists);
    const uint64_t fence_value = state.next_fence_value_++;
    if (FAILED(state.queue_->Signal(state.fence_.Get(), fence_value))) {
        ++state.stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    const uint64_t sequence = state.next_sequence_++;
    slot->sequence_ = sequence;
    slot->fence_value_ = fence_value;
    slot->instance_count_ = parameters.static_instance_count + parameters.has_active_target;
    *output = {sequence, frame.scene_epoch, slot_index, slot->instance_count_};
    ++state.stats_.submissions;
    return SACCADE_OK;
}

SaccadeResult OverlayRenderer::poll(const OverlaySubmission& submission, bool* complete) const noexcept {
    if (!initialized_ || complete == nullptr || submission.sequence == 0 || submission.slot_index >= slot_count) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const Impl& state = impl();
    if (!state.owns_thread()) return SACCADE_ERROR_STATE;
    const Impl::Slot& slot = state.slots_[submission.slot_index];
    if (slot.sequence_ != submission.sequence || slot.scene_epoch_ != submission.scene_epoch) {
        return SACCADE_ERROR_STALE_HANDLE;
    }
    const SaccadeResult status = state.fence_status(slot.fence_value_);
    *complete = status == SACCADE_OK;
    return status == SACCADE_ERROR_BUSY ? SACCADE_OK : status;
}

SaccadeResult OverlayRenderer::wait(const OverlaySubmission& submission, uint64_t timeout_ns) const noexcept {
    bool complete = false;
    const SaccadeResult polled = poll(submission, &complete);
    if (polled != SACCADE_OK || complete) return polled;
    return impl().wait_fence(impl().slots_[submission.slot_index].fence_value_, timeout_ns);
}

SaccadeResult OverlayRenderer::copy_instances(const OverlaySubmission& submission, OverlayInstanceSpan output,
                                              size_t* count) noexcept {
    if (count == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeResult waited = wait(submission, UINT64_C(1'000'000'000));
    if (waited != SACCADE_OK) return waited;
    Impl& state = impl();
    Impl::Slot& slot = state.slots_[submission.slot_index];
    *count = slot.instance_count_;
    if (output.capacity < slot.instance_count_ || output.rects == nullptr || output.metadata == nullptr)
        return SACCADE_ERROR_CAPACITY;
    if (FAILED(slot.allocator_->Reset()) || FAILED(slot.commands_->Reset(slot.allocator_.Get(), nullptr))) {
        return SACCADE_ERROR_BACKEND;
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    ID3D12Resource* resources[]{slot.rects_.Get(), slot.metadata_.Get()};
    for (uint32_t index = 0; index < 2; ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = resources[index];
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[index].Transition.StateBefore = shader_resource_state;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    slot.commands_->ResourceBarrier(2, barriers);
    slot.commands_->CopyResource(slot.rects_readback_.Get(), slot.rects_.Get());
    slot.commands_->CopyResource(slot.metadata_readback_.Get(), slot.metadata_.Get());
    for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    slot.commands_->ResourceBarrier(2, barriers);
    if (FAILED(slot.commands_->Close())) return SACCADE_ERROR_BACKEND;
    ID3D12CommandList* lists[]{slot.commands_.Get()};
    state.queue_->ExecuteCommandLists(1, lists);
    slot.fence_value_ = state.next_fence_value_++;
    if (FAILED(state.queue_->Signal(state.fence_.Get(), slot.fence_value_)) ||
        state.wait_fence(slot.fence_value_, UINT64_C(1'000'000'000)) != SACCADE_OK) {
        return SACCADE_ERROR_BACKEND;
    }
    void* rects = nullptr;
    void* metadata = nullptr;
    D3D12_RANGE rect_range{0, slot.instance_count_ * sizeof(SaccadeOverlayRect)};
    D3D12_RANGE metadata_range{0, slot.instance_count_ * sizeof(SaccadeOverlayInstanceMeta)};
    if (FAILED(slot.rects_readback_->Map(0, &rect_range, &rects)) ||
        FAILED(slot.metadata_readback_->Map(0, &metadata_range, &metadata))) {
        if (rects != nullptr) slot.rects_readback_->Unmap(0, nullptr);
        return SACCADE_ERROR_BACKEND;
    }
    std::memcpy(output.rects, rects, rect_range.End);
    std::memcpy(output.metadata, metadata, metadata_range.End);
    D3D12_RANGE no_write{0, 0};
    slot.metadata_readback_->Unmap(0, &no_write);
    slot.rects_readback_->Unmap(0, &no_write);
    return SACCADE_OK;
}

SaccadeResult OverlayRenderer::memory_stats(SaccadeMemoryStats* output) const noexcept {
    if (!initialized_ || output == nullptr || output->struct_size != sizeof(*output) ||
        output->api_version != SACCADE_API_VERSION) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    const uint64_t per_slot = target_bytes + style_bytes + static_cast<uint64_t>(rect_bytes) * 2U +
                              static_cast<uint64_t>(metadata_bytes) * 2U + sizeof(D3D12_DRAW_ARGUMENTS);
    SaccadeMemoryStats value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.device_owned = per_slot * slot_count + overlay::glyph_atlas_bytes;
    value.high_water_bytes = value.device_owned;
    *output = value;
    return SACCADE_OK;
}

OverlayStats OverlayRenderer::stats() const noexcept {
    return initialized_ ? impl().stats_ : OverlayStats{};
}

} // namespace saccade::backend::d3d12
