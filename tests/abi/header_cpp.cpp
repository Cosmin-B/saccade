#include <saccade/saccade.h>

static_assert(sizeof(SaccadeRuntimeHandle) == 8, "runtime handle ABI");
static_assert(sizeof(SaccadeSpanU8) == 16, "span ABI");

int main() {
    SaccadeRuntimeDesc desc = {};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    return saccade_api_version() == SACCADE_API_VERSION ? 0 : 1;
}
