#include <saccade/saccade_backend.h>

extern "C" SaccadeResult SACCADE_CALL enumerate_sources(void* context, uint32_t index,
                                                        SaccadeCaptureSourceInfo* out_source) {
    (void)context;
    (void)index;
    (void)out_source;
    return SACCADE_OK;
}

int main() {
    SaccadeCaptureOps ops{};
    SaccadeCaptureProviderDesc provider{};
    SaccadeOverlayFrameDesc frame{};

    if ((frame.flags & SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET) != 0) {
        return 1;
    }

    ops.struct_size = static_cast<uint32_t>(sizeof(ops));
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_sources = enumerate_sources;

    provider.struct_size = static_cast<uint32_t>(sizeof(provider));
    provider.api_version = SACCADE_API_VERSION;
    provider.info.struct_size = static_cast<uint32_t>(sizeof(provider.info));
    provider.info.api_version = SACCADE_API_VERSION;
    provider.info.family = SACCADE_PROVIDER_FAMILY_CAPTURE;
    provider.ops = ops;

    return provider.ops.enumerate_sources(nullptr, 0, nullptr);
}
