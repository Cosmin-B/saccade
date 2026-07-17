#include "platform/macos/global_hotkeys.hpp"

#include "platform/macos/keyboard.hpp"

namespace saccade::platform::macos {
namespace {

constexpr OSType hotkey_signature = 'SCCD';
constexpr uint32_t first_registration_id = 1;
constexpr double nanoseconds_per_second = 1'000'000'000.0;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;
constexpr uint32_t binding_flag_mask = application::hotkey_always_active | application::hotkey_session_only;

bool command_valid(application::Command command) noexcept {
    return command >= application::Command::pointer_move && command <= application::last_command;
}

uint32_t carbon_modifiers(uint32_t modifiers) noexcept {
    uint32_t result = 0;
    if ((modifiers & SACCADE_INPUT_MODIFIER_SHIFT) != 0) result |= shiftKey;
    if ((modifiers & SACCADE_INPUT_MODIFIER_CONTROL) != 0) result |= controlKey;
    if ((modifiers & SACCADE_INPUT_MODIFIER_ALT) != 0) result |= optionKey;
    if ((modifiers & SACCADE_INPUT_MODIFIER_META) != 0) result |= cmdKey;
    return result;
}

bool bindings_valid(const application::HotkeyBinding* bindings, uint32_t count) noexcept {
    if ((bindings == nullptr) != (count == 0) || count > application::maximum_hotkey_bindings) return false;
    for (uint32_t index = 0; index < count; ++index) {
        const application::HotkeyBinding& binding = bindings[index];
        CGKeyCode keycode = 0;
        if (!command_valid(binding.command) || binding.physical_key == 0 ||
            !keycode_from_hid_usage(binding.physical_key, &keycode) || (binding.modifiers & ~modifier_mask) != 0 ||
            (binding.flags & ~binding_flag_mask) != 0 ||
            (binding.flags & (application::hotkey_always_active | application::hotkey_session_only)) ==
                (application::hotkey_always_active | application::hotkey_session_only))
            return false;
        for (uint32_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].physical_key == binding.physical_key &&
                bindings[previous].modifiers == binding.modifiers)
                return false;
        }
    }
    return true;
}

OSStatus hotkey_event(EventHandlerCallRef, EventRef event, void* context) noexcept {
    EventHotKeyID identifier{};
    const OSStatus read = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr,
                                            sizeof(identifier), nullptr, &identifier);
    if (read != noErr || identifier.signature != hotkey_signature) return eventNotHandledErr;
    auto* hotkeys = static_cast<GlobalHotkeys*>(context);
    const uint64_t timestamp_ns = static_cast<uint64_t>(GetEventTime(event) * nanoseconds_per_second);
    return hotkeys->dispatch_registered_id(identifier.id, timestamp_ns) == SACCADE_OK
               ? static_cast<OSStatus>(noErr)
               : static_cast<OSStatus>(eventNotHandledErr);
}

} // namespace

GlobalHotkeys::~GlobalHotkeys() {
    if (initialized_ && owns_thread()) (void)shutdown();
}

bool GlobalHotkeys::owns_thread() const noexcept {
    return initialized_ && pthread_equal(owner_, pthread_self()) != 0;
}

