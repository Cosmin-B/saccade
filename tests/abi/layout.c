#include <saccade/saccade.h>

_Static_assert(sizeof(SaccadeRuntimeHandle) == 8, "runtime handle ABI");
_Static_assert(sizeof(SaccadeFrameHandle) == 8, "frame handle ABI");
_Static_assert(sizeof(SaccadeTicketHandle) == 8, "ticket handle ABI");
_Static_assert(sizeof(SaccadeSpanU8) == 16, "span ABI");
_Static_assert(sizeof(SaccadeRuntimeDesc) == 56, "runtime descriptor ABI");
_Static_assert(offsetof(SaccadeRuntimeDesc, struct_size) == 0,
               "runtime descriptor struct size offset");
_Static_assert(offsetof(SaccadeRuntimeDesc, api_version) == 4,
               "runtime descriptor API version offset");
_Static_assert(offsetof(SaccadeRuntimeDesc, flags) == 8,
               "runtime descriptor flags offset");
_Static_assert(offsetof(SaccadeRuntimeDesc, reserved) == 16,
               "runtime descriptor reserved offset");

int main(void) {
    return 0;
}
