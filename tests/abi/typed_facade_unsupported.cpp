#include <saccade/saccade.h>

struct UnsupportedFrameDesc {
    uint32_t value;
};

int main() {
    UnsupportedFrameDesc desc{};
    SaccadeFrameHandle frame = 0;
    return saccade_frame_import(UINT64_C(1), &desc, &frame);
}
