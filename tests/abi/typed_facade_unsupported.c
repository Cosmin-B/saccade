#include <saccade/saccade.h>

typedef struct UnsupportedFrameDesc {
    uint32_t value;
} UnsupportedFrameDesc;

int main(void) {
    UnsupportedFrameDesc desc = {0};
    SaccadeFrameHandle frame = 0;
    return saccade_frame_import(UINT64_C(1), &desc, &frame);
}
