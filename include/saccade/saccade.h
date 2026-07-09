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
    SACCADE_ERROR_BACKEND = -6
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

#ifdef __cplusplus
extern "C" {
#endif

SACCADE_API SaccadeApiVersion SACCADE_CALL saccade_api_version(void);

#ifdef __cplusplus
}
#endif

#endif
