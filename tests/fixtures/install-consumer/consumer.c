#include <saccade/saccade.h>
#include <saccade/saccade_backend.h>

int main(void) {
    SaccadeRuntimeDesc desc = {0};
    SaccadeRuntimeHandle runtime = 0;
    uint8_t pixel[4] = {0};
    SaccadeHostFrameDesc frame = {0};
    SaccadeFrameHandle frame_handle = 0;
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.api_version = SACCADE_API_VERSION;
    if (saccade_api_version() != SACCADE_API_VERSION || saccade_runtime_create(&desc, &runtime) != SACCADE_OK ||
        runtime == 0) {
        return 1;
    }

    frame.struct_size = (uint32_t)sizeof(frame);
    frame.api_version = SACCADE_API_VERSION;
    frame.data.data = pixel;
    frame.data.size = sizeof(pixel);
    frame.width = 1;
    frame.height = 1;
    frame.row_stride_bytes = 4;
    frame.pixel_format = 1;
    frame.frame_id = 1;
    frame.transform_epoch = 1;
    if (saccade_frame_import(runtime, &frame, &frame_handle) != SACCADE_OK || frame_handle == 0 ||
        saccade_frame_release(runtime, frame_handle) != SACCADE_OK) {
        return 2;
    }
    return saccade_runtime_destroy(runtime) == SACCADE_OK ? 0 : 3;
}
