#include <saccade/saccade.h>

static_assert(offsetof(SaccadeHostFrameDesc, struct_size) == 0, "host descriptor size prefix");
static_assert(offsetof(SaccadeHostFrameDesc, api_version) == 4, "host descriptor version prefix");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, struct_size) == 0, "IOSurface descriptor size prefix");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, api_version) == 4, "IOSurface descriptor version prefix");
static_assert(offsetof(SaccadeWin32CaptureFrameDesc, struct_size) == 0, "Win32 capture descriptor size prefix");
static_assert(offsetof(SaccadeWin32CaptureFrameDesc, api_version) == 4, "Win32 capture descriptor version prefix");
static_assert(offsetof(SaccadeHostFrameDesc, transform_epoch) == 48, "host descriptor transform epoch offset");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, transform_epoch) == 40,
              "IOSurface descriptor transform epoch offset");
static_assert(offsetof(SaccadeWin32CaptureFrameDesc, transform_epoch) == 40,
              "Win32 capture descriptor transform epoch offset");
static_assert(offsetof(SaccadeWin32CaptureFrameDesc, ready_fence) == 48, "Win32 capture descriptor ready fence offset");
static_assert(offsetof(SaccadeWin32CaptureFrameDesc, ready_value) == 56, "Win32 capture descriptor ready value offset");

namespace {

constexpr SaccadeResult host_result = 41;
constexpr SaccadeResult iosurface_result = 42;
constexpr SaccadeResult win32_capture_result = 43;
constexpr SaccadeRuntimeHandle expected_runtime = UINT64_C(0x8070605040302010);

SaccadeHostFrameDesc host_desc{};
int host_calls = 0;
int iosurface_calls = 0;
int win32_capture_calls = 0;
int descriptor_evaluations = 0;

SaccadeHostFrameDesc* next_host_desc() noexcept {
    ++descriptor_evaluations;
    return &host_desc;
}

} // namespace

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(SaccadeRuntimeHandle runtime,
                                                                 const SaccadeHostFrameDesc* desc,
                                                                 SaccadeFrameHandle* out_frame) {
    ++host_calls;
    if ((runtime != expected_runtime) || (desc != &host_desc)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(401);
    return host_result;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(SaccadeRuntimeHandle runtime,
                                                                      const SaccadeIOSurfaceFrameDesc* desc,
                                                                      SaccadeFrameHandle* out_frame) {
    ++iosurface_calls;
    if ((runtime != expected_runtime) || (desc == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(402);
    return iosurface_result;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_win32_capture(SaccadeRuntimeHandle runtime,
                                                                          const SaccadeWin32CaptureFrameDesc* desc,
                                                                          SaccadeFrameHandle* out_frame) {
    ++win32_capture_calls;
    if ((runtime != expected_runtime) || (desc == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(403);
    return win32_capture_result;
}

int main() {
    const SaccadeWin32CaptureFrameDesc win32_capture_desc{static_cast<uint32_t>(sizeof(SaccadeWin32CaptureFrameDesc)),
                                                          SACCADE_API_VERSION,
                                                          reinterpret_cast<void*>(uintptr_t{17}),
                                                          0,
                                                          0,
                                                          2560,
                                                          1440,
                                                          UINT64_C(2),
                                                          UINT64_C(3),
                                                          nullptr,
                                                          0,
                                                          {0}};
    SaccadeFrameHandle frame = 0;

    static_assert(noexcept(saccade_frame_import(expected_runtime, &host_desc, &frame)));
    static_assert(noexcept(saccade_frame_import(expected_runtime, &win32_capture_desc, &frame)));

    host_desc.struct_size = static_cast<uint32_t>(sizeof(host_desc));
    host_desc.api_version = SACCADE_API_VERSION;

    const SaccadeResult host = saccade_frame_import(expected_runtime, next_host_desc(), &frame);
    if ((host != host_result) || (frame != UINT64_C(401)) || (host_calls != 1) || (descriptor_evaluations != 1)) {
        return 1;
    }

    frame = 0;
    const SaccadeResult win32_capture = saccade_frame_import(expected_runtime, &win32_capture_desc, &frame);
    if ((win32_capture != win32_capture_result) || (frame != UINT64_C(403)) || (win32_capture_calls != 1) ||
        (iosurface_calls != 0)) {
        return 2;
    }

    return 0;
}
