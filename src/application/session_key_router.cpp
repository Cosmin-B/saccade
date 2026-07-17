#include "application/session_key_router.hpp"

namespace saccade::application {
namespace {

constexpr uint32_t symbol_modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT;
constexpr uint32_t modifier_mask = SACCADE_INPUT_MODIFIER_SHIFT | SACCADE_INPUT_MODIFIER_CONTROL |
                                   SACCADE_INPUT_MODIFIER_ALT | SACCADE_INPUT_MODIFIER_META;

bool session_binding_valid(const HotkeyBinding& binding) noexcept {
    return binding.command >= Command::pointer_move && binding.command <= last_command && binding.physical_key != 0 &&
           (binding.modifiers & ~modifier_mask) == 0 && (binding.flags & hotkey_session_only) != 0 &&
           (binding.flags & ~(hotkey_always_active | hotkey_session_only)) == 0 &&
           (binding.flags & hotkey_always_active) == 0;
}

} // namespace

SaccadeResult SessionKeyRouter::initialize(DesktopRuntime* runtime, SessionCommandSink sink) noexcept {
    if (initialized_) return SACCADE_ERROR_ALREADY_EXISTS;
    if (runtime == nullptr || sink.command == nullptr) return SACCADE_ERROR_INVALID_ARGUMENT;
    runtime_ = runtime;
    sink_ = sink;
    initialized_ = true;
    return SACCADE_OK;
}

SaccadeResult SessionKeyRouter::replace(const HotkeyBinding* bindings, uint32_t count) noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    if ((bindings == nullptr) != (count == 0) || count > maximum_hotkey_bindings) return SACCADE_ERROR_INVALID_ARGUMENT;

    for (uint32_t index = 0; index < count; ++index) {
        if ((bindings[index].flags & hotkey_session_only) == 0) continue;
        if (!session_binding_valid(bindings[index])) return SACCADE_ERROR_INVALID_ARGUMENT;
        for (uint32_t previous = 0; previous < index; ++previous) {
            if ((bindings[previous].flags & hotkey_session_only) != 0 &&
                bindings[previous].physical_key == bindings[index].physical_key &&
                bindings[previous].modifiers == bindings[index].modifiers) {
                return SACCADE_ERROR_ALREADY_EXISTS;
            }
        }
    }

    bindings_.fill({});
    binding_count_ = 0;
    for (uint32_t index = 0; index < count; ++index) {
        if ((bindings[index].flags & hotkey_session_only) != 0) bindings_[binding_count_++] = bindings[index];
    }
    return SACCADE_OK;
}

SaccadeResult SessionKeyRouter::route(const KeyEvent& input, SessionKeyRoute* output) noexcept {
    if (!initialized_ || output == nullptr || input.timestamp_ns == 0 || input.physical_key == 0 ||
        input.reserved != 0) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *output = {};
    ++stats_.keys;
    if (!runtime_->active()) {
        ++stats_.passed_through;
        return SACCADE_OK;
    }
    const bool was_active = true;
    for (uint32_t index = 0; index < binding_count_; ++index) {
        const HotkeyBinding& binding = bindings_[index];
        if (binding.physical_key != input.physical_key || binding.modifiers != input.modifiers) continue;
        output->result = sink_.command(sink_.context, binding.command, input.timestamp_ns);
        output->handled = true;
        ++stats_.controls;
        break;
    }
    if (!output->handled && (input.modifiers & ~symbol_modifier_mask) == 0) {
        const uint16_t physical_symbol = runtime_->hint_symbol_for_physical_key(input.physical_key);
        const uint16_t symbol = physical_symbol != 0 ? physical_symbol : input.logical_symbol;
        if (symbol == 0) {
            ++stats_.passed_through;
            return SACCADE_OK;
        }
        SessionEvent event{};
        output->result = runtime_->enter_symbol(symbol, input.timestamp_ns, &event);
        output->handled = true;
        ++stats_.symbols;
        if (physical_symbol != 0)
            ++stats_.physical_symbols;
        else
            ++stats_.logical_symbols;
    } else if (!output->handled) {
        ++stats_.passed_through;
        return SACCADE_OK;
    }
    if (output->result != SACCADE_OK) ++stats_.rejected;
    output->session_ended = was_active && !runtime_->active();
    return SACCADE_OK;
}

SaccadeResult SessionKeyRouter::shutdown() noexcept {
    if (!initialized_) return SACCADE_ERROR_STATE;
    bindings_.fill({});
    runtime_ = nullptr;
    sink_ = {};
    binding_count_ = 0;
    initialized_ = false;
    return SACCADE_OK;
}

bool route_session_key(void* context, const KeyEvent& input) noexcept {
    auto* router = static_cast<SessionKeyRouter*>(context);
    if (router == nullptr) return false;
    SessionKeyRoute result{};
    return router->route(input, &result) == SACCADE_OK && result.handled;
}

} // namespace saccade::application
