#include "../support/allocation_tracker.hpp"
#include "core/stack_string_builder.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace {

bool equals(std::string_view actual, std::string_view expected) noexcept {
    return actual == expected;
}

} // namespace

int main() {
    using saccade::core::StackStringBuilder;

    if (!saccade::test::allocation_tracker_self_test()) {
        return 1;
    }

    StackStringBuilder<64> text;
    if (!text.empty() || text.size() != 0 || text.capacity() != 64 || text.truncated() || text.c_str()[0] != '\0') {
        return 2;
    }

    if (!text.append("frame ") || !text.append_unsigned(42) || !text.append(" at ") || !text.append_signed(-7) ||
        !equals(text.view(), "frame 42 at -7")) {
        return 3;
    }

    text.reset();
    if (!text.append_unsigned(std::numeric_limits<uint64_t>::max()) || !text.append(' ') ||
        !text.append_signed(std::numeric_limits<int64_t>::min()) ||
        !equals(text.view(), "18446744073709551615 -9223372036854775808")) {
        return 4;
    }

    StackStringBuilder<5> bounded;
    if (bounded.append("abcdef") || !bounded.truncated() || bounded.size() != bounded.capacity() ||
        !equals(bounded.view(), "abcde") || bounded.c_str()[5] != '\0') {
        return 5;
    }

    if (bounded.append('z') || !equals(bounded.view(), "abcde")) {
        return 6;
    }

    bounded.reset();
    if (bounded.truncated() || !bounded.append("abcde") || !equals(bounded.view(), "abcde")) {
        return 7;
    }

    StackStringBuilder<3> number;
    if (number.append_unsigned(1234) || !number.truncated() || !equals(number.view(), "123")) {
        return 8;
    }

    StackStringBuilder<0> empty;
    if (empty.append('x') || !empty.truncated() || !empty.view().empty() || empty.c_str()[0] != '\0') {
        return 9;
    }

    bool measured_complete = false;
    bool measured_equal = false;
    saccade::test::begin_allocation_tracking();
    {
        StackStringBuilder<64> measured;
        measured_complete = measured.append("frame ") && measured.append_unsigned(120) && measured.append(" target ") &&
                            measured.append_signed(-42);
        measured_equal = equals(measured.view(), "frame 120 target -42");
    }
    const size_t measured_allocations = saccade::test::end_allocation_tracking();
    if (!measured_complete || !measured_equal || measured_allocations != 0) {
        return 10;
    }

    return 0;
}
