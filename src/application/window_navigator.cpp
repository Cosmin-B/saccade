#include "application/window_navigator.hpp"

#include <cstdint>

namespace saccade::application {
namespace {

bool valid(const SaccadeWindowInfo& window) noexcept {
    return window.stable_id != 0 && window.process_id != 0 && window.desktop_bounds.width > 0 &&
           window.desktop_bounds.height > 0 && window.reserved0 == 0;
}

int64_t center_x(const SaccadeWindowInfo& window) noexcept {
    return static_cast<int64_t>(window.desktop_bounds.x) * 2 + window.desktop_bounds.width;
}

int64_t center_y(const SaccadeWindowInfo& window) noexcept {
    return static_cast<int64_t>(window.desktop_bounds.y) * 2 + window.desktop_bounds.height;
}

bool overlaps(const SaccadeWindowInfo& left, const SaccadeWindowInfo& right) noexcept {
    return static_cast<int64_t>(left.desktop_bounds.x) + left.desktop_bounds.width > right.desktop_bounds.x &&
           static_cast<int64_t>(right.desktop_bounds.x) + right.desktop_bounds.width > left.desktop_bounds.x &&
           static_cast<int64_t>(left.desktop_bounds.y) + left.desktop_bounds.height > right.desktop_bounds.y &&
           static_cast<int64_t>(right.desktop_bounds.y) + right.desktop_bounds.height > left.desktop_bounds.y;
}

} // namespace

SaccadeResult WindowNavigator::collect(SaccadeAccessibilityProviderDesc provider, uint64_t excluded_process_id,
                                       WindowSnapshot* output) noexcept {
    if (output == nullptr || provider.context == nullptr || provider.ops.enumerate_windows == nullptr)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    *output = {};
    for (uint32_t index = 0; index < window_navigation_capacity; ++index) {
        SaccadeWindowInfo window{};
        window.struct_size = sizeof(window);
        window.api_version = SACCADE_API_VERSION;
        const SaccadeResult result = provider.ops.enumerate_windows(provider.context, index, &window);
        if (result == SACCADE_ERROR_NOT_FOUND) return output->count == 0 ? SACCADE_ERROR_NOT_FOUND : SACCADE_OK;
        if (result != SACCADE_OK) return result;
        if (!valid(window) || window.process_id == excluded_process_id) continue;
        output->windows[output->count++] = window;
    }
    SaccadeWindowInfo extra{};
    extra.struct_size = sizeof(extra);
    extra.api_version = SACCADE_API_VERSION;
    return provider.ops.enumerate_windows(provider.context, window_navigation_capacity, &extra) ==
                   SACCADE_ERROR_NOT_FOUND
               ? SACCADE_OK
               : SACCADE_ERROR_CAPACITY;
}

SaccadeResult WindowNavigator::cycle(const WindowSnapshot& snapshot, uint64_t current_window_id, bool forward,
                                     uint64_t* output) const noexcept {
    if (output == nullptr || snapshot.count == 0 || snapshot.count > snapshot.windows.size())
        return SACCADE_ERROR_INVALID_ARGUMENT;
    uint32_t current = snapshot.count;
    for (uint32_t index = 0; index < snapshot.count; ++index)
        if (snapshot.windows[index].stable_id == current_window_id) {
            current = index;
            break;
        }
    if (current == snapshot.count) {
        *output = snapshot.windows[0].stable_id;
        return SACCADE_OK;
    }
    for (uint32_t distance = 1; distance < snapshot.count; ++distance) {
        const uint32_t candidate =
            forward ? (current + distance) % snapshot.count : (current + snapshot.count - distance) % snapshot.count;
        if (!overlaps(snapshot.windows[current], snapshot.windows[candidate])) continue;
        *output = snapshot.windows[candidate].stable_id;
        return SACCADE_OK;
    }
    return SACCADE_ERROR_NOT_FOUND;
}

SaccadeResult WindowNavigator::behind(const WindowSnapshot& snapshot, uint64_t current_window_id,
                                      uint64_t* output) const noexcept {
    return cycle(snapshot, current_window_id, true, output);
}

SaccadeResult WindowNavigator::directional(const WindowSnapshot& snapshot, uint64_t current_window_id,
                                           WindowDirection direction, uint64_t* output) const noexcept {
    if (output == nullptr || snapshot.count == 0 || snapshot.count > snapshot.windows.size() ||
        direction < WindowDirection::left || direction > WindowDirection::down)
        return SACCADE_ERROR_INVALID_ARGUMENT;
    const SaccadeWindowInfo* current = nullptr;
    for (uint32_t index = 0; index < snapshot.count; ++index)
        if (snapshot.windows[index].stable_id == current_window_id) {
            current = &snapshot.windows[index];
            break;
        }
    if (current == nullptr) return SACCADE_ERROR_NOT_FOUND;
    const int64_t origin_x = center_x(*current);
    const int64_t origin_y = center_y(*current);
    const SaccadeWindowInfo* best = nullptr;
    uint64_t best_primary = UINT64_MAX;
    uint64_t best_secondary = UINT64_MAX;
    for (uint32_t index = 0; index < snapshot.count; ++index) {
        const SaccadeWindowInfo& candidate = snapshot.windows[index];
        if (candidate.stable_id == current_window_id) continue;
        const int64_t dx = center_x(candidate) - origin_x;
        const int64_t dy = center_y(candidate) - origin_y;
        const bool eligible = direction == WindowDirection::left    ? dx < 0
                              : direction == WindowDirection::right ? dx > 0
                              : direction == WindowDirection::up    ? dy < 0
                                                                    : dy > 0;
        if (!eligible) continue;
        const uint64_t primary = static_cast<uint64_t>(
            direction == WindowDirection::left || direction == WindowDirection::right ? dx < 0 ? -dx : dx
            : dy < 0                                                                  ? -dy
                                                                                      : dy);
        const uint64_t secondary = static_cast<uint64_t>(
            direction == WindowDirection::left || direction == WindowDirection::right ? dy < 0 ? -dy : dy
            : dx < 0                                                                  ? -dx
                                                                                      : dx);
        if (best == nullptr || primary < best_primary ||
            (primary == best_primary &&
             (secondary < best_secondary || (secondary == best_secondary && candidate.stable_id < best->stable_id)))) {
            best = &candidate;
            best_primary = primary;
            best_secondary = secondary;
        }
    }
    if (best == nullptr) return SACCADE_ERROR_NOT_FOUND;
    *output = best->stable_id;
    return SACCADE_OK;
}

} // namespace saccade::application
