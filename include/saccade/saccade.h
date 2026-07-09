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
    SACCADE_ERROR_ALREADY_EXISTS = -12
};

typedef uint64_t SaccadeRuntimeHandle;
typedef uint64_t SaccadeFrameHandle;
typedef uint64_t SaccadeTicketHandle;

typedef struct SaccadeSpanU8 {
    const uint8_t* data;
    size_t size;
} SaccadeSpanU8;

typedef struct SaccadeRuntimeDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t flags;
    uint64_t reserved[5];
} SaccadeRuntimeDesc;

typedef struct SaccadeHostFrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
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

typedef struct SaccadeD3D11FrameDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t shared_handle;
    uint32_t subresource;
    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint64_t frame_id;
    uint64_t transform_epoch;
    uint64_t reserved[3];
} SaccadeD3D11FrameDesc;

#define SACCADE_DETAIL_FRAME_IMPORT_TYPES(FIRST, NEXT)                         \
    FIRST(SaccadeHostFrameDesc,      saccade_frame_import_host)                \
    NEXT (SaccadeIOSurfaceFrameDesc, saccade_frame_import_iosurface)           \
    NEXT (SaccadeD3D11FrameDesc,     saccade_frame_import_d3d11)

#ifdef __cplusplus
extern "C" {
#endif

SACCADE_API SaccadeApiVersion SACCADE_CALL saccade_api_version(void);
SACCADE_API SaccadeSpanU8 SACCADE_CALL saccade_last_error(void);
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_create(
    const SaccadeRuntimeDesc* desc,
    SaccadeRuntimeHandle* out_runtime);
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_freeze(
    SaccadeRuntimeHandle runtime);
SACCADE_API SaccadeResult SACCADE_CALL saccade_runtime_destroy(
    SaccadeRuntimeHandle runtime);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_host(
    SaccadeRuntimeHandle runtime,
    const SaccadeHostFrameDesc* desc,
    SaccadeFrameHandle* out_frame);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_iosurface(
    SaccadeRuntimeHandle runtime,
    const SaccadeIOSurfaceFrameDesc* desc,
    SaccadeFrameHandle* out_frame);

SACCADE_API SaccadeResult SACCADE_CALL saccade_frame_import_d3d11(
    SaccadeRuntimeHandle runtime,
    const SaccadeD3D11FrameDesc* desc,
    SaccadeFrameHandle* out_frame);

#ifdef __cplusplus
}

#define SACCADE_DETAIL_IMPORT_OVERLOAD(type, function)                         \
    inline SaccadeResult saccade_frame_import(                                 \
        SaccadeRuntimeHandle runtime,                                          \
        const type* desc,                                                      \
        SaccadeFrameHandle* out_frame) noexcept {                              \
        return function(runtime, desc, out_frame);                             \
    }

SACCADE_DETAIL_FRAME_IMPORT_TYPES(
    SACCADE_DETAIL_IMPORT_OVERLOAD, SACCADE_DETAIL_IMPORT_OVERLOAD)

#undef SACCADE_DETAIL_IMPORT_OVERLOAD
#else
#define SACCADE_DETAIL_IMPORT_FIRST(type, function)                            \
    type*: function, const type*: function
#define SACCADE_DETAIL_IMPORT_NEXT(type, function)                             \
    , type*: function, const type*: function

#define saccade_frame_import(runtime, desc, out_frame)                         \
    _Generic((desc),                                                           \
        SACCADE_DETAIL_FRAME_IMPORT_TYPES(                                     \
            SACCADE_DETAIL_IMPORT_FIRST, SACCADE_DETAIL_IMPORT_NEXT)           \
    )((runtime), (desc), (out_frame))
#endif

#endif
