#include <saccade/saccade.h>

static_assert(offsetof(SaccadeHostFrameDesc, struct_size) == 0,
              "host descriptor size prefix");
static_assert(offsetof(SaccadeHostFrameDesc, api_version) == 4,
              "host descriptor version prefix");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, struct_size) == 0,
              "IOSurface descriptor size prefix");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, api_version) == 4,
              "IOSurface descriptor version prefix");
static_assert(offsetof(SaccadeD3D11FrameDesc, struct_size) == 0,
              "D3D11 descriptor size prefix");
static_assert(offsetof(SaccadeD3D11FrameDesc, api_version) == 4,
              "D3D11 descriptor version prefix");
static_assert(offsetof(SaccadeHostFrameDesc, transform_epoch) == 48,
              "host descriptor transform epoch offset");
static_assert(offsetof(SaccadeIOSurfaceFrameDesc, transform_epoch) == 40,
              "IOSurface descriptor transform epoch offset");
static_assert(offsetof(SaccadeD3D11FrameDesc, transform_epoch) == 40,
              "D3D11 descriptor transform epoch offset");

namespace {

constexpr SaccadeResult host_result = 41;
constexpr SaccadeResult iosurface_result = 42;
constexpr SaccadeResult d3d11_result = 43;
constexpr SaccadeRuntimeHandle expected_runtime = UINT64_C(0x8070605040302010);

SaccadeHostFrameDesc host_desc{};
int host_calls = 0;
int iosurface_calls = 0;
int d3d11_calls = 0;
int descriptor_evaluations = 0;

SaccadeHostFrameDesc* next_host_desc() noexcept {
    ++descriptor_evaluations;
    return &host_desc;
}

}  // namespace

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(
    SaccadeRuntimeHandle runtime,
    const SaccadeHostFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++host_calls;
    if ((runtime != expected_runtime) || (desc != &host_desc)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(401);
    return host_result;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(
    SaccadeRuntimeHandle runtime,
    const SaccadeIOSurfaceFrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++iosurface_calls;
    if ((runtime != expected_runtime) || (desc == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(402);
    return iosurface_result;
}

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_d3d11(
    SaccadeRuntimeHandle runtime,
    const SaccadeD3D11FrameDesc* desc,
    SaccadeFrameHandle* out_frame) {
    ++d3d11_calls;
    if ((runtime != expected_runtime) || (desc == nullptr)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *out_frame = UINT64_C(403);
    return d3d11_result;
}

int main() {
    const SaccadeD3D11FrameDesc d3d11_desc{
        static_cast<uint32_t>(sizeof(SaccadeD3D11FrameDesc)),
        SACCADE_API_VERSION,
        UINT64_C(17),
        0,
        0,
        2560,
        1440,
        UINT64_C(2),
        UINT64_C(3),
        {0}
    };
    SaccadeFrameHandle frame = 0;

    static_assert(noexcept(saccade_frame_import(expected_runtime, &host_desc, &frame)));
    static_assert(noexcept(saccade_frame_import(expected_runtime, &d3d11_desc, &frame)));

    host_desc.struct_size = static_cast<uint32_t>(sizeof(host_desc));
    host_desc.api_version = SACCADE_API_VERSION;

    const SaccadeResult host =
        saccade_frame_import(expected_runtime, next_host_desc(), &frame);
    if ((host != host_result) || (frame != UINT64_C(401)) ||
        (host_calls != 1) || (descriptor_evaluations != 1)) {
        return 1;
    }

    frame = 0;
    const SaccadeResult d3d11 =
        saccade_frame_import(expected_runtime, &d3d11_desc, &frame);
    if ((d3d11 != d3d11_result) || (frame != UINT64_C(403)) ||
        (d3d11_calls != 1) || (iosurface_calls != 0)) {
        return 2;
    }

    return 0;
}
