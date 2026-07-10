#include <saccade/saccade_backend.h>

#define SACCADE_ABI_SIZE(type, expected)                                                           \
    _Static_assert(sizeof(type) == (expected), #type " size changed");
#define SACCADE_ABI_OFFSET(type, member, expected)                                                 \
    _Static_assert(offsetof(type, member) == (expected), #type "." #member " offset changed");

#include <abi/layout-v1.def>

#undef SACCADE_ABI_OFFSET
#undef SACCADE_ABI_SIZE

int main(void) {
    return 0;
}
