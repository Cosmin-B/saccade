#ifndef SACCADE_PLATFORM_WINDOWS_ACTION_POINT_QUALIFIER_HPP
#define SACCADE_PLATFORM_WINDOWS_ACTION_POINT_QUALIFIER_HPP

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <saccade/saccade.h>

#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <cstdint>

namespace saccade::platform::windows {

enum class ActionPointDisposition : uint8_t { unavailable = 0, secure = 1, qualified = 2 };

class ActionPointQualifier final {
  public:
    SaccadeResult initialize() noexcept;
    SaccadeResult shutdown() noexcept;
    ActionPointDisposition qualify(int32_t x_q8, int32_t y_q8, uint64_t window_id) const noexcept;
    ActionPointDisposition qualify_focus(uint64_t window_id) const noexcept;

  private:
    Microsoft::WRL::ComPtr<IUIAutomation> automation_{};
    DWORD owner_thread_id_ = 0;
    bool co_initialized_ = false;
    bool initialized_ = false;
};

} // namespace saccade::platform::windows

#endif
