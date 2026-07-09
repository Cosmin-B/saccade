#include <saccade/saccade.h>

_Static_assert(offsetof(SaccadeHostFrameDesc, struct_size) == 0,
               "host descriptor size prefix");
_Static_assert(offsetof(SaccadeHostFrameDesc, api_version) == 4,
               "host descriptor version prefix");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, struct_size) == 0,
               "IOSurface descriptor size prefix");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, api_version) == 4,
               "IOSurface descriptor version prefix");
_Static_assert(offsetof(SaccadeD3D11FrameDesc, struct_size) == 0,
               "D3D11 descriptor size prefix");
_Static_assert(offsetof(SaccadeD3D11FrameDesc, api_version) == 4,
               "D3D11 descriptor version prefix");
_Static_assert(offsetof(SaccadeHostFrameDesc, transform_epoch) == 48,
               "host descriptor transform epoch offset");
_Static_assert(offsetof(SaccadeIOSurfaceFrameDesc, transform_epoch) == 40,
               "IOSurface descriptor transform epoch offset");
_Static_assert(offsetof(SaccadeD3D11FrameDesc, transform_epoch) == 40,
               "D3D11 descriptor transform epoch offset");

enum {
    HOST_RESULT = 11,
    IOSURFACE_RESULT = 22,
    D3D11_RESULT = 33
};

static const SaccadeRuntimeHandle expected_runtime = UINT64_C(0x1020304050607080);
static SaccadeHostFrameDesc host_desc;
static int host_calls;
static int iosurface_calls;
static int d3d11_calls;
static int descriptor_evaluations;

static SaccadeHostFrameDesc* next_host_desc(void) {
    ++descriptor_evaluations;
    return &host_desc;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(
    SaccadeRuntimeHandle runtime,
    const SaccadeHostFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++host_calls;
    if ((runtime != expected_runtime) || (desc != &host_desc)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(101);
    return HOST_RESULT;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(
    SaccadeRuntimeHandle runtime,
    const SaccadeIOSurfaceFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++iosurface_calls;
    if ((runtime != expected_runtime) || (desc == NULL)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(202);
    return IOSURFACE_RESULT;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_d3d11(
    SaccadeRuntimeHandle runtime,
    const SaccadeD3D11FrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++d3d11_calls;
    if ((runtime != expected_runtime) || (desc == NULL)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(303);
    return D3D11_RESULT;
}

int main(void) {
    const SaccadeIOSurfaceFrameDesc iosurface_desc = {
        (uint32_t)sizeof(SaccadeIOSurfaceFrameDesc),
        SACCADE_API_VERSION,
        UINT64_C(7),
        0,
        0,
        1920,
        1080,
        UINT64_C(1),
        UINT64_C(2),
        {0}
    };
    SaccadeFrameHandle frame = 0;
    SaccadeResult result;

    host_desc.struct_size = (uint32_t)sizeof(host_desc);
    host_desc.api_version = SACCADE_API_VERSION;

    result = saccade_frame_import(expected_runtime, next_host_desc(), &frame);
    if ((result != HOST_RESULT) || (frame != UINT64_C(101)) ||
        (host_calls != 1) || (descriptor_evaluations != 1)) {
        return 1;
    }

    frame = 0;
    result = saccade_frame_import(expected_runtime, &iosurface_desc, &frame);
    if ((result != IOSURFACE_RESULT) || (frame != UINT64_C(202)) ||
        (iosurface_calls != 1) || (d3d11_calls != 0)) {
        return 2;
    }

    return 0;
}
