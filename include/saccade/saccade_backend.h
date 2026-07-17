#ifndef SACCADE_SACCADE_BACKEND_H
#define SACCADE_SACCADE_BACKEND_H

#include <saccade/saccade.h>
#include <saccade/saccade_input.h>
#include <saccade/saccade_overlay.h>
#include <saccade/saccade_scene.h>

typedef uint64_t SaccadeProviderHandle;
typedef uint64_t SaccadeDeviceHandle;
typedef uint64_t SaccadeModelHandle;
typedef uint64_t SaccadeExecutionContextHandle;
typedef uint64_t SaccadeCaptureStreamHandle;
typedef uint64_t SaccadeOverlayHandle;
typedef uint64_t SaccadeSnapshotHandle;

typedef uint32_t SaccadeProviderFamily;

enum {
    SACCADE_PROVIDER_FAMILY_INFERENCE = 1,
    SACCADE_PROVIDER_FAMILY_CAPTURE = 2,
    SACCADE_PROVIDER_FAMILY_OVERLAY = 3,
    SACCADE_PROVIDER_FAMILY_ACCESSIBILITY = 4,
    SACCADE_PROVIDER_FAMILY_INPUT = 5
};

typedef uint32_t SaccadeProviderCapabilityBits;

enum {
    SACCADE_PROVIDER_CAPABILITY_CPU = UINT32_C(1) << 0,
    SACCADE_PROVIDER_CAPABILITY_GPU = UINT32_C(1) << 1,
    SACCADE_PROVIDER_CAPABILITY_ACCELERATOR = UINT32_C(1) << 2,
    SACCADE_PROVIDER_CAPABILITY_HOST_IMPORT = UINT32_C(1) << 3,
    SACCADE_PROVIDER_CAPABILITY_NATIVE_IMPORT = UINT32_C(1) << 4,
    SACCADE_PROVIDER_CAPABILITY_ASYNC = UINT32_C(1) << 5,
    SACCADE_PROVIDER_CAPABILITY_CANCELLATION = UINT32_C(1) << 6,
    SACCADE_PROVIDER_CAPABILITY_DAMAGE = UINT32_C(1) << 7
};

typedef uint32_t SaccadeFormatBits;

enum {
    SACCADE_FORMAT_BGRA8 = UINT32_C(1) << 0,
    SACCADE_FORMAT_RGBA8 = UINT32_C(1) << 1,
    SACCADE_FORMAT_BGRX8 = UINT32_C(1) << 2,
    SACCADE_FORMAT_R8 = UINT32_C(1) << 3,
    SACCADE_FORMAT_RGB_F16 = UINT32_C(1) << 4
};

typedef uint32_t SaccadePrecisionBits;

enum {
    SACCADE_PRECISION_FP32 = UINT32_C(1) << 0,
    SACCADE_PRECISION_FP16 = UINT32_C(1) << 1,
    SACCADE_PRECISION_INT8 = UINT32_C(1) << 2,
    SACCADE_PRECISION_INT4 = UINT32_C(1) << 3
};

typedef uint32_t SaccadeImportBits;

enum {
    SACCADE_IMPORT_HOST = UINT32_C(1) << 0,
    SACCADE_IMPORT_IOSURFACE = UINT32_C(1) << 1,
    SACCADE_IMPORT_WIN32_CAPTURE = UINT32_C(1) << 2,
    SACCADE_IMPORT_DMABUF = UINT32_C(1) << 3
};

typedef uint32_t SaccadeTicketState;

enum {
    SACCADE_TICKET_QUEUED = 1,
    SACCADE_TICKET_RUNNING = 2,
    SACCADE_TICKET_COMPLETE = 3,
    SACCADE_TICKET_CANCELLED = 4,
    SACCADE_TICKET_FAILED = 5
};

typedef struct SaccadeMutableSpanU8 {
    uint8_t* data;
    size_t size;
} SaccadeMutableSpanU8;

typedef struct SaccadeRectI32 {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} SaccadeRectI32;

