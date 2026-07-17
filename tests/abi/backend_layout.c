#include <saccade/saccade_backend.h>

_Static_assert(sizeof(void*) == 8, "provider ABI requires a 64-bit target");

_Static_assert(sizeof(SaccadeProviderInfo) == 88, "provider info ABI");
_Static_assert(offsetof(SaccadeProviderInfo, struct_size) == 0, "provider info size offset");
_Static_assert(offsetof(SaccadeProviderInfo, api_version) == 4, "provider info version offset");
_Static_assert(offsetof(SaccadeProviderInfo, name) == 24, "provider info name offset");
_Static_assert(offsetof(SaccadeProviderInfo, reserved) == 40, "provider info reserve offset");

_Static_assert(sizeof(SaccadeDeviceInfo) == 104, "device info ABI");
_Static_assert(offsetof(SaccadeDeviceInfo, format_bits) == 20, "device format offset");
_Static_assert(offsetof(SaccadeDeviceInfo, precision_bits) == 24, "device precision offset");
_Static_assert(offsetof(SaccadeDeviceInfo, import_bits) == 28, "device import offset");
_Static_assert(offsetof(SaccadeDeviceInfo, name) == 56, "device info name offset");
_Static_assert(offsetof(SaccadeDeviceInfo, reserved) == 72, "device info reserve offset");

_Static_assert(offsetof(SaccadeModelDesc, device_id) == 32, "model device offset");
_Static_assert(offsetof(SaccadeExecutionContextDesc, device_id) == 16, "execution context device offset");

_Static_assert(sizeof(SaccadeMemoryStats) == 104, "memory stats ABI");
_Static_assert(offsetof(SaccadeMemoryStats, high_water_bytes) == 56, "memory high-water offset");
_Static_assert(offsetof(SaccadeMemoryStats, reserved) == 64, "memory stats reserve offset");

_Static_assert(sizeof(SaccadeInferenceOps) == 184, "inference operations ABI");
_Static_assert(offsetof(SaccadeInferenceOps, enumerate_devices) == 8, "inference first operation offset");
_Static_assert(offsetof(SaccadeInferenceOps, reserved) == 120, "inference reserve offset");

_Static_assert(sizeof(SaccadeCaptureOps) == 152, "capture operations ABI");
_Static_assert(offsetof(SaccadeCaptureOps, enumerate_sources) == 8, "capture first operation offset");
_Static_assert(offsetof(SaccadeCaptureOps, reserved) == 88, "capture reserve offset");

_Static_assert(sizeof(SaccadeOverlayOps) == 128, "overlay operations ABI");
_Static_assert(offsetof(SaccadeOverlayOps, create) == 8, "overlay first operation offset");
_Static_assert(offsetof(SaccadeOverlayOps, reserved) == 64, "overlay reserve offset");

_Static_assert(sizeof(SaccadeOverlayFrameDesc) == 88, "overlay frame ABI");
_Static_assert(offsetof(SaccadeOverlayFrameDesc, packet) == 24, "overlay packet offset");
_Static_assert(offsetof(SaccadeOverlayFrameDesc, flags) == 40, "overlay frame flags offset");
_Static_assert(offsetof(SaccadeOverlayFrameDesc, active_target_index) == 44, "overlay active target offset");
_Static_assert(sizeof(((SaccadeOverlayFrameDesc*)0)->packet) == sizeof(SaccadeSpanU8), "overlay packet span width");
_Static_assert(sizeof(((SaccadeOverlayFrameDesc*)0)->flags) == 4, "overlay frame flags width");
_Static_assert(sizeof(((SaccadeOverlayFrameDesc*)0)->active_target_index) == 4, "overlay active target width");

_Static_assert(sizeof(SaccadeAccessibilityOps) == 144, "accessibility operations ABI");
_Static_assert(sizeof(SaccadeAccessibilityQueryDesc) == 88, "accessibility query ABI");
_Static_assert(offsetof(SaccadeAccessibilityQueryDesc, topology_epoch) == 56,
               "accessibility query topology epoch offset");
_Static_assert(offsetof(SaccadeAccessibilityQueryDesc, frame_id) == 64, "accessibility query frame offset");
_Static_assert(sizeof(SaccadeAccessibilityStatus) == 88, "accessibility status ABI");
_Static_assert(offsetof(SaccadeAccessibilityStatus, topology_epoch) == 56,
               "accessibility status topology epoch offset");
_Static_assert(offsetof(SaccadeAccessibilityOps, enumerate_windows) == 8, "accessibility first operation offset");
_Static_assert(offsetof(SaccadeAccessibilityOps, reserved) == 80, "accessibility reserve offset");

_Static_assert(sizeof(SaccadeInputOps) == 136, "input operations ABI");
_Static_assert(offsetof(SaccadeInputOps, execute) == 8, "input first operation offset");
_Static_assert(offsetof(SaccadeInputOps, reserved) == 72, "input reserve offset");

_Static_assert(offsetof(SaccadeInferenceProviderDesc, info) == 8, "inference provider info offset");
_Static_assert(offsetof(SaccadeInferenceProviderDesc, context) == 96, "inference provider context offset");
_Static_assert(offsetof(SaccadeInferenceProviderDesc, ops) == 104, "inference provider operations offset");

int main(void) {
    return 0;
}
