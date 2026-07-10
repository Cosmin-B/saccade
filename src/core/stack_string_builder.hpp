#ifndef SACCADE_CORE_STACK_STRING_BUILDER_HPP
#define SACCADE_CORE_STACK_STRING_BUILDER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace saccade::core {

template <size_t Capacity>
class StackStringBuilder final {
public:
    constexpr StackStringBuilder() noexcept = default;

    bool append(std::string_view text) noexcept {
        const size_t available = Capacity - size_;
        const size_t copied = text.size() < available ? text.size() : available;
        if (copied != 0) {
            std::memcpy(storage_.data() + size_, text.data(), copied);
            size_ += copied;
            storage_[size_] = '\0';
        }
        if (copied != text.size()) {
            truncated_ = true;
            return false;
        }
        return true;
    }

    bool append(char value) noexcept {
        if (size_ == Capacity) {
            truncated_ = true;
            return false;
        }
        storage_[size_] = value;
        ++size_;
        storage_[size_] = '\0';
        return true;
    }

    bool append_unsigned(uint64_t value) noexcept {
        char digits[20];
        char* const end = digits + sizeof(digits);
        char* begin = end;
        do {
            const uint64_t digit = value % 10U;
            --begin;
            *begin = static_cast<char>('0' + static_cast<int>(digit));
            value /= 10U;
        } while (value != 0);
        return append(std::string_view(begin, static_cast<size_t>(end - begin)));
    }

    bool append_signed(int64_t value) noexcept {
        const bool negative = value < 0;
        const uint64_t magnitude = negative
            ? static_cast<uint64_t>(-(value + 1)) + 1U
            : static_cast<uint64_t>(value);
        const bool sign_complete = !negative || append('-');
        const bool digits_complete = append_unsigned(magnitude);
        return sign_complete && digits_complete;
    }

    void reset() noexcept {
        size_ = 0;
        truncated_ = false;
        storage_[0] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {storage_.data(), size_};
    }

    [[nodiscard]] const char* c_str() const noexcept {
        return storage_.data();
    }

    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool truncated() const noexcept {
        return truncated_;
    }

private:
    std::array<char, Capacity + 1> storage_{};
    size_t size_ = 0;
    bool truncated_ = false;
};

}  // namespace saccade::core

#endif
