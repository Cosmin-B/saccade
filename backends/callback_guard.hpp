#ifndef SACCADE_BACKENDS_CALLBACK_GUARD_HPP
#define SACCADE_BACKENDS_CALLBACK_GUARD_HPP

#include <saccade/saccade.h>

namespace saccade::backend::detail {

template <auto Callback> struct CallbackGuard;

template <typename... Arguments, SaccadeResult(SACCADE_CALL* Callback)(Arguments...)> struct CallbackGuard<Callback> {
    static SaccadeResult SACCADE_CALL invoke(Arguments... arguments) noexcept {
        try {
            return Callback(arguments...);
        } catch (...) {
            return SACCADE_ERROR_BACKEND;
        }
    }
};

template <auto Callback> inline constexpr auto guarded_callback = &CallbackGuard<Callback>::invoke;

} // namespace saccade::backend::detail

#endif
