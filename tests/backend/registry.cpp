#include "backend/registry.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

namespace {

std::atomic<bool> count_allocations{false};
std::atomic<size_t> allocation_count{0};

SaccadeSpanU8 text_span(const char* text) {
    return {reinterpret_cast<const uint8_t*>(text), std::strlen(text)};
}

SaccadeProviderInfo provider_info(uint32_t family, uint64_t stable_id, uint32_t capabilities,
                                  const char* name = "provider") {
    SaccadeProviderInfo info{};
    info.struct_size = static_cast<uint32_t>(sizeof(info));
    info.api_version = SACCADE_API_VERSION;
    info.family = family;
    info.capability_bits = capabilities;
    info.stable_id = stable_id;
    info.name = text_span(name);
    return info;
}

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

SaccadeCaptureOps capture_ops() {
    SaccadeCaptureOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_sources = +[](void*, uint32_t, SaccadeCaptureSourceInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.create = +[](void*, const SaccadeCaptureStreamDesc*, SaccadeCaptureStreamHandle*) -> SaccadeResult {
        return SACCADE_OK;
    };
    ops.destroy = +[](void*, SaccadeCaptureStreamHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.start = +[](void*, SaccadeCaptureStreamHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.stop = +[](void*, SaccadeCaptureStreamHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.acquire =
        +[](void*, SaccadeCaptureStreamHandle, uint64_t, SaccadeCapturedFrame*) -> SaccadeResult { return SACCADE_OK; };
    ops.copy_damage = +[](void*, SaccadeCaptureStreamHandle, SaccadeFrameHandle, SaccadeRectI32*, uint32_t,
                          uint32_t*) -> SaccadeResult { return SACCADE_OK; };
    ops.release = +[](void*, SaccadeCaptureStreamHandle, SaccadeFrameHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, SaccadeCaptureStreamHandle, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats =
        +[](void*, SaccadeCaptureStreamHandle, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeOverlayOps overlay_ops() {
    SaccadeOverlayOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.create = +[](void*, const SaccadeOverlayDesc*, SaccadeOverlayHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.destroy = +[](void*, SaccadeOverlayHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.submit =
        +[](void*, SaccadeOverlayHandle, const SaccadeOverlayFrameDesc*) -> SaccadeResult { return SACCADE_OK; };
    ops.set_visible = +[](void*, SaccadeOverlayHandle, uint32_t) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, SaccadeOverlayHandle, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats = +[](void*, SaccadeOverlayHandle, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    ops.reset = +[](void*, SaccadeOverlayHandle) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeAccessibilityOps accessibility_ops() {
    SaccadeAccessibilityOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_windows = +[](void*, uint32_t, SaccadeWindowInfo*) -> SaccadeResult { return SACCADE_OK; };
    ops.request =
        +[](void*, const SaccadeAccessibilityQueryDesc*, SaccadeTicketHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.poll = +[](void*, SaccadeTicketHandle, SaccadeAccessibilityStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.wait =
        +[](void*, SaccadeTicketHandle, uint64_t, SaccadeAccessibilityStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.collect =
        +[](void*, SaccadeSnapshotHandle, SaccadeMutableSpanU8, size_t*) -> SaccadeResult { return SACCADE_OK; };
    ops.cancel = +[](void*, SaccadeTicketHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.release = +[](void*, SaccadeSnapshotHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats = +[](void*, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeInputOps input_ops() {
    SaccadeInputOps ops{};
    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.execute = +[](void*, const SaccadeInputPlanDesc*, SaccadeTicketHandle*) -> SaccadeResult { return SACCADE_OK; };
    ops.poll = +[](void*, SaccadeTicketHandle, SaccadeInputStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.wait = +[](void*, SaccadeTicketHandle, uint64_t, SaccadeInputStatus*) -> SaccadeResult { return SACCADE_OK; };
    ops.cancel = +[](void*, SaccadeTicketHandle) -> SaccadeResult { return SACCADE_OK; };
    ops.release_all = +[](void*) -> SaccadeResult { return SACCADE_OK; };
    ops.synchronize = +[](void*, uint64_t) -> SaccadeResult { return SACCADE_OK; };
    ops.reset = +[](void*) -> SaccadeResult { return SACCADE_OK; };
    ops.memory_stats = +[](void*, SaccadeMemoryStats*) -> SaccadeResult { return SACCADE_OK; };
    return ops;
}

SaccadeInferenceProviderDesc inference_provider(uint64_t id, uint32_t capabilities, const char* name = "inference") {
    SaccadeInferenceProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_INFERENCE, id, capabilities, name);
    desc.ops = inference_ops();
    return desc;
}

SaccadeCaptureProviderDesc capture_provider(uint64_t id) {
    SaccadeCaptureProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_CAPTURE, id, 0, "capture");
    desc.ops = capture_ops();
    return desc;
}

SaccadeOverlayProviderDesc overlay_provider(uint64_t id) {
    SaccadeOverlayProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_OVERLAY, id, 0, "overlay");
    desc.ops = overlay_ops();
    return desc;
}

SaccadeAccessibilityProviderDesc accessibility_provider(uint64_t id) {
    SaccadeAccessibilityProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_ACCESSIBILITY, id, 0, "accessibility");
    desc.ops = accessibility_ops();
    return desc;
}

SaccadeInputProviderDesc input_provider(uint64_t id) {
    SaccadeInputProviderDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    desc.info = provider_info(SACCADE_PROVIDER_FAMILY_INPUT, id, 0, "input");
    desc.ops = input_ops();
    return desc;
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
    using saccade::backend::ProviderRegistry;
    using saccade::backend::ProviderSelection;
    using saccade::backend::SelectionReason;

    ProviderRegistry registry;
    SaccadeProviderHandle cpu_handle = 0;
    SaccadeInferenceProviderDesc cpu = inference_provider(10, SACCADE_PROVIDER_CAPABILITY_CPU, "mutable");
    char mutable_name[] = "mutable";
    cpu.info.name = {reinterpret_cast<const uint8_t*>(mutable_name), sizeof(mutable_name) - 1};
    if (registry.register_inference(&cpu, &cpu_handle) != SACCADE_OK || cpu_handle == 0) {
        return 1;
    }
    mutable_name[0] = 'x';
    const auto* stored_cpu = registry.inference(cpu_handle);
    if (stored_cpu == nullptr || stored_cpu->info.name.size != 7 || stored_cpu->info.name.data[0] != 'm') {
        return 2;
    }
    if (registry.register_inference(&cpu, nullptr) != SACCADE_ERROR_ALREADY_EXISTS) {
        return 3;
    }

    SaccadeProviderHandle gpu_handle = 0;
    SaccadeInferenceProviderDesc gpu =
        inference_provider(11, SACCADE_PROVIDER_CAPABILITY_CPU | SACCADE_PROVIDER_CAPABILITY_GPU, "gpu");
    if (registry.register_inference(&gpu, &gpu_handle) != SACCADE_OK) {
        return 4;
    }

    ProviderSelection selection{};
    if (registry.select_inference(SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_PROVIDER_CAPABILITY_GPU, &selection) !=
            SACCADE_OK ||
        selection.handle != gpu_handle || selection.stable_id != 11 ||
        selection.reason != SelectionReason::preferred_capability) {
        return 5;
    }
    if (registry.select_inference_by_id(10, &selection) != SACCADE_OK || selection.handle != cpu_handle ||
        selection.reason != SelectionReason::explicit_id) {
        return 6;
    }
    if (registry.select_inference(SACCADE_PROVIDER_CAPABILITY_ACCELERATOR, 0, &selection) != SACCADE_ERROR_NOT_FOUND) {
        return 7;
    }

    SaccadeCaptureProviderDesc capture = capture_provider(20);
    SaccadeOverlayProviderDesc overlay = overlay_provider(30);
    SaccadeAccessibilityProviderDesc accessibility = accessibility_provider(40);
    SaccadeInputProviderDesc input = input_provider(50);
    if (registry.register_capture(&capture, nullptr) != SACCADE_OK ||
        registry.register_overlay(&overlay, nullptr) != SACCADE_OK ||
        registry.register_accessibility(&accessibility, nullptr) != SACCADE_OK ||
        registry.register_input(&input, nullptr) != SACCADE_OK) {
        return 8;
    }

    ProviderRegistry malformed_registry;
    SaccadeInferenceProviderDesc malformed = inference_provider(100, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.struct_size = 0;
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 9;
    }
    malformed = inference_provider(101, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.api_version = UINT32_C(0x00020000);
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_VERSION) {
        return 10;
    }
    malformed = inference_provider(102, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.struct_size = static_cast<uint32_t>(offsetof(SaccadeInferenceProviderDesc, reserved) - 1);
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 11;
    }
    malformed = inference_provider(103, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.ops.submit = nullptr;
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 12;
    }
    malformed = inference_provider(104, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.reserved[0] = 1;
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 13;
    }
    malformed = inference_provider(105, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.info.reserved[0] = 1;
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 14;
    }
    malformed = inference_provider(106, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.ops.reserved[0] = 1;
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 15;
    }
    malformed = inference_provider(107, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.info.struct_size = static_cast<uint32_t>(sizeof(malformed.info) + 8);
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 16;
    }
    malformed = inference_provider(108, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.ops.struct_size = static_cast<uint32_t>(sizeof(malformed.ops) + 8);
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 17;
    }

    std::array<uint8_t, 65> long_name{};
    long_name.fill('n');
    malformed = inference_provider(109, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.info.name = {long_name.data(), 63};
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_OK) {
        return 18;
    }
    malformed = inference_provider(110, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.info.name = {long_name.data(), 64};
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 19;
    }
    malformed = inference_provider(111, SACCADE_PROVIDER_CAPABILITY_CPU);
    malformed.info.name = {long_name.data(), 65};
    if (malformed_registry.register_inference(&malformed, nullptr) != SACCADE_ERROR_INVALID_ARGUMENT) {
        return 20;
    }

    ProviderRegistry compatible_registry;
    SaccadeInferenceProviderDesc smaller = inference_provider(200, SACCADE_PROVIDER_CAPABILITY_CPU);
    smaller.struct_size = static_cast<uint32_t>(offsetof(SaccadeInferenceProviderDesc, reserved));
    smaller.info.struct_size = static_cast<uint32_t>(offsetof(SaccadeProviderInfo, reserved));
    smaller.ops.struct_size = static_cast<uint32_t>(offsetof(SaccadeInferenceOps, reserved));
    smaller.info.reserved[0] = UINT64_C(0xFEEDBEEF);
    smaller.ops.reserved[0] = UINT64_C(0xDEADBEEF);
    SaccadeProviderHandle smaller_handle = 0;
    if (compatible_registry.register_inference(&smaller, &smaller_handle) != SACCADE_OK) {
        return 21;
    }
    const auto* stored_smaller = compatible_registry.inference(smaller_handle);
    if (stored_smaller == nullptr || stored_smaller->info.reserved[0] != 0 || stored_smaller->ops.reserved[0] != 0) {
        return 22;
    }

    struct ExtendedInferenceProviderDesc {
        SaccadeInferenceProviderDesc current;
        uint64_t future[4];
    };

    ExtendedInferenceProviderDesc larger{};
    larger.current = inference_provider(201, SACCADE_PROVIDER_CAPABILITY_CPU);
    larger.current.struct_size = static_cast<uint32_t>(sizeof(larger));
    if (compatible_registry.register_inference(&larger.current, nullptr) != SACCADE_OK) {
        return 23;
    }

    ProviderRegistry capacity_registry;
    for (size_t index = 0; index < ProviderRegistry::capacity_per_family; ++index) {
        SaccadeInferenceProviderDesc item =
            inference_provider(300 + static_cast<uint64_t>(index), SACCADE_PROVIDER_CAPABILITY_CPU);
        if (capacity_registry.register_inference(&item, nullptr) != SACCADE_OK) {
            return 24;
        }
    }
    SaccadeInferenceProviderDesc overflow = inference_provider(400, SACCADE_PROVIDER_CAPABILITY_CPU);
    if (capacity_registry.register_inference(&overflow, nullptr) != SACCADE_ERROR_CAPACITY) {
        return 25;
    }

    registry.freeze();
    SaccadeInferenceProviderDesc late = inference_provider(500, SACCADE_PROVIDER_CAPABILITY_CPU);
    if (!registry.frozen() || registry.register_inference(&late, nullptr) != SACCADE_ERROR_STATE) {
        return 26;
    }

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (int index = 0; index < 1000; ++index) {
        ProviderSelection measured{};
        if (registry.select_inference(SACCADE_PROVIDER_CAPABILITY_CPU, SACCADE_PROVIDER_CAPABILITY_GPU, &measured) !=
                SACCADE_OK ||
            registry.inference(measured.handle) == nullptr) {
            return 27;
        }
    }
    count_allocations.store(false, std::memory_order_relaxed);
    if (allocation_count.load(std::memory_order_relaxed) != 0) {
        return 28;
    }

    ProviderRegistry foreign_registry;
    SaccadeInferenceProviderDesc foreign_provider = inference_provider(600, SACCADE_PROVIDER_CAPABILITY_CPU);
    SaccadeProviderHandle foreign_handle = 0;
    if (foreign_registry.register_inference(&foreign_provider, &foreign_handle) != SACCADE_OK ||
        foreign_handle == cpu_handle || foreign_registry.inference(cpu_handle) != nullptr) {
        return 29;
    }

    constexpr size_t domain_thread_count = 16;
    std::array<SaccadeProviderHandle, domain_thread_count> domain_handles{};
    std::array<std::thread, domain_thread_count> domain_threads{};
    std::atomic<bool> domain_failure{false};
    for (size_t index = 0; index < domain_thread_count; ++index) {
        domain_threads[index] = std::thread([&, index]() {
            ProviderRegistry local_registry;
            SaccadeInferenceProviderDesc local_provider = inference_provider(700, SACCADE_PROVIDER_CAPABILITY_CPU);
            if (local_registry.register_inference(&local_provider, &domain_handles[index]) != SACCADE_OK) {
                domain_failure.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& thread : domain_threads) {
        thread.join();
    }
    if (domain_failure.load(std::memory_order_relaxed)) {
        return 30;
    }
    for (size_t left = 0; left < domain_thread_count; ++left) {
        if (domain_handles[left] == 0) {
            return 31;
        }
        for (size_t right = left + 1; right < domain_thread_count; ++right) {
            if (domain_handles[left] == domain_handles[right]) {
                return 32;
            }
        }
    }

    allocation_count.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
    for (size_t index = 0; index < 1000; ++index) {
        ProviderRegistry measured_registry;
    }
    count_allocations.store(false, std::memory_order_relaxed);
    if (allocation_count.load(std::memory_order_relaxed) != 0) {
        return 33;
    }

    return 0;
}