typedef struct SaccadeProviderInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t family;
    uint32_t capability_bits;
    uint64_t stable_id;
    SaccadeSpanU8 name;
    uint64_t reserved[6];
} SaccadeProviderInfo;

typedef struct SaccadeDeviceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stable_id;
    uint32_t capability_bits;
    uint32_t format_bits;
    uint32_t precision_bits;
    uint32_t import_bits;
    uint32_t queue_capacity;
    uint32_t max_in_flight;
    uint64_t host_alignment;
    uint64_t device_alignment;
    SaccadeSpanU8 name;
    uint64_t reserved[4];
} SaccadeDeviceInfo;

typedef struct SaccadeMemoryStats {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t host_committed;
    uint64_t host_reserved;
    uint64_t device_imported;
    uint64_t device_owned;
    uint64_t framework_opaque;
    uint64_t copied_bytes;
    uint64_t high_water_bytes;
    uint64_t reserved[5];
} SaccadeMemoryStats;

typedef struct SaccadeModelInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stable_id;
    uint64_t required_host_bytes;
    uint64_t required_device_bytes;
    uint32_t capability_bits;
    uint32_t max_output_bytes;
    SaccadeSpanU8 name;
    uint64_t reserved[4];
} SaccadeModelInfo;

typedef struct SaccadeModelDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeSpanU8 bytes;
    uint64_t stable_id;
    uint64_t device_id;
    uint64_t flags;
    uint64_t reserved[5];
} SaccadeModelDesc;

typedef struct SaccadeExecutionContextDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeModelHandle model;
    uint64_t device_id;
    uint32_t queue_capacity;
    uint32_t max_in_flight;
    uint64_t flags;
    uint64_t reserved[5];
} SaccadeExecutionContextDesc;

typedef uint32_t SaccadeFrameStorageKind;

enum { SACCADE_FRAME_STORAGE_HOST = 1, SACCADE_FRAME_STORAGE_IOSURFACE = 2, SACCADE_FRAME_STORAGE_WIN32_CAPTURE = 3 };

typedef struct SaccadeFrameResourceView {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeFrameStorageKind storage;
    uint32_t pixel_format;
    SaccadeSpanU8 host_data;
    uint64_t native_id;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride_bytes;
    uint32_t plane_or_subresource;
    uint64_t frame_id;
    uint64_t transform_epoch;
    uint64_t ready_fence;
    uint64_t ready_value;
} SaccadeFrameResourceView;

typedef struct SaccadeInferenceSubmitDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeFrameHandle frame;
    SaccadeRectI32 scope;
    const SaccadeRectI32* priority_regions;
    uint32_t priority_region_count;
    uint32_t output_capacity;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
    uint64_t flags;
    uint64_t reserved[4];
} SaccadeInferenceSubmitDesc;

typedef struct SaccadeInferenceDispatchDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeFrameResourceView frame;
    SaccadeRectI32 scope;
    const SaccadeRectI32* priority_regions;
    uint32_t priority_region_count;
    uint32_t output_capacity;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
    uint64_t flags;
    uint64_t reserved[4];
} SaccadeInferenceDispatchDesc;

typedef struct SaccadeInferenceStatus {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    SaccadeResult result;
    SaccadeTicketHandle ticket;
    uint64_t frame_id;
    uint64_t model_epoch;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t source_id;
    uint32_t produced_bytes;
    uint32_t required_bytes;
    uint64_t reserved[4];
} SaccadeInferenceStatus;

typedef struct SaccadeInferenceSessionDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeSpanU8 model_bytes;
    uint64_t model_stable_id;
    uint64_t provider_stable_id;
    uint64_t device_stable_id;
    uint32_t required_capability_bits;
    uint32_t preferred_capability_bits;
    uint32_t required_format_bits;
    uint32_t required_precision_bits;
    uint32_t required_import_bits;
    uint32_t queue_capacity;
    uint32_t max_in_flight;
    uint32_t reserved32;
    uint64_t flags;
    uint64_t reserved[4];
} SaccadeInferenceSessionDesc;