SaccadeResult GlobalHotkeys::register_binding(uint32_t index) noexcept {
    BindingSlot& slot = bindings_[index];
    if (slot.native_ != nullptr || (slot.binding_.flags & application::hotkey_session_only) != 0) return SACCADE_OK;

    CGKeyCode keycode = 0;
    (void)keycode_from_hid_usage(slot.binding_.physical_key, &keycode);
    const EventHotKeyID identifier{hotkey_signature, first_registration_id + index};
    EventHotKeyRef native = nullptr;
    const OSStatus registered = RegisterEventHotKey(keycode, carbon_modifiers(slot.binding_.modifiers), identifier,
                                                    GetApplicationEventTarget(), 0, &native);
    if (registered != noErr || native == nullptr) {
        ++stats_.failures;
        return registered == eventHotKeyExistsErr ? SACCADE_ERROR_ALREADY_EXISTS : SACCADE_ERROR_BACKEND;
    }

    slot.native_ = native;
    ++stats_.registrations;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::unregister_binding(uint32_t index) noexcept {
    BindingSlot& slot = bindings_[index];
    if (slot.native_ == nullptr) return SACCADE_OK;
    if (UnregisterEventHotKey(slot.native_) != noErr) {
        ++stats_.failures;
        return SACCADE_ERROR_BACKEND;
    }
    slot.native_ = nullptr;
    ++stats_.unregisters;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::initialize(application::CommandSink sink) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (sink.command == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    const EventTypeSpec type{kEventClassKeyboard, kEventHotKeyPressed};
    EventHandlerRef handler = nullptr;
    const OSStatus installed = InstallEventHandler(GetApplicationEventTarget(), hotkey_event, 1, &type, this, &handler);
    if (installed != noErr || handler == nullptr) return SACCADE_ERROR_BACKEND;
    sink_ = sink;
    handler_ = handler;
    owner_ = pthread_self();
    initialized_ = true;
    return SACCADE_OK;
}

void GlobalHotkeys::unregister_all() noexcept {
    for (uint32_t index = 0; index < binding_count_; ++index) {
        (void)unregister_binding(index);
        bindings_[index] = {};
    }
    binding_count_ = 0;
}

SaccadeResult GlobalHotkeys::replace(const application::HotkeyBinding* bindings, uint32_t count) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (!bindings_valid(bindings, count)) return SACCADE_ERROR_INVALID_ARGUMENT;
    unregister_all();
    for (uint32_t index = 0; index < count; ++index) {
        bindings_[index].binding_ = bindings[index];
        binding_count_ = index + 1U;
        if ((bindings[index].flags & application::hotkey_session_only) != 0) continue;
        if (suspended_ && (bindings[index].flags & application::hotkey_always_active) == 0) continue;

        const SaccadeResult registered = register_binding(index);
        if (registered != SACCADE_OK) {
            unregister_all();
            return registered;
        }
    }
    ++stats_.replacements;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::set_suspended(bool value) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    if (suspended_ == value) return SACCADE_OK;

    if (value) {
        uint32_t index = 0;
        for (; index < binding_count_; ++index) {
            if ((bindings_[index].binding_.flags &
                 (application::hotkey_always_active | application::hotkey_session_only)) != 0)
                continue;
            if (unregister_binding(index) == SACCADE_OK) continue;

            for (uint32_t restore = 0; restore < index; ++restore) {
                if ((bindings_[restore].binding_.flags &
                     (application::hotkey_always_active | application::hotkey_session_only)) == 0)
                    (void)register_binding(restore);
            }
            return SACCADE_ERROR_BACKEND;
        }
    } else {
        uint32_t index = 0;
        for (; index < binding_count_; ++index) {
            if ((bindings_[index].binding_.flags &
                 (application::hotkey_always_active | application::hotkey_session_only)) != 0)
                continue;
            const SaccadeResult registered = register_binding(index);
            if (registered == SACCADE_OK) continue;

            for (uint32_t rollback = 0; rollback < index; ++rollback) {
                if ((bindings_[rollback].binding_.flags &
                     (application::hotkey_always_active | application::hotkey_session_only)) == 0)
                    (void)unregister_binding(rollback);
            }
            return registered;
        }
    }

    suspended_ = value;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::dispatch_registered_id(uint32_t registration_id, uint64_t timestamp_ns) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    ++stats_.events;
    if (registration_id < first_registration_id || registration_id - first_registration_id >= binding_count_) {
        ++stats_.unknown;
        return SACCADE_ERROR_NOT_FOUND;
    }
    const BindingSlot& slot = bindings_[registration_id - first_registration_id];
    const application::HotkeyBinding& binding = slot.binding_;
    if (slot.native_ == nullptr || (binding.flags & application::hotkey_session_only) != 0) {
        ++stats_.unknown;
        return SACCADE_ERROR_NOT_FOUND;
    }
    if (suspended_ && (binding.flags & application::hotkey_always_active) == 0) {
        ++stats_.suspended;
        return SACCADE_ERROR_NOT_FOUND;
    }
    if (sink_.command_observed != nullptr) sink_.command_observed(sink_.context, timestamp_ns);
    sink_.command(sink_.context,
                  {timestamp_ns, binding.command, binding.physical_key, binding.modifiers, binding.flags});
    ++stats_.dispatched;
    return SACCADE_OK;
}

SaccadeResult GlobalHotkeys::dispatch_physical(uint32_t physical_key, uint32_t modifiers,
                                               uint64_t timestamp_ns) noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    ++stats_.events;
    for (uint32_t index = 0; index < binding_count_; ++index) {
        const application::HotkeyBinding& binding = bindings_[index].binding_;
        if ((binding.flags & application::hotkey_session_only) != 0) continue;
        if (binding.physical_key != physical_key || binding.modifiers != modifiers) continue;
        if (suspended_ && (binding.flags & application::hotkey_always_active) == 0) {
            ++stats_.suspended;
            return SACCADE_ERROR_NOT_FOUND;
        }
        if (sink_.command_observed != nullptr) sink_.command_observed(sink_.context, timestamp_ns);
        sink_.command(sink_.context,
                      {timestamp_ns, binding.command, binding.physical_key, binding.modifiers, binding.flags});
        ++stats_.dispatched;
        return SACCADE_OK;
    }
    ++stats_.unknown;
    return SACCADE_ERROR_NOT_FOUND;
}

SaccadeResult GlobalHotkeys::shutdown() noexcept {
    if (!initialized_ || !owns_thread()) return SACCADE_ERROR_STATE;
    unregister_all();
    const OSStatus removed = RemoveEventHandler(handler_);
    handler_ = nullptr;
    sink_ = {};
    initialized_ = false;
    suspended_ = false;
    return removed == noErr ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

} // namespace saccade::platform::macos
