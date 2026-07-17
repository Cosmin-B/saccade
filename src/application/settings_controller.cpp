#include "application/settings_controller.hpp"

namespace saccade::application {

SaccadeResult SettingsController::fail(SaccadeResult result) noexcept {
    if (result != SACCADE_OK) ++stats_.failures;
    return result;
}

SaccadeResult SettingsController::initialize(SettingsDocument settings, SettingsSink sink) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (sink.apply == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    SaccadeResult result = validate_settings(settings);
    if (result != SACCADE_OK) return result;
    result = sink.apply(sink.context, settings);
    if (result != SACCADE_OK) return result;
    current_ = settings;
    staged_ = settings;
    sink_ = sink;
    revision_ = 1;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult SettingsController::begin_edit() noexcept {
    if (!initialized_ || editing_) return SACCADE_ERROR_STATE;
    staged_ = current_;
    editing_ = true;
    ++stats_.edits_started;
    return SACCADE_OK;
}

SaccadeResult SettingsController::stage(SettingsDocument settings) noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = validate_settings(settings);
    if (result != SACCADE_OK) return fail(result);
    staged_ = settings;
    ++stats_.stages;
    return SACCADE_OK;
}

SaccadeResult SettingsController::reset_page(SettingsPage page) noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    const SaccadeResult result = reset_settings_page(page, &staged_);
    if (result != SACCADE_OK) return fail(result);
    ++stats_.page_resets;
    return SACCADE_OK;
}

SaccadeResult SettingsController::reset_all() noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    staged_ = default_settings();
    ++stats_.full_resets;
    return SACCADE_OK;
}

SaccadeResult SettingsController::import_document(SaccadeSpanU8 encoded) noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    SettingsDocument decoded{};
    const SaccadeResult result = decode_settings(encoded, &decoded);
    if (result != SACCADE_OK) return fail(result);
    staged_ = decoded;
    ++stats_.imports;
    return SACCADE_OK;
}

SaccadeResult SettingsController::export_document(SaccadeMutableSpanU8 output, size_t* output_size) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    const SettingsDocument& source = editing_ ? staged_ : current_;
    const SaccadeResult result = encode_settings(source, output, output_size);
    if (result != SACCADE_OK) return fail(result);
    ++stats_.exports;
    return SACCADE_OK;
}

SaccadeResult SettingsController::commit() noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    const SaccadeResult valid = validate_settings(staged_);
    if (valid != SACCADE_OK) return fail(valid);
    const SaccadeResult applied = sink_.apply(sink_.context, staged_);
    if (applied != SACCADE_OK) return fail(applied);
    current_ = staged_;
    ++revision_;
    if (revision_ == 0) ++revision_;
    editing_ = false;
    ++stats_.commits;
    return SACCADE_OK;
}

SaccadeResult SettingsController::cancel() noexcept {
    if (!initialized_ || !editing_) return SACCADE_ERROR_STATE;
    staged_ = current_;
    editing_ = false;
    ++stats_.cancellations;
    return SACCADE_OK;
}

SaccadeResult SettingsController::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    current_ = {};
    staged_ = {};
    sink_ = {};
    revision_ = 0;
    initialized_ = false;
    editing_ = false;
    return SACCADE_OK;
}

} // namespace saccade::application
