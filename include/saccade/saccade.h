#ifndef SACCADE_SACCADE_H
#define SACCADE_SACCADE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define SACCADE_CALL __cdecl
#if defined(SACCADE_SHARED_BUILD)
#define SACCADE_API __declspec(dllexport)
#elif defined(SACCADE_SHARED_USE)
#define SACCADE_API __declspec(dllimport)
#else
#define SACCADE_API
#endif
#else
#define SACCADE_CALL
#define SACCADE_API __attribute__((visibility("default")))
#endif

#define SACCADE_API_VERSION UINT32_C(0x00010000)

typedef int32_t SaccadeResult;
typedef uint32_t SaccadeApiVersion;

enum {
    SACCADE_OK = 0,
    SACCADE_ERROR_INVALID_ARGUMENT = -1,
    SACCADE_ERROR_VERSION = -2,
    SACCADE_ERROR_CAPACITY = -3,
    SACCADE_ERROR_STALE_HANDLE = -4,
    SACCADE_ERROR_UNSUPPORTED = -5,
    SACCADE_ERROR_BACKEND = -6,
    SACCADE_ERROR_STATE = -7,
    SACCADE_ERROR_NOT_FOUND = -8,
    SACCADE_ERROR_TIMEOUT = -9,
    SACCADE_ERROR_CANCELLED = -10,
    SACCADE_ERROR_BUSY = -11,
    SACCADE_ERROR_ALREADY_EXISTS = -12,
    SACCADE_ERROR_PERMISSION = -13
};

typedef uint64_t SaccadeRuntimeHandle;
typedef uint64_t SaccadeFrameHandle;
typedef uint64_t SaccadeTicketHandle;

typedef struct SaccadeSpanU8 {
    const uint8_t* data;
    size_t size;
} SaccadeSpanU8;

/* Extensible runtime and provider structs in this API begin with struct_size
   and api_version. Callers set struct_size to the bytes they provide.
   The runtime accepts a larger same-major struct and requires every reserved
   byte beyond the last known field to be zero. Anything else fails with
   SACCADE_ERROR_INVALID_ARGUMENT. Agent, scene, overlay, and input packet
   headers carry their own exact-version rules instead. See
   docs/architecture/abi.md. */
typedef struct SaccadeRuntimeDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t flags;
    uint64_t reserved[5];
} SaccadeRuntimeDesc;

typedef struct SaccadeHostFrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
    /* Borrowed, not copied. See saccade_frame_import_host. */
    SaccadeSpanU8 data;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride_bytes;
    uint32_t pixel_format;
    uint64_t frame_id;
    uint64_t transform_epoch;
    uint64_t reserved[2];
} SaccadeHostFrameDesc;

typedef struct SaccadeIOSurfaceFrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t iosurface_id;
    uint32_t plane_index;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint64_t frame_id;
    uint64_t transform_epoch;
    uint64_t reserved[3];
} SaccadeIOSurfaceFrameDesc;

typedef struct SaccadeWin32CaptureFrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
    void* texture;
    uint32_t subresource;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint64_t frame_id;
    uint64_t transform_epoch;
    void* ready_fence;
    uint64_t ready_value;
    uint64_t reserved[1];
} SaccadeWin32CaptureFrameDesc;

#define SACCADE_DETAIL_FRAME_IMPORT_TYPES(FIRST, NEXT)                                                                 \
    FIRST(SaccadeHostFrameDesc, saccade_frame_import_host)                                                             \
    NEXT(SaccadeIOSurfaceFrameDesc, saccade_frame_import_iosurface)                                                    \
    NEXT(SaccadeWin32CaptureFrameDesc, saccade_frame_import_win32_capture)

#ifdef __cplusplus
extern "C" {
#endif

SACCADE_API SaccadeApiVersion SACCADE_CALL saccade_api_version(void);
/* Returns the calling thread's last error text. The span is thread-local. It
   stays valid only until the next Saccade call on this thread that returns a
   SaccadeResult. That call clears or replaces it. */
SACCADE_API SaccadeSpanU8 SACCADE_CALL saccade_last_error(void);
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_create(const SaccadeRuntimeDesc* desc,
                                                              SaccadeRuntimeHandle* out_runtime);
/* Freezing is one-way: register providers before this call, create inference
   sessions after it. Doing either in the wrong order fails with
   SACCADE_ERROR_STATE. There is no unfreeze and no unregister, because the
   registry uses fixed storage that must not change while the pipeline runs. */
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_freeze(SaccadeRuntimeHandle runtime);
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_destroy(SaccadeRuntimeHandle runtime);

/* Frame imports borrow the caller's bytes or native resource without copying.
   For host frames, desc->data must remain valid until saccade_frame_release
   returns for this frame. Release removes a pending frame from the
   newest-frame mailbox before returning buffer ownership to the caller. */
SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(SaccadeRuntimeHandle runtime,
                                                                 const SaccadeHostFrameDesc* desc,
                                                                 SaccadeFrameHandle* out_frame);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(SaccadeRuntimeHandle runtime,
                                                                      const SaccadeIOSurfaceFrameDesc* desc,
                                                                      SaccadeFrameHandle* out_frame);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_win32_capture(SaccadeRuntimeHandle runtime,
                                                                          const SaccadeWin32CaptureFrameDesc* desc,
                                                                          SaccadeFrameHandle* out_frame);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_release(SaccadeRuntimeHandle runtime, SaccadeFrameHandle frame);

#ifdef __cplusplus
}

#define SACCADE_DETAIL_IMPORT_OVERLOAD(type, function)                                                                 \
    inline SaccadeResult saccade_frame_import(SaccadeRuntimeHandle runtime, const type* desc,                          \
                                              SaccadeFrameHandle* out_frame) noexcept {                                \
        return function(runtime, desc, out_frame);                                                                     \
    }

SACCADE_DETAIL_FRAME_IMPORT_TYPES(SACCADE_DETAIL_IMPORT_OVERLOAD, SACCADE_DETAIL_IMPORT_OVERLOAD)

#undef SACCADE_DETAIL_IMPORT_OVERLOAD
#else
#define SACCADE_DETAIL_IMPORT_FIRST(type, function) type* : function, const type* : function
#define SACCADE_DETAIL_IMPORT_NEXT(type, function) , type* : function, const type* : function

#define saccade_frame_import(runtime, desc, out_frame)                                                                 \
    _Generic((desc), SACCADE_DETAIL_FRAME_IMPORT_TYPES(SACCADE_DETAIL_IMPORT_FIRST, SACCADE_DETAIL_IMPORT_NEXT))(      \
        (runtime), (desc), (out_frame))
#endif

#endif
