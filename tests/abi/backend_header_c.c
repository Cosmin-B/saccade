#include <saccade/saccade_backend.h>

static SaccadeResult SACCADE_CALL enumerate_devices(
    void* context,
    uint32_t index,
    SaccadeDeviceInfo* out_device) {
    (void)context;
    (void)index;
    (void)out_device;
    return SACCADE_OK;
}

int main(void) {
    SaccadeInferenceOps ops = {0};
    SaccadeInferenceProviderDesc provider = {0};

    ops.struct_size = (uint32_t)sizeof(ops);
    ops.api_version = SACCADE_API_VERSION;
    ops.enumerate_devices = enumerate_devices;

    provider.struct_size = (uint32_t)sizeof(provider);
    provider.api_version = SACCADE_API_VERSION;
    provider.info.struct_size = (uint32_t)sizeof(provider.info);
    provider.info.api_version = SACCADE_API_VERSION;
    provider.info.family = SACCADE_PROVIDER_FAMILY_INFERENCE;
    provider.ops = ops;

    return provider.ops.enumerate_devices(NULL, 0, NULL);
}
