#include <saccade/saccade.h>

_Static_assert(offsetof(SaccadeHostFrameDesc, struct_size) == 0, "host descriptor size prefix");
_Static_assert(offsetof(SaccadeHostFrameDesc, api_version) == 4, "host descriptor version prefix");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, struct_size) == 0, "IOSurface descriptor size prefix");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, api_version) == 4, "IOSurface descriptor version prefix");
_Static_assert(offsetof(SaccadeWin32CaptureFrameDesc, struct_size) == 0, "Win32 capture descriptor size prefix");
_Static_assert(offsetof(SaccadeWin32CaptureFrameDesc, api_version) == 4, "Win32 capture descriptor version prefix");
_Static_assert(offsetof(SaccadeHostFrameDesc, transform_epoch) == 48, "host descriptor transform epoch offset");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, transform_epoch) == 40,
               "IOSurface descriptor transform epoch offset");
_Static_assert(offsetof(SaccadeWin32CaptureFrameDesc, transform_epoch) == 40,
               "Win32 capture descriptor transform epoch offset");
_Static_assert(offsetof(SaccadeWin32CaptureFrameDesc, ready_fence) == 48,
               "Win32 capture descriptor ready fence offset");
_Static_assert(offsetof(SaccadeWin32CaptureFrameDesc, ready_value) == 56,
               "Win32 capture descriptor ready value offset");

enum { HOST_RESULT = 11, IOSURFACE_RESULT = 22, WIN32_CAPTURE_RESULT = 33 };

static const SaccadeRuntimeHandle expected_runtime = UINT64_C(0x1020304050607080);
static SaccadeHostFrameDesc host_desc;
static int host_calls;
static int iosurface_calls;
static int win32_capture_calls;
static int descriptor_evaluations;

static SaccadeHostFrameDesc* next_host_desc(void) {
    ++descriptor_evaluations;
    return &host_desc;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(SaccadeRuntimeHandle runtime,
                                                                 const SaccadeHostFrameDesc* desc,
                                                                 SaccadeFrameHandle* out_frame) {
    ++host_calls;
    if ((runtime != expected_runtime) || (desc != &host_desc)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(101);
    return HOST_RESULT;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(SaccadeRuntimeHandle runtime,
                                                                      const SaccadeIOSurfaceFrameDesc* desc,
                                                                      SaccadeFrameHandle* out_frame) {
    ++iosurface_calls;
    if ((runtime != expected_runtime) || (desc == NULL)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(202);
    return IOSURFACE_RESULT;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_win32_capture(SaccadeRuntimeHandle runtime,
                                                                          const SaccadeWin32CaptureFrameDesc* desc,
                                                                          SaccadeFrameHandle* out_frame) {
    ++win32_capture_calls;
    if ((runtime != expected_runtime) || (desc == NULL)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(303);
    return WIN32_CAPTURE_RESULT;
}

int main(void) {
    const SaccadeIOSurfaceFrameDesc iosurface_desc = {(uint32_t)sizeof(SaccadeIOSurfaceFrameDesc),
                                                      SACCADE_API_VERSION,
                                                      UINT64_C(7),
                                                      0,
                                                      0,
                                                      1920,
                                                      1080,
                                                      UINT64_C(1),
                                                      UINT64_C(2),
                                                      {0}};
    SaccadeFrameHandle frame = 0;
    SaccadeResult result;

    host_desc.struct_size = (uint32_t)sizeof(host_desc);
    host_desc.api_version = SACCADE_API_VERSION;

    result = saccade_frame_import(expected_runtime, next_host_desc(), &frame);
    if ((result != HOST_RESULT) || (frame != UINT64_C(101)) || (host_calls != 1) || (descriptor_evaluations != 1)) {
        return 1;
    }

    frame = 0;
    result = saccade_frame_import(expected_runtime, &iosurface_desc, &frame);
    if ((result != IOSURFACE_RESULT) || (frame != UINT64_C(202)) || (iosurface_calls != 1) ||
        (win32_capture_calls != 0)) {
        return 2;
    }

    return 0;
}
