#ifndef SACCADE_PLATFORM_WINDOWS_RUNTIME_SCHEDULING_HPP
#define SACCADE_PLATFORM_WINDOWS_RUNTIME_SCHEDULING_HPP

#include <saccade/saccade.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace saccade::platform::windows {

class RuntimeScheduling final {
  public:
    RuntimeScheduling() noexcept = default;
    ~RuntimeScheduling();

    RuntimeScheduling(const RuntimeScheduling&) = delete;
    RuntimeScheduling& operator=(const RuntimeScheduling&) = delete;
    RuntimeScheduling(RuntimeScheduling&&) = delete;
    RuntimeScheduling& operator=(RuntimeScheduling&&) = delete;

    SaccadeResult initialize() noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept { return task_ != nullptr || process_priority_elevated_; }

    [[nodiscard]] bool mmcss_active() const noexcept { return task_ != nullptr; }

    [[nodiscard]] bool process_priority_elevated() const noexcept { return process_priority_elevated_; }

  private:
    HANDLE task_ = nullptr;
    DWORD previous_priority_class_ = 0;
    DWORD owner_thread_id_ = 0;
    bool process_priority_elevated_ = false;
};

} // namespace saccade::platform::windows

#endif
