#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

#include <array>

int main() {
    SaccadeRuntimeDesc desc{};
    desc.struct_size = static_cast<uint32_t>(sizeof(desc));
    desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&desc, &runtime) != SACCADE_OK) {
        return 1;
    }

    std::array<uint8_t, 4> pixel{};
    SaccadeHostFrameDesc frame{};
    frame.struct_size = static_cast<uint32_t>(sizeof(frame));
    frame.api_version = SACCADE_API_VERSION;
    frame.data = {pixel.data(), pixel.size()};
    frame.width = 1;
    frame.height = 1;
    frame.row_stride_bytes = 4;
    frame.pixel_format = 1;
    frame.frame_id = 1;
    frame.transform_epoch = 1;
    SaccadeFrameHandle frame_handle = 0;
    if (saccade_frame_import(runtime, &frame, &frame_handle) != SACCADE_OK ||
        frame_handle == 0 ||
        saccade_frame_release(runtime, frame_handle) != SACCADE_OK) {
        return 2;
    }
    return saccade_runtime_destroy(runtime) == SACCADE_OK ? 0 : 3;
}
