#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "platform/windows/accessibility_target_policy.hpp"

#include <array>
#include <cstddef>

namespace {

using namespace saccade::platform::windows;

enum class TestResult : int {
    success,
    direct_type_missing,
    generic_type_admitted,
    pattern_missing,
    unrelated_property_admitted,
    duplicate_direct_type,
    duplicate_pattern
};

template <typename T, size_t Count> bool unique(const std::array<T, Count>& values) noexcept {
    for (size_t left = 0; left < Count; ++left) {
        for (size_t right = left + 1; right < Count; ++right) {
            if (values[left] == values[right]) return false;
        }
    }
    return true;
}

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

} // namespace

int main() {
    for (CONTROLTYPEID type : accessibility_target_control_types) {
        if (!direct_accessibility_target(type)) return result(TestResult::direct_type_missing);
    }

    constexpr std::array generic_types = {
        UIA_CustomControlTypeId,   UIA_DocumentControlTypeId,  UIA_GroupControlTypeId,  UIA_ImageControlTypeId,
        UIA_PaneControlTypeId,     UIA_SeparatorControlTypeId, UIA_TextControlTypeId,   UIA_ThumbControlTypeId,
        UIA_TitleBarControlTypeId, UIA_ToolBarControlTypeId,   UIA_WindowControlTypeId,
    };
    for (CONTROLTYPEID type : generic_types) {
        if (direct_accessibility_target(type)) return result(TestResult::generic_type_admitted);
    }

    for (PROPERTYID property : accessibility_target_pattern_properties) {
        if (!accessibility_target_pattern(property)) return result(TestResult::pattern_missing);
    }

    constexpr std::array unrelated_properties = {
        UIA_BoundingRectanglePropertyId, UIA_ControlTypePropertyId, UIA_IsEnabledPropertyId,
        UIA_IsOffscreenPropertyId,       UIA_NamePropertyId,
    };
    for (PROPERTYID property : unrelated_properties) {
        if (accessibility_target_pattern(property)) return result(TestResult::unrelated_property_admitted);
    }

    if (!unique(accessibility_target_control_types)) return result(TestResult::duplicate_direct_type);
    if (!unique(accessibility_target_pattern_properties)) return result(TestResult::duplicate_pattern);
    return result(TestResult::success);
}
