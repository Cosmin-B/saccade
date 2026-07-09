#ifndef SACCADE_CORE_ABI_GUARD_HPP
#define SACCADE_CORE_ABI_GUARD_HPP

#include "core/error.hpp"

#include <exception>
#include <utility>

namespace saccade::core {

template <typename Function>
SaccadeResult abi_guard(Function&& function) noexcept {
    ErrorScope scope;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        return std::forward<Function>(function)();
    } catch (const std::exception& exception) {
        set_last_error(exception.what());
        return SACCADE_ERROR_BACKEND;
    } catch (...) {
        set_last_error("unexpected C++ exception");
        return SACCADE_ERROR_BACKEND;
    }
#else
    return std::forward<Function>(function)();
#endif
}

}  // namespace saccade::core

#endif
