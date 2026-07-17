#include "model/mapped_artifact.hpp"

#include <cstdint>
#include <limits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace saccade::model {

MappedArtifact::~MappedArtifact() {
    (void)shutdown();
}

void MappedArtifact::unmap() noexcept {
#if defined(_WIN32)
    if (data_ != nullptr) (void)UnmapViewOfFile(data_);
    if (mapping_ != nullptr) (void)CloseHandle(mapping_);
    if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) (void)CloseHandle(file_);
    mapping_ = nullptr;
    file_ = nullptr;
#else
    if (data_ != nullptr && size_ != 0) (void)munmap(const_cast<uint8_t*>(data_), size_);
    if (file_ >= 0) (void)close(file_);
    file_ = -1;
#endif
    data_ = nullptr;
    size_ = 0;
    view_ = {};
}

SaccadeResult MappedArtifact::initialize(const char* path, ArtifactVerifier verifier) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (path == nullptr || path[0] == '\0' || verifier.verify == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
#if defined(_WIN32)
    std::array<wchar_t, 1024> wide_path{};
    const int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path.data(),
                                              static_cast<int>(wide_path.size()));
    if (converted == 0)
        return GetLastError() == ERROR_INSUFFICIENT_BUFFER ? SACCADE_ERROR_CAPACITY : SACCADE_ERROR_INVALID_ARGUMENT;
    HANDLE file = CreateFileW(wide_path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return SACCADE_ERROR_NOT_FOUND;
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size) == 0 || file_size.QuadPart <= 0 ||
        static_cast<uint64_t>(file_size.QuadPart) > UINT32_MAX) {
        (void)CloseHandle(file);
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        (void)CloseHandle(file);
        return SACCADE_ERROR_BACKEND;
    }
    const void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (mapped == nullptr) {
        (void)CloseHandle(mapping);
        (void)CloseHandle(file);
        return SACCADE_ERROR_BACKEND;
    }
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<const uint8_t*>(mapped);
    size_ = static_cast<size_t>(file_size.QuadPart);
#else
    const int file = open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) return SACCADE_ERROR_NOT_FOUND;
    struct stat status{};
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        static_cast<uint64_t>(status.st_size) > UINT32_MAX ||
        static_cast<uint64_t>(status.st_size) > std::numeric_limits<size_t>::max()) {
        (void)close(file);
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    void* mapped = mmap(nullptr, static_cast<size_t>(status.st_size), PROT_READ, MAP_PRIVATE, file, 0);
    if (mapped == MAP_FAILED) {
        (void)close(file);
        return SACCADE_ERROR_BACKEND;
    }
    file_ = file;
    data_ = static_cast<const uint8_t*>(mapped);
    size_ = static_cast<size_t>(status.st_size);
#endif
    SaccadeResult result = parse_artifact(bytes(), &view_);
    if (result == SACCADE_OK) result = verify_artifact(view_, verifier);
    if (result != SACCADE_OK) {
        unmap();
        return result;
    }
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult MappedArtifact::shutdown() noexcept {
    if (!initialized_) return SACCADE_OK;
    unmap();
    initialized_ = false;
    return SACCADE_OK;
}

} // namespace saccade::model
