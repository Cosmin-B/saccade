#include "backend/registry.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

std::atomic<bool> count_allocations{false};
std::atomic<size_t> allocation_count{0};

SaccadeInferenceOps inference_ops() {
    SaccadeInferenceOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = +[](void*, uint32_t, SaccadeDeviceInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.query_model = +[](void*, SaccadeSpanU8, SaccadeModelInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.create_model = +[](void*, const SaccadeModelDesc*, SaccadeModelHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.destroy_model = +[](void*, SaccadeModelHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.create_context = +[](void*, const SaccadeExecutionContextDesc*,
                             SaccadeExecutionContextHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.destroy_context = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.submit = +[](void*, SaccadeExecutionContextHandle, const SaccadeInferenceDispatchDesc*,
                     SaccadeTicketHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.poll = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.wait = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, uint64_t,
                   SaccadeInferenceStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.collect = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle, SaccadeMutableSpanU8,
                      size_t*) -> SaccadeResult { return SACCADE_OK; };
    ops.cancel = +[](void*, SaccadeExecutionContextHandle, SaccadeTicketHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.reset = +[](void*, SaccadeExecutionContextHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, SaccadeExecutionContextHandle, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats =
        +[](void*, SaccadeExecutionContextHandle, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeInferenceProviderDesc provider_desc(uint64_t stable_id) {
    static const uint8_t name[] = "device-test";
    SaccadeInferenceProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info.struct_size = static_cast<uint32_t>(sizeof(desc.info));
    desc.info.api_version = SACCADE_API_VERSION;
    desc.info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    desc.info.capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_GPU |
                                SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT | SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT |
                                SACCADE_PROVIDER_CAPABILITY_ASYNC;
    desc.info.stable_id = stable_id;
    desc.info.name = {name, sizeof(name) - 1};
    desc.ops = inference_ops();
    return desc;
}

SaccadeDeviceInfo device_info(uint64_t stable_id, uint32_t capabilities, uint32_t formats, uint32_t precisions,
                              uint32_t imports, const char* name = "device") {
    SaccadeDeviceInfo info{};
    info.struct_size = static_cast<uint32_t>(sizeof(info));
    info.api_version = SACCADE_API_VERSION;
    info.stable_id = stable_id;
    info.capability_bits = capabilities;
    info.format_bits = formats;
    info.precision_bits = precisions;
    info.import_bits = imports;
    info.queue_capacity = 2;
    info.max_in_flight = 1;
    info.host_alignment = 64;
    info.device_alignment = capabilities & SACCADE_PROVIDER_CAPABILITY_GPU ? 256 : 0;
    info.name = {reinterpret_cast<const uint8_t*>(name), std::strlen(name)};
    return info;
}

} // namespace

void* operator new(std::size_t size) {
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    using saccade::backend::DeviceRequirements;
    using saccade::backend::DeviceSelection;
    using saccade::backend::ProviderRegistry;
    using saccade::backend::SelectionReason;

    ProviderRegistry registry;
    SaccadeInferenceProviderDesc provider = provider_desc(10);
    SaccadeProviderHandle provider_handle = 0;
    if (registry.register_inference(&provider, &provider_handle) != SACCADE_OK) {
        return 1;
    }

    char mutable_name[] = "mutable";
    SaccadeDeviceInfo cpu =
        device_info(100, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8 | SACCADE_FORMAT_RGBA8,
                    SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
    cpu.name = {reinterpret_cast<const uint8_t*>(mutable_name), sizeof(mutable_name) - 1};
    SaccadeDeviceHandle cpu_handle = 0;
    if (registry.register_device(provider_handle, &cpu, &cpu_handle) != SACCADE_OK || cpu_handle == 0) {
        return 2;
    }
    mutable_name[0] = 'x';
    const auto* stored_cpu = registry.device(cpu_handle);
    if (stored_cpu == nullptr || stored_cpu->provider != provider_handle || stored_cpu->info.name.size != 7 ||
        stored_cpu->info.name.data[0] != 'm') {
        return 3;
    }
    if (registry.register_device(provider_handle, &cpu, nullptr) != SACCADE_ERROR_ALREADY_EXISTS) {
        return 4;
    }

    SaccadeDeviceInfo gpu = device_info(101, SACCADE_PROVIDER_CAPABILITY_GPU | SACCADE_PROVIDER_CAPABILITY_ASYNC,
                                        SACCADE_FORMAT_BGRA8, SACCADE_PRECISION_FP16 | SACCADE_PRECISION_INT8,
                                        SACCADE_IMPORT_HOST | SACCADE_IMPORT_IOSURFACE, "gpu");
    SaccadeDeviceHandle gpu_handle = 0;
    if (registry.register_device(provider_handle, &gpu, &gpu_handle) != SACCADE_OK) {
        return 5;
    }

    DeviceRequirements requirements{};
    requirements.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_GPU;
    requirements.preferred_capability_bits = SACCADE_PROVIDER_CAPABILITY_ASYNC;
    requirements.required_format_bits = SACCADE_FORMAT_BGRA8;
    requirements.required_precision_bits = SACCADE_PRECISION_FP16;
    requirements.required_import_bits = SACCADE_IMPORT_HOST;
    requirements.preferred_import_bits = SACCADE_IMPORT_IOSURFACE;
    requirements.minimum_queue_capacity = 2;
    requirements.minimum_max_in_flight = 1;
    DeviceSelection selection{};
    if (registry.select_device(requirements, &selection) != SACCADE_OK || selection.handle != gpu_handle ||
        selection.provider != provider_handle || selection.stable_id != 101 ||
        selection.reason != SelectionReason::preferred_capability) {
        return 6;
    }
    if (registry.select_device_by_id(provider_handle, 100, &selection) != SACCADE_OK ||
        selection.handle != cpu_handle || selection.reason != SelectionReason::explicit_id) {
        return 7;
    }
    requirements.required_import_bits = SACCADE_IMPORT_WIN32_CAPTURE;
    if (registry.select_device(requirements, &selection) != SACCADE_ERROR_NOT_FOUND) {
        return 8;
    }

    ProviderRegistry malformed_registry;
    provider = provider_desc(20);
    if (malformed_registry.register_inference(&provider, &provider_handle) != SACCADE_OK) {
        return 9;
    }
    SaccadeDeviceInfo malformed = device_info(200, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8,
                                              SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
    malformed.struct_size = 0;
    if (malformed_registry.register_device(provider_handle, &malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 10;
    }
    malformed = device_info(201, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8, SACCADE_PRECISION_FP32,
                            SACCADE_IMPORT_HOST);
    malformed.host_alignment = 3;
    if (malformed_registry.register_device(provider_handle, &malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 11;
    }
    std::array<uint8_t, 64> long_name{};
    long_name.fill('n');
    malformed.host_alignment = 64;
    malformed.name = {long_name.data(), long_name.size()};
    if (malformed_registry.register_device(provider_handle, &malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 12;
    }
    if (malformed_registry.register_device(UINT64_C(0xBAD), &malformed, nullptr) != SACCADE_ERROR_STALE_HANDLE) {
        return 13;
    }

    malformed = device_info(202, SACCADE_PROVIDER_CAPABILITY_ACCELERATOR, SACCADE_FORMAT_BGRA8, SACCADE_PRECISION_FP32,
                            SACCADE_IMPORT_HOST);
    if (malformed_registry.register_device(provider_handle, &malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 22;
    }

    SaccadeDeviceInfo misaligned = device_info(203, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8,
                                               SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
    alignas(SaccadeDeviceInfo) std::array<std::byte, sizeof(SaccadeDeviceInfo) + 1> device_storage{};
    std::memcpy(device_storage.data() + 1, &misaligned, sizeof(misaligned));
    const auto* misaligned_info = reinterpret_cast<const SaccadeDeviceInfo*>(device_storage.data() + 1);
    if (malformed_registry.register_device(provider_handle, misaligned_info, nullptr) != SACCADE_OK) {
        return 23;
    }

    ProviderRegistry capacity_registry;
    provider = provider_desc(30);
    if (capacity_registry.register_inference(&provider, &provider_handle) != SACCADE_OK) {
        return 14;
    }
    for (size_t index = 0; index < ProviderRegistry::device_capacity; ++index) {
        SaccadeDeviceInfo item = device_info(300 + static_cast<uint64_t>(index), SACCADE_PROVIDER_CAPABILITY_CPU,
                                             SACCADE_FORMAT_BGRA8, SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
        if (capacity_registry.register_device(provider_handle, &item, nullptr) != SACCADE_OK) {
            return 15;
        }
    }
    SaccadeDeviceInfo overflow = device_info(999, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8,
                                             SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
    if (capacity_registry.register_device(provider_handle, &overflow, nullptr) != SACCADE_ERROR_CAPACITY) {
        return 16;
    }

    registry.freeze();
    if (registry.register_device(provider_handle, &overflow, nullptr) != SACCADE_ERROR_STATE) {
        return 17;
    }

    requirements = {};
    requirements.required_capability_bits = SACCADE_PROVIDER_CAPABILITY_CPU;
    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (int index = 0; index < 1000; ++index) {
        if (registry.select_device(requirements, &selection) != SACCADE_OK ||
            registry.device(selection.handle) == nullptr) {
            return 18;
        }
    }
    count_allocations.store(false, std::memory_order_relaxed);
    if (allocation_count.load(std::memory_order_relaxed) != 0) {
        return 19;
    }

    ProviderRegistry foreign_registry;
    provider = provider_desc(40);
    SaccadeProviderHandle foreign_provider = 0;
    if (foreign_registry.register_inference(&provider, &foreign_provider) != SACCADE_OK) {
        return 20;
    }
    SaccadeDeviceInfo foreign = device_info(400, SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_FORMAT_BGRA8,
                                            SACCADE_PRECISION_FP32, SACCADE_IMPORT_HOST);
    SaccadeDeviceHandle foreign_handle = 0;
    if (foreign_registry.register_device(foreign_provider, &foreign, &foreign_handle) != SACCADE_OK ||
        foreign_handle == cpu_handle || foreign_registry.device(cpu_handle) != nullptr) {
        return 21;
    }

    return 0;
}