typedef struct SaccadeInferenceSessionInfo {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeExecutionContextHandle session;
    uint64_t provider_stable_id;
    uint64_t device_stable_id;
    uint64_t model_stable_id;
    uint32_t capability_bits;
    uint32_t format_bits;
    uint32_t precision_bits;
    uint32_t import_bits;
    uint32_t max_output_bytes;
    uint32_t queue_capacity;
    uint32_t max_in_flight;
    uint32_t reserved32;
    uint64_t reserved[4];
} SaccadeInferenceSessionInfo;

typedef uint32_t SaccadeCaptureSourceKind;

enum { SACCADE_CAPTURE_SOURCE_DISPLAY = 1, SACCADE_CAPTURE_SOURCE_WINDOW = 2, SACCADE_CAPTURE_SOURCE_REGION = 3 };

typedef struct SaccadeCaptureSourceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stable_id;
    uint32_t kind;
    uint32_t capability_bits;
    SaccadeRectI32 desktop_bounds;
    SaccadeSpanU8 name;
    uint64_t reserved[5];
} SaccadeCaptureSourceInfo;

typedef struct SaccadeCaptureStreamDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t source_id;
    uint32_t pixel_format;
    uint32_t queue_capacity;
    /* Zero selects native size. Nonzero values form an aspect-preserving fit box. */
    uint32_t max_width;
    uint32_t max_height;
    uint64_t flags;
    uint64_t reserved[5];
} SaccadeCaptureStreamDesc;

typedef struct SaccadeCapturedFrame {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeFrameHandle frame;
    uint64_t source_id;
    uint64_t frame_id;
    uint64_t transform_epoch;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t damage_count;
    uint64_t reserved[4];
} SaccadeCapturedFrame;

typedef struct SaccadeOverlayDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t source_id;
    SaccadeRectI32 desktop_bounds;
    uint32_t queue_capacity;
    uint32_t flags;
    uint64_t reserved[5];
} SaccadeOverlayDesc;

typedef struct SaccadeOverlayFrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
    /* Changing either epoch publishes a new immutable packet snapshot. */
    uint64_t scene_epoch;
    uint64_t transform_epoch;
    SaccadeSpanU8 packet;
    uint32_t flags;
    uint32_t active_target_index;
    uint64_t reserved[5];
} SaccadeOverlayFrameDesc;

typedef struct SaccadeWindowInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t stable_id;
    uint64_t process_id;
    SaccadeRectI32 desktop_bounds;
    uint32_t flags;
    uint32_t reserved0;
    SaccadeSpanU8 title;
    uint64_t reserved[4];
} SaccadeWindowInfo;

typedef struct SaccadeAccessibilityQueryDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t window_id;
    SaccadeRectI32 scope;
    uint32_t target_capacity;
    uint32_t flags;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint64_t topology_epoch;
    uint64_t frame_id;
    uint64_t reserved[2];
} SaccadeAccessibilityQueryDesc;

typedef struct SaccadeAccessibilityStatus {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    SaccadeResult result;
    SaccadeTicketHandle ticket;
    SaccadeSnapshotHandle snapshot;
    uint64_t session_epoch;
    uint64_t transform_epoch;
    uint32_t target_count;
    uint32_t required_bytes;
    uint64_t topology_epoch;
    uint64_t frame_id;
    uint64_t reserved[2];
} SaccadeAccessibilityStatus;

typedef struct SaccadeInputPlanDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeSpanU8 plan;
    uint64_t flags;
    uint64_t reserved[4];
} SaccadeInputPlanDesc;

typedef struct SaccadeInputStatus {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    SaccadeResult result;
    SaccadeTicketHandle ticket;
    uint64_t session_epoch;
    uint32_t completed_actions;
    uint32_t total_actions;
    uint64_t reserved[5];
} SaccadeInputStatus;

