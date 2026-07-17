#pragma once

#include <cstdint>
#include <pthread.h>

namespace saccade::platform::macos {

enum class SurfaceDisposition : uint8_t { unknown = 0, blocked = 1, qualified = 2 };
enum class ActionPointDisposition : uint8_t { unavailable = 0, secure = 1, qualified = 2 };

enum SurfaceQualifierReason : uint32_t {
    surface_reason_none = 0,
    surface_reason_session_missing = UINT32_C(1) << 0,
    surface_reason_frontmost_missing = UINT32_C(1) << 1,
    surface_reason_ax_untrusted = UINT32_C(1) << 2,
    surface_reason_ax_failure = UINT32_C(1) << 3,
    surface_reason_pid_mismatch = UINT32_C(1) << 4,
    surface_reason_secure_input = UINT32_C(1) << 5,
    surface_reason_system_dialog = UINT32_C(1) << 6,
    surface_reason_system_floating_window = UINT32_C(1) << 7,
    surface_reason_missing_evidence = UINT32_C(1) << 8,
    surface_reason_wrong_type = UINT32_C(1) << 9,
    surface_reason_probe_failure = UINT32_C(1) << 10,
    surface_reason_preflight_limit = UINT32_C(1) << 11,
    surface_reason_owner_thread = UINT32_C(1) << 12
};

struct SurfaceQualifierSnapshot final {
    uint64_t epoch = 0;
    uint64_t sampled_time = 0;
    SurfaceDisposition disposition = SurfaceDisposition::unknown;
    uint32_t reason_bits = surface_reason_none;
    uint64_t focus_pid = 0;
};

enum class AccessibilitySampleStatus : uint8_t { success = 0, untrusted = 1, failure = 2, missing = 3, wrong_type = 4 };

enum class AccessibilityRole : uint8_t {
    unknown = 0,
    window = 1,
    dialog = 2,
    sheet = 3,
    system_dialog = 4,
    system_floating_window = 5,
    text_field = 6,
    secure_text_field = 7,
    button = 8,
    other = 9
};

enum class AccessibilitySubrole : uint8_t {
    unknown = 0,
    standard_window = 1,
    dialog = 2,
    sheet = 3,
    system_dialog = 4,
    floating_window = 5,
    system_floating_window = 6,
    secure_text_field = 7,
    other = 8
};

struct AccessibilityActionPoint final {
    AccessibilityRole role = AccessibilityRole::unknown;
    AccessibilitySubrole subrole = AccessibilitySubrole::unknown;
    bool protected_content = false;
};

struct AccessibilityEvidence final {
    AccessibilitySampleStatus status = AccessibilitySampleStatus::missing;
    uint64_t focus_pid = 0;
    AccessibilityRole focus_role = AccessibilityRole::unknown;
    AccessibilitySubrole focus_subrole = AccessibilitySubrole::unknown;
    uint32_t ancestor_count = 0;
    AccessibilityActionPoint ancestors[16]{};
    uint32_t action_point_count = 0;
    AccessibilityActionPoint action_points[64]{};
    bool ancestor_limit_reached = false;
    bool action_point_limit_reached = false;
    bool secure_text_present = false;
};

struct SessionEvidence final {
    bool type_valid = false;
    bool on_console = false;
    bool login_done = false;
};

using SampleSession = bool (*)(void* context, SessionEvidence* output) noexcept;
using SampleFrontmostPid = bool (*)(void* context, uint64_t* output) noexcept;
using SampleAccessibility = bool (*)(void* context, uint64_t frontmost_pid, AccessibilityEvidence* output) noexcept;
using SampleSecureInput = bool (*)(void* context, bool* output) noexcept;
using SampleTime = bool (*)(void* context, uint64_t* output) noexcept;

struct SurfaceQualifierProbe final {
    void* context = nullptr;
    SampleSession sample_session = nullptr;
    SampleFrontmostPid sample_frontmost_pid = nullptr;
    SampleAccessibility sample_accessibility = nullptr;
    SampleSecureInput sample_secure_input = nullptr;
    SampleTime sample_time = nullptr;
};

class SurfaceQualifier final {
  public:
    explicit SurfaceQualifier(const SurfaceQualifierProbe& probe = {}) noexcept;
    ~SurfaceQualifier() = default;

    SurfaceQualifier(const SurfaceQualifier&) = delete;
    SurfaceQualifier& operator=(const SurfaceQualifier&) = delete;
    SurfaceQualifier(SurfaceQualifier&&) = delete;
    SurfaceQualifier& operator=(SurfaceQualifier&&) = delete;

    [[nodiscard]] const SurfaceQualifierSnapshot& cached() const noexcept;
    [[nodiscard]] bool refresh() noexcept;
    void invalidate() noexcept;
    void notification_invalidated() noexcept;
    [[nodiscard]] bool on_owner_thread() const noexcept;

    [[nodiscard]] static uint32_t classify_preflight(const AccessibilityEvidence& evidence) noexcept;

  private:
    static pthread_t current_thread() noexcept;
    bool check_owner() const noexcept;

    SurfaceQualifierProbe probe_{};
    pthread_t owner_thread_{};
    SurfaceQualifierSnapshot snapshot_{};
};

ActionPointDisposition qualify_action_point(int32_t x_q8, int32_t y_q8) noexcept;

static_assert(sizeof(SurfaceQualifierSnapshot) == 32);
static_assert(sizeof(AccessibilityEvidence) <= 1024);

} // namespace saccade::platform::macos
