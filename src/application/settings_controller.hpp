#ifndef SACCADE_APPLICATION_SETTINGS_CONTROLLER_HPP
#define SACCADE_APPLICATION_SETTINGS_CONTROLLER_HPP

#include "application/settings.hpp"

#include <cstdint>

namespace saccade::application {

using ApplySettingsFn = SaccadeResult (*)(void*, const SettingsDocument&) noexcept;

struct SettingsSink {
    void* context = nullptr;
    ApplySettingsFn apply = nullptr;
};

struct SettingsControllerStats {
    uint64_t edits_started = 0;
    uint64_t stages = 0;
    uint64_t commits = 0;
    uint64_t cancellations = 0;
    uint64_t page_resets = 0;
    uint64_t full_resets = 0;
    uint64_t imports = 0;
    uint64_t exports = 0;
    uint64_t failures = 0;
};

class SettingsController final {
  public:
    SaccadeResult initialize(SettingsDocument, SettingsSink) noexcept;
    SaccadeResult begin_edit() noexcept;
    SaccadeResult stage(SettingsDocument) noexcept;
    SaccadeResult reset_page(SettingsPage) noexcept;
    SaccadeResult reset_all() noexcept;
    SaccadeResult import_document(SaccadeSpanU8) noexcept;
    SaccadeResult export_document(SaccadeMutableSpanU8, size_t*) noexcept;
    SaccadeResult commit() noexcept;
    SaccadeResult cancel() noexcept;
    SaccadeResult shutdown() noexcept;

    [[nodiscard]] const SettingsDocument& current() const noexcept { return current_; }

    [[nodiscard]] const SettingsDocument& staged() const noexcept { return staged_; }

    [[nodiscard]] bool editing() const noexcept { return editing_; }

    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] SettingsControllerStats stats() const noexcept { return stats_; }

  private:
    SaccadeResult fail(SaccadeResult) noexcept;

    SettingsDocument current_{};
    SettingsDocument staged_{};
    SettingsSink sink_{};
    SettingsControllerStats stats_{};
    uint64_t revision_ = 0;
    bool initialized_ = false;
    bool editing_ = false;
};

static_assert(sizeof(SettingsControllerStats) == 72);

} // namespace saccade::application

#endif
