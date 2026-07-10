#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

int main(void) {
    SaccadeRuntimeDesc desc = {0};
    SaccadeRuntimeHandle runtime = 0;
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    if (saccade_api_version() != SACCADE_API_VERSION ||
        saccade_runtime_create(&desc, &runtime) != SACCADE_OK || runtime == 0) {
        return 1;
    }
    return saccade_runtime_destroy(runtime) == SACCADE_OK ? 0 : 2;
}
