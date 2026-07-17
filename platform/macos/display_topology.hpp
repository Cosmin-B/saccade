#ifndef SACCADE_PLATFORM_MACOS_DISPLAY_TOPOLOGY_HPP
#define SACCADE_PLATFORM_MACOS_DISPLAY_TOPOLOGY_HPP

#include "geometry/display_catalog.hpp"

#include <cstdint>

namespace saccade::platform::macos {

struct DisplayCollectorStats {
    uint64_t refresh_attempts = 0;
    uint64_t topology_changes = 0;
    uint64_t failures = 0;
    uint32_t last_display_count = 0;
    uint32_t reserved = 0;
};

static_assert(sizeof(DisplayCollectorStats) == 32);

class DisplayCollector final {
  public:
    SaccadeResult refresh(geometry::DisplayCatalog*) noexcept;
    SaccadeResult read_stats(DisplayCollectorStats*) const noexcept;

  private:
    DisplayCollectorStats stats_{};
};

} // namespace saccade::platform::macos

#endif
