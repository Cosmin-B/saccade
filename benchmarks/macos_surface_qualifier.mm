#include "core/stack_string_builder.hpp"
#include "platform/macos/surface_qualifier.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <time.h>
#include <unistd.h>

namespace {

using saccade::core::StackStringBuilder;
using saccade::platform::macos::SurfaceDisposition;
using saccade::platform::macos::SurfaceQualifier;

constexpr uint32_t default_runs = 64;
constexpr uint32_t maximum_runs = 1024;

enum class ExitCode : int { success, usage, invalid_sample, qualification };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

bool parse_runs(const char* text, uint32_t* output) noexcept {
    if (text == nullptr || output == nullptr) return false;
    const char* end = text;
    while (*end != '\0')
        ++end;
    uint32_t value = 0;
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0 || value > maximum_runs) return false;
    *output = value;
    return true;
}

uint64_t monotonic_ns() noexcept {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t percentile(std::array<uint64_t, maximum_runs>* samples, uint32_t count, uint32_t numerator) noexcept {
    std::sort(samples->begin(), samples->begin() + count);
    const uint32_t index = std::min(count - 1U, ((count - 1U) * numerator + 50U) / 100U);
    return samples->at(index);
}

void emit(std::string_view text) noexcept {
    (void)write(STDOUT_FILENO, text.data(), text.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) return exit_code(ExitCode::usage);
    uint32_t runs = default_runs;
    if (argc == 2 && !parse_runs(argv[1], &runs)) return exit_code(ExitCode::usage);

    SurfaceQualifier qualifier;
    std::array<uint64_t, maximum_runs> samples{};
    uint32_t qualified_count = 0;
    uint32_t blocked_count = 0;
    uint32_t combined_reasons = 0;
    for (uint32_t index = 0; index < runs; ++index) {
        const uint64_t started_ns = monotonic_ns();
        const bool qualified = qualifier.refresh();
        const uint64_t completed_ns = monotonic_ns();
        const auto& snapshot = qualifier.cached();
        if (completed_ns < started_ns || snapshot.epoch != static_cast<uint64_t>(index) + 1U ||
            snapshot.disposition == SurfaceDisposition::unknown ||
            (qualified != (snapshot.disposition == SurfaceDisposition::qualified))) {
            return exit_code(ExitCode::invalid_sample);
        }
        samples[index] = completed_ns - started_ns;
        qualified_count += qualified ? 1U : 0U;
        blocked_count += qualified ? 0U : 1U;
        combined_reasons |= snapshot.reason_bits;
    }

    const uint64_t p50_ns = percentile(&samples, runs, 50);
    const uint64_t p95_ns = percentile(&samples, runs, 95);
    const uint64_t p99_ns = percentile(&samples, runs, 99);
    const bool qualified = qualified_count == runs;
    StackStringBuilder<384> output;
    const bool written = output.append("macos_surface_qualifier runs=") && output.append_unsigned(runs) &&
                         output.append(" qualified=") && output.append_unsigned(qualified_count) &&
                         output.append(" blocked=") && output.append_unsigned(blocked_count) &&
                         output.append(" reason_bits=") && output.append_unsigned(combined_reasons) &&
                         output.append(" p50_ns=") && output.append_unsigned(p50_ns) && output.append(" p95_ns=") &&
                         output.append_unsigned(p95_ns) && output.append(" p99_ns=") &&
                         output.append_unsigned(p99_ns) && output.append(" accepted=") &&
                         output.append_unsigned(qualified ? 1U : 0U) && output.append('\n');
    if (!written || output.truncated()) return exit_code(ExitCode::invalid_sample);
    emit(output.view());
    return qualified ? exit_code(ExitCode::success) : exit_code(ExitCode::qualification);
}
