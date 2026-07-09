#include <saccade/saccade.h>

_Static_assert(sizeof(SaccadeRuntimeHandle) == 8, "runtime handle ABI");
_Static_assert(sizeof(SaccadeSpanU8) == 16, "span ABI");

int main(void) {
    SaccadeRuntimeDesc desc = {0};
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    return saccade_api_version() == SACCADE_API_VERSION ? 0 : 1;
}
