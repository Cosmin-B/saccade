#ifndef SACCADE_PLATFORM_WINDOWS_ACCESSIBILITY_TARGET_POLICY_HPP
#define SACCADE_PLATFORM_WINDOWS_ACCESSIBILITY_TARGET_POLICY_HPP

#include <windows.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <array>
#include <cstddef>

namespace saccade::platform::windows {

inline constexpr std::array<CONTROLTYPEID, 14> accessibility_target_control_types = {
    UIA_ButtonControlTypeId,      UIA_CheckBoxControlTypeId,  UIA_ComboBoxControlTypeId, UIA_DataItemControlTypeId,
    UIA_EditControlTypeId,        UIA_HyperlinkControlTypeId, UIA_ListItemControlTypeId, UIA_MenuItemControlTypeId,
    UIA_RadioButtonControlTypeId, UIA_SliderControlTypeId,    UIA_SpinnerControlTypeId,  UIA_SplitButtonControlTypeId,
    UIA_TabItemControlTypeId,     UIA_TreeItemControlTypeId,
};

inline constexpr std::array<PROPERTYID, 8> accessibility_target_pattern_properties = {
    UIA_IsExpandCollapsePatternAvailablePropertyId, UIA_IsInvokePatternAvailablePropertyId,
    UIA_IsRangeValuePatternAvailablePropertyId,     UIA_IsScrollItemPatternAvailablePropertyId,
    UIA_IsSelectionItemPatternAvailablePropertyId,  UIA_IsTextPatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId,         UIA_IsValuePatternAvailablePropertyId,
};

template <typename T, size_t Count>
[[nodiscard]] constexpr bool policy_contains(const std::array<T, Count>& values, T value) noexcept {
    for (T candidate : values) {
        if (candidate == value) return true;
    }
    return false;
}

[[nodiscard]] constexpr bool direct_accessibility_target(CONTROLTYPEID type) noexcept {
    return policy_contains(accessibility_target_control_types, type);
}

[[nodiscard]] constexpr bool accessibility_target_pattern(PROPERTYID property) noexcept {
    return policy_contains(accessibility_target_pattern_properties, property);
}

} // namespace saccade::platform::windows

#endif