#ifdef __cplusplus
extern "C" {
#endif

typedef SaccadeResult(SACCADE_CALL* SaccadeEnumerateDevicesFn)(void*, uint32_t, SaccadeDeviceInfo*);
typedef SaccadeResult(SACCADE_CALL* SaccadeQueryModelFn)(void*, SaccadeSpanU8, SaccadeModelInfo*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCreateModelFn)(void*, const SaccadeModelDesc*, SaccadeModelHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadeDestroyModelFn)(void*, SaccadeModelHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeCreateExecutionContextFn)(void*, const SaccadeExecutionContextDesc*,
                                                                     SaccadeExecutionContextHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadeDestroyExecutionContextFn)(void*, SaccadeExecutionContextHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeSubmitInferenceFn)(void*, SaccadeExecutionContextHandle,
                                                              const SaccadeInferenceDispatchDesc*,
                                                              SaccadeTicketHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadePollInferenceFn)(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                                            SaccadeInferenceStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeWaitInferenceFn)(void*, SaccadeExecutionContextHandle, SaccadeTicketHandle,
                                                            uint64_t, SaccadeInferenceStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCollectInferenceFn)(void*, SaccadeExecutionContextHandle,
                                                               SaccadeTicketHandle, SaccadeMutableSpanU8, size_t*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCancelInferenceFn)(void*, SaccadeExecutionContextHandle,
                                                              SaccadeTicketHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeResetInferenceFn)(void*, SaccadeExecutionContextHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeSynchronizeInferenceFn)(void*, SaccadeExecutionContextHandle, uint64_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeInferenceMemoryStatsFn)(void*, SaccadeExecutionContextHandle,
                                                                   SaccadeMemoryStats*);

typedef struct SaccadeInferenceOps {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeEnumerateDevicesFn enumerate_devices;
    SaccadeQueryModelFn query_model;
    SaccadeCreateModelFn create_model;
    SaccadeDestroyModelFn destroy_model;
    SaccadeCreateExecutionContextFn create_context;
    SaccadeDestroyExecutionContextFn destroy_context;
    SaccadeSubmitInferenceFn submit;
    SaccadePollInferenceFn poll;
    SaccadeWaitInferenceFn wait;
    SaccadeCollectInferenceFn collect;
    SaccadeCancelInferenceFn cancel;
    SaccadeResetInferenceFn reset;
    SaccadeSynchronizeInferenceFn synchronize;
    SaccadeInferenceMemoryStatsFn memory_stats;
    uint64_t reserved[8];
} SaccadeInferenceOps;

typedef SaccadeResult(SACCADE_CALL* SaccadeEnumerateCaptureSourcesFn)(void*, uint32_t, SaccadeCaptureSourceInfo*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCreateCaptureStreamFn)(void*, const SaccadeCaptureStreamDesc*,
                                                                  SaccadeCaptureStreamHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadeDestroyCaptureStreamFn)(void*, SaccadeCaptureStreamHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeStartCaptureFn)(void*, SaccadeCaptureStreamHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeStopCaptureFn)(void*, SaccadeCaptureStreamHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeAcquireFrameFn)(void*, SaccadeCaptureStreamHandle, uint64_t,
                                                           SaccadeCapturedFrame*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCopyDamageFn)(void*, SaccadeCaptureStreamHandle, SaccadeFrameHandle,
                                                         SaccadeRectI32*, uint32_t, uint32_t*);
typedef SaccadeResult(SACCADE_CALL* SaccadeReleaseFrameFn)(void*, SaccadeCaptureStreamHandle, SaccadeFrameHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeSynchronizeCaptureFn)(void*, SaccadeCaptureStreamHandle, uint64_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeCaptureMemoryStatsFn)(void*, SaccadeCaptureStreamHandle,
                                                                 SaccadeMemoryStats*);

typedef struct SaccadeCaptureOps {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeEnumerateCaptureSourcesFn enumerate_sources;
    SaccadeCreateCaptureStreamFn create;
    SaccadeDestroyCaptureStreamFn destroy;
    SaccadeStartCaptureFn start;
    SaccadeStopCaptureFn stop;
    SaccadeAcquireFrameFn acquire;
    SaccadeCopyDamageFn copy_damage;
    SaccadeReleaseFrameFn release;
    SaccadeSynchronizeCaptureFn synchronize;
    SaccadeCaptureMemoryStatsFn memory_stats;
    uint64_t reserved[8];
} SaccadeCaptureOps;

typedef SaccadeResult(SACCADE_CALL* SaccadeCreateOverlayFn)(void*, const SaccadeOverlayDesc*, SaccadeOverlayHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadeDestroyOverlayFn)(void*, SaccadeOverlayHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeSubmitOverlayFn)(void*, SaccadeOverlayHandle,
                                                            const SaccadeOverlayFrameDesc*);
typedef SaccadeResult(SACCADE_CALL* SaccadeSetOverlayVisibleFn)(void*, SaccadeOverlayHandle, uint32_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeSynchronizeOverlayFn)(void*, SaccadeOverlayHandle, uint64_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeOverlayMemoryStatsFn)(void*, SaccadeOverlayHandle, SaccadeMemoryStats*);
typedef SaccadeResult(SACCADE_CALL* SaccadeResetOverlayFn)(void*, SaccadeOverlayHandle);

typedef struct SaccadeOverlayOps {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeCreateOverlayFn create;
    SaccadeDestroyOverlayFn destroy;
    SaccadeSubmitOverlayFn submit;
    SaccadeSetOverlayVisibleFn set_visible;
    SaccadeSynchronizeOverlayFn synchronize;
    SaccadeOverlayMemoryStatsFn memory_stats;
    SaccadeResetOverlayFn reset;
    uint64_t reserved[8];
} SaccadeOverlayOps;

typedef SaccadeResult(SACCADE_CALL* SaccadeEnumerateWindowsFn)(void*, uint32_t, SaccadeWindowInfo*);
typedef SaccadeResult(SACCADE_CALL* SaccadeRequestAccessibilityFn)(void*, const SaccadeAccessibilityQueryDesc*,
                                                                   SaccadeTicketHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadePollAccessibilityFn)(void*, SaccadeTicketHandle,
                                                                SaccadeAccessibilityStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeWaitAccessibilityFn)(void*, SaccadeTicketHandle, uint64_t,
                                                                SaccadeAccessibilityStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCollectAccessibilityFn)(void*, SaccadeSnapshotHandle, SaccadeMutableSpanU8,
                                                                   size_t*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCancelAccessibilityFn)(void*, SaccadeTicketHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeReleaseSnapshotFn)(void*, SaccadeSnapshotHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeSynchronizeAccessibilityFn)(void*, uint64_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeAccessibilityMemoryStatsFn)(void*, SaccadeMemoryStats*);

typedef struct SaccadeAccessibilityOps {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeEnumerateWindowsFn enumerate_windows;
    SaccadeRequestAccessibilityFn request;
    SaccadePollAccessibilityFn poll;
    SaccadeWaitAccessibilityFn wait;
    SaccadeCollectAccessibilityFn collect;
    SaccadeCancelAccessibilityFn cancel;
    SaccadeReleaseSnapshotFn release;
    SaccadeSynchronizeAccessibilityFn synchronize;
    SaccadeAccessibilityMemoryStatsFn memory_stats;
    uint64_t reserved[8];
} SaccadeAccessibilityOps;

typedef SaccadeResult(SACCADE_CALL* SaccadeExecuteInputFn)(void*, const SaccadeInputPlanDesc*, SaccadeTicketHandle*);
typedef SaccadeResult(SACCADE_CALL* SaccadePollInputFn)(void*, SaccadeTicketHandle, SaccadeInputStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeWaitInputFn)(void*, SaccadeTicketHandle, uint64_t, SaccadeInputStatus*);
typedef SaccadeResult(SACCADE_CALL* SaccadeCancelInputFn)(void*, SaccadeTicketHandle);
typedef SaccadeResult(SACCADE_CALL* SaccadeReleaseAllInputFn)(void*);
typedef SaccadeResult(SACCADE_CALL* SaccadeSynchronizeInputFn)(void*, uint64_t);
typedef SaccadeResult(SACCADE_CALL* SaccadeResetInputFn)(void*);
typedef SaccadeResult(SACCADE_CALL* SaccadeInputMemoryStatsFn)(void*, SaccadeMemoryStats*);

typedef struct SaccadeInputOps {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeExecuteInputFn execute;
    SaccadePollInputFn poll;
    SaccadeWaitInputFn wait;
    SaccadeCancelInputFn cancel;
    SaccadeReleaseAllInputFn release_all;
    SaccadeSynchronizeInputFn synchronize;
    SaccadeResetInputFn reset;
    SaccadeInputMemoryStatsFn memory_stats;
    uint64_t reserved[8];
} SaccadeInputOps;

typedef struct SaccadeInferenceProviderDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeProviderInfo info;
    void* context;
    SaccadeInferenceOps ops;
    uint64_t reserved[4];
} SaccadeInferenceProviderDesc;

typedef struct SaccadeCaptureProviderDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeProviderInfo info;
    void* context;
    SaccadeCaptureOps ops;
    uint64_t reserved[4];
} SaccadeCaptureProviderDesc;

typedef struct SaccadeOverlayProviderDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeProviderInfo info;
    void* context;
    SaccadeOverlayOps ops;
    uint64_t reserved[4];
} SaccadeOverlayProviderDesc;

typedef struct SaccadeAccessibilityProviderDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeProviderInfo info;
    void* context;
    SaccadeAccessibilityOps ops;
    uint64_t reserved[4];
} SaccadeAccessibilityProviderDesc;

typedef struct SaccadeInputProviderDesc {
    uint32_t struct_size;
    uint32_t api_version;
    SaccadeProviderInfo info;
    void* context;
    SaccadeInputOps ops;
    uint64_t reserved[4];
} SaccadeInputProviderDesc;

SACCADE_API SaccadeResult SACCADE_CALL saccade_register_inference_provider(SaccadeRuntimeHandle,
                                                                           const SaccadeInferenceProviderDesc*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_register_capture_provider(SaccadeRuntimeHandle,
                                                                         const SaccadeCaptureProviderDesc*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_register_overlay_provider(SaccadeRuntimeHandle,
                                                                         const SaccadeOverlayProviderDesc*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_register_accessibility_provider(SaccadeRuntimeHandle,
                                                                               const SaccadeAccessibilityProviderDesc*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_register_input_provider(SaccadeRuntimeHandle,
                                                                       const SaccadeInputProviderDesc*);

SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_session_create(SaccadeRuntimeHandle,
                                                                        const SaccadeInferenceSessionDesc*,
                                                                        SaccadeExecutionContextHandle*,
                                                                        SaccadeInferenceSessionInfo*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_session_destroy(SaccadeRuntimeHandle,
                                                                         SaccadeExecutionContextHandle);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_submit(SaccadeRuntimeHandle, SaccadeExecutionContextHandle,
                                                                const SaccadeInferenceSubmitDesc*,
                                                                SaccadeTicketHandle*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_poll(SaccadeRuntimeHandle, SaccadeExecutionContextHandle,
                                                              SaccadeTicketHandle, SaccadeInferenceStatus*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_wait(SaccadeRuntimeHandle, SaccadeExecutionContextHandle,
                                                              SaccadeTicketHandle, uint64_t, SaccadeInferenceStatus*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_collect(SaccadeRuntimeHandle, SaccadeExecutionContextHandle,
                                                                 SaccadeTicketHandle, SaccadeMutableSpanU8, size_t*);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_cancel(SaccadeRuntimeHandle, SaccadeExecutionContextHandle,
                                                                SaccadeTicketHandle);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_reset(SaccadeRuntimeHandle, SaccadeExecutionContextHandle);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_synchronize(SaccadeRuntimeHandle,
                                                                     SaccadeExecutionContextHandle, uint64_t);
SACCADE_API SaccadeResult SACCADE_CALL saccade_inference_memory_stats(SaccadeRuntimeHandle,
                                                                      SaccadeExecutionContextHandle,
                                                                      SaccadeMemoryStats*);

#ifdef __cplusplus
}
#endif

#endif
