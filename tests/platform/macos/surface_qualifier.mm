#include "platform/macos/surface_qualifier.hpp"

#include <cstdlib>

namespace {

using namespace saccade::platform::macos;

struct Fixture final {
    SessionEvidence session{};
    uint64_t frontmost_pid = 42;
    AccessibilityEvidence accessibility{};
    bool secure_input = false;
    uint64_t sampled_time = 99;
    uint32_t calls = 0;
    bool session_succeeds = true;
    bool frontmost_succeeds = true;
    bool accessibility_succeeds = true;
    bool secure_input_succeeds = true;
    bool time_succeeds = true;
};

bool session(void* context, SessionEvidence* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    ++fixture->calls;
    if (!fixture->session_succeeds) return false;
    *output = fixture->session;
    return true;
}

bool frontmost(void* context, uint64_t* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    ++fixture->calls;
    if (!fixture->frontmost_succeeds) return false;
    *output = fixture->frontmost_pid;
    return true;
}

bool accessibility(void* context, uint64_t, AccessibilityEvidence* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    ++fixture->calls;
    if (!fixture->accessibility_succeeds) return false;
    *output = fixture->accessibility;
    return true;
}

bool secure_input(void* context, bool* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    ++fixture->calls;
    if (!fixture->secure_input_succeeds) return false;
    *output = fixture->secure_input;
    return true;
}

bool time(void* context, uint64_t* output) noexcept {
    auto* fixture = static_cast<Fixture*>(context);
    ++fixture->calls;
    if (!fixture->time_succeeds) return false;
    *output = fixture->sampled_time;
    return true;
}

void require(bool condition) noexcept {
    if (!condition) std::abort();
}

Fixture qualified_fixture() noexcept {
    Fixture fixture{};
    fixture.session = {true, true, true};
    fixture.accessibility.status = AccessibilitySampleStatus::success;
    fixture.accessibility.focus_pid = 42;
    fixture.accessibility.focus_role = AccessibilityRole::window;
    fixture.accessibility.focus_subrole = AccessibilitySubrole::standard_window;
    return fixture;
}

SurfaceQualifier make_qualifier(Fixture* fixture) noexcept {
    SurfaceQualifierProbe probe{};
    probe.context = fixture;
    probe.sample_session = session;
    probe.sample_frontmost_pid = frontmost;
    probe.sample_accessibility = accessibility;
    probe.sample_secure_input = secure_input;
    probe.sample_time = time;
    return SurfaceQualifier(probe);
}

void test_qualified_and_hot_read() noexcept {
    Fixture fixture = qualified_fixture();
    SurfaceQualifier qualifier = make_qualifier(&fixture);
    require(qualifier.cached().disposition == SurfaceDisposition::unknown);
    require(qualifier.refresh());
    const auto first = qualifier.cached();
    require(first.disposition == SurfaceDisposition::qualified && first.epoch == 1 && first.sampled_time == 99 &&
            first.focus_pid == 42 && first.reason_bits == surface_reason_none);
    const uint32_t calls = fixture.calls;
    require(qualifier.cached().disposition == SurfaceDisposition::qualified);
    require(fixture.calls == calls);
    qualifier.notification_invalidated();
    require(qualifier.cached().disposition == SurfaceDisposition::unknown);
    require(qualifier.refresh());
    require(qualifier.cached().epoch == 2);
}

void test_dialogs_and_blockers() noexcept {
    Fixture fixture = qualified_fixture();
    SurfaceQualifier qualifier = make_qualifier(&fixture);

    fixture.accessibility.focus_role = AccessibilityRole::dialog;
    fixture.accessibility.focus_subrole = AccessibilitySubrole::dialog;
    require(qualifier.refresh());

    fixture.accessibility.focus_role = AccessibilityRole::sheet;
    fixture.accessibility.focus_subrole = AccessibilitySubrole::sheet;
    require(qualifier.refresh());

    fixture.accessibility.focus_role = AccessibilityRole::system_dialog;
    fixture.accessibility.focus_subrole = AccessibilitySubrole::system_dialog;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_system_dialog) != 0);

    fixture.accessibility.focus_role = AccessibilityRole::window;
    fixture.accessibility.focus_subrole = AccessibilitySubrole::standard_window;
    fixture.secure_input = true;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_secure_input) != 0);

    fixture.secure_input = false;
    fixture.accessibility.secure_text_present = true;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_secure_input) != 0);
}

void test_fail_closed_matrix() noexcept {
    Fixture fixture = qualified_fixture();
    SurfaceQualifier qualifier = make_qualifier(&fixture);

    fixture.session.type_valid = false;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_wrong_type) != 0);
    fixture.session = {true, true, true};

    fixture.session.on_console = false;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_session_missing) != 0);
    fixture.session.on_console = true;

    fixture.session.login_done = false;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_session_missing) != 0);
    fixture.session.login_done = true;

    fixture.frontmost_pid = 0;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_frontmost_missing) != 0);
    fixture.frontmost_pid = 42;

    fixture.accessibility.status = AccessibilitySampleStatus::untrusted;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_ax_untrusted) != 0);
    fixture.accessibility.status = AccessibilitySampleStatus::failure;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_ax_failure) != 0);
    fixture.accessibility.status = AccessibilitySampleStatus::wrong_type;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_wrong_type) != 0);
    fixture.accessibility.status = AccessibilitySampleStatus::missing;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_missing_evidence) != 0);

    fixture.accessibility = qualified_fixture().accessibility;
    fixture.accessibility.focus_pid = 7;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_pid_mismatch) != 0);

    fixture.accessibility = qualified_fixture().accessibility;
    fixture.accessibility.ancestor_limit_reached = true;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_preflight_limit) != 0);

    fixture = qualified_fixture();
    fixture.accessibility_succeeds = false;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_probe_failure) != 0);

    fixture = qualified_fixture();
    fixture.time_succeeds = false;
    require(!qualifier.refresh());
    require((qualifier.cached().reason_bits & surface_reason_probe_failure) != 0);
}

void test_preflight_bounds() noexcept {
    AccessibilityEvidence evidence{};
    evidence.status = AccessibilitySampleStatus::success;
    evidence.focus_pid = 42;
    evidence.focus_role = AccessibilityRole::window;
    evidence.focus_subrole = AccessibilitySubrole::standard_window;
    require(SurfaceQualifier::classify_preflight(evidence) == surface_reason_none);
    evidence.action_point_limit_reached = true;
    require((SurfaceQualifier::classify_preflight(evidence) & surface_reason_preflight_limit) != 0);
    evidence.action_point_limit_reached = false;
    evidence.action_point_count = 1;
    evidence.action_points[0].protected_content = true;
    require((SurfaceQualifier::classify_preflight(evidence) & surface_reason_secure_input) != 0);
    evidence.action_points[0].protected_content = false;
    evidence.ancestor_count = 16;
    evidence.ancestors[15] = {AccessibilityRole::system_floating_window, AccessibilitySubrole::system_floating_window};
    require((SurfaceQualifier::classify_preflight(evidence) & surface_reason_system_floating_window) != 0);
}

} // namespace

int main() {
    test_qualified_and_hot_read();
    test_dialogs_and_blockers();
    test_fail_closed_matrix();
    test_preflight_bounds();
    return 0;
}
