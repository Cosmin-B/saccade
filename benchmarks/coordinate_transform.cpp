#include "geometry/coordinate_transform.hpp"
#include "core/stack_string_builder.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

enum class ExitCode : int {
    success = 0,
    benchmark_failure = 1,
    output_failure = 2,
};

constexpr int to_process_exit_code(ExitCode code) noexcept {
    return static_cast<int>(code);
}

constexpr uint32_t rectangle_count = 10000;

alignas(64) std::array<saccade::geometry::RectQ8, rectangle_count> inputs{};
alignas(64) std::array<saccade::geometry::RectQ8, rectangle_count> outputs{};
uint64_t benchmark_sink = 0;

constexpr int32_t q8(int32_t value) noexcept {
    return value * saccade::geometry::coordinate_one;
}

void build_inputs() noexcept {
    for (uint32_t index = 0; index < rectangle_count; ++index) {
        inputs[index] = {q8(-1920) + static_cast<int32_t>(index % 100U) * q8(72),
                         q8(-240) + static_cast<int32_t>(index / 100U) * q8(36), q8(48), q8(24)};
    }
}

uint64_t measure_batches(const saccade::geometry::CoordinateTransform& transform, uint32_t iterations) noexcept {
    uint64_t accumulator = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        for (uint32_t index = 0; index < rectangle_count; ++index) {
            const SaccadeResult result = transform.map_rect_clipped(inputs[index], &outputs[index]);
            accumulator += result == SACCADE_OK ? static_cast<uint32_t>(outputs[index].x) : 0U;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    benchmark_sink = accumulator;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

bool append_result(saccade::core::StackStringBuilder<1024>* text, saccade::geometry::QuarterTurn rotation,
                   uint32_t iterations, uint64_t elapsed_ns) noexcept {
    const uint64_t operations = static_cast<uint64_t>(rectangle_count) * iterations;
    return elapsed_ns != 0 && text->append("rects=") && text->append_unsigned(rectangle_count) &&
           text->append(" rotation=") && text->append_unsigned(static_cast<uint32_t>(rotation) * 90U) &&
           text->append(" iterations=") && text->append_unsigned(iterations) && text->append(" total_ns=") &&
           text->append_unsigned(elapsed_ns) && text->append(" ns_per_batch=") &&
           text->append_unsigned(elapsed_ns / iterations) && text->append(" ns_per_rect=") &&
           text->append_unsigned(elapsed_ns / operations) && text->append('\n');
}

bool run_case(saccade::geometry::QuarterTurn rotation, uint32_t iterations,
              saccade::core::StackStringBuilder<1024>* text) noexcept {
    saccade::geometry::TransformDesc desc{};
    desc.source = {q8(-1920), q8(-240), q8(9600), q8(4560)};
    desc.destination = {0, 0, q8(7680), q8(4320)};
    desc.epoch = 1;
    desc.source_space = saccade::geometry::CoordinateSpace::desktop;
    desc.destination_space = saccade::geometry::CoordinateSpace::surface;
    desc.rotation = rotation;
    saccade::geometry::CoordinateTransform transform;
    if (transform.initialize(desc) != SACCADE_OK) {
        return false;
    }
    const uint64_t elapsed_ns = measure_batches(transform, iterations);
    return append_result(text, rotation, iterations, elapsed_ns);
}

} // namespace

int main() {
    build_inputs();
    saccade::core::StackStringBuilder<1024> text;
    if (!run_case(saccade::geometry::QuarterTurn::clockwise_0, 1000, &text) ||
        !run_case(saccade::geometry::QuarterTurn::clockwise_90, 1000, &text) || text.truncated()) {
        return to_process_exit_code(ExitCode::benchmark_failure);
    }
    return to_process_exit_code(std::fwrite(text.view().data(), 1, text.view().size(), stdout) == text.view().size()
                                    ? ExitCode::success
                                    : ExitCode::output_failure);
}
