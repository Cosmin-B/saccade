#ifndef SACCADE_CORE_HANDLE_TABLE_HPP
#define SACCADE_CORE_HANDLE_TABLE_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace saccade::core {

template <typename T, size_t Capacity, typename Generation = uint32_t>
class HandleTable final {
    static_assert(Capacity > 0);
    static_assert(Capacity <= std::numeric_limits<uint32_t>::max());
    static_assert(std::is_integral_v<Generation> && std::is_unsigned_v<Generation>);
    static_assert(sizeof(Generation) <= sizeof(uint32_t));

public:
    HandleTable() = default;
    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;
    HandleTable(HandleTable&&) = delete;
    HandleTable& operator=(HandleTable&&) = delete;

    template <typename... Arguments>
    SaccadeResult emplace(uint64_t* out_handle, Arguments&&... arguments)
        noexcept(std::is_nothrow_constructible_v<T, Arguments&&...>) {
        if (out_handle == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_handle = 0;

        for (size_t index = 0; index < Capacity; ++index) {
            Slot& slot = slots_[index];
            if (slot.retired || slot.value.has_value()) {
                continue;
            }

            slot.value.emplace(std::forward<Arguments>(arguments)...);
            slot.sequence = next_sequence();
            ++size_;
            *out_handle = encode(index, slot.generation);
            return SACCADE_OK;
        }
        return SACCADE_ERROR_CAPACITY;
    }

    T* get(uint64_t handle) noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return nullptr;
        }
        Slot& slot = slots_[decoded.index];
        if (!slot.value.has_value() ||
            static_cast<uint32_t>(slot.generation) != decoded.generation) {
            return nullptr;
        }
        return &slot.value.value();
    }

    const T* get(uint64_t handle) const noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return nullptr;
        }
        const Slot& slot = slots_[decoded.index];
        if (!slot.value.has_value() ||
            static_cast<uint32_t>(slot.generation) != decoded.generation) {
            return nullptr;
        }
        return &slot.value.value();
    }

    SaccadeResult erase(uint64_t handle) noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        Slot& slot = slots_[decoded.index];
        if (!slot.value.has_value() ||
            static_cast<uint32_t>(slot.generation) != decoded.generation) {
            return SACCADE_ERROR_STALE_HANDLE;
        }

        slot.value.reset();
        slot.sequence = 0;
        advance_generation(slot);
        --size_;
        return SACCADE_OK;
    }

    template <typename Function>
    void for_each(Function&& function) {
        for (size_t index = 0; index < Capacity; ++index) {
            Slot& slot = slots_[index];
            if (slot.value.has_value()) {
                function(encode(index, slot.generation), slot.value.value());
            }
        }
    }

    template <typename Function>
    void for_each(Function&& function) const {
        for (size_t index = 0; index < Capacity; ++index) {
            const Slot& slot = slots_[index];
            if (slot.value.has_value()) {
                function(encode(index, slot.generation), slot.value.value());
            }
        }
    }

    template <typename Function>
        requires std::is_nothrow_invocable_v<Function&, uint64_t, T&>
    void clear_reverse(Function&& before_destroy) {
        std::array<size_t, Capacity> order{};
        size_t count = 0;
        for (size_t index = 0; index < Capacity; ++index) {
            if (slots_[index].value.has_value()) {
                order[count++] = index;
            }
        }

        for (size_t index = 1; index < count; ++index) {
            const size_t candidate = order[index];
            size_t position = index;
            while (position > 0 &&
                   slots_[order[position - 1]].sequence < slots_[candidate].sequence) {
                order[position] = order[position - 1];
                --position;
            }
            order[position] = candidate;
        }

        for (size_t position = 0; position < count; ++position) {
            const size_t index = order[position];
            Slot& slot = slots_[index];
            before_destroy(encode(index, slot.generation), slot.value.value());
            slot.value.reset();
            slot.sequence = 0;
            advance_generation(slot);
        }
        size_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }

private:
    struct Slot {
        std::optional<T> value;
        Generation generation = 1;
        uint64_t sequence = 0;
        bool retired = false;
    };

    struct Decoded {
        size_t index = 0;
        uint32_t generation = 0;
        bool valid = false;
    };

    static uint64_t encode(size_t index, Generation generation) noexcept {
        const uint64_t slot = static_cast<uint64_t>(index) + UINT64_C(1);
        return (static_cast<uint64_t>(generation) << 32U) | slot;
    }

    static Decoded decode(uint64_t handle) noexcept {
        const uint32_t slot = static_cast<uint32_t>(handle & UINT64_C(0xFFFFFFFF));
        const uint32_t generation = static_cast<uint32_t>(handle >> 32U);
        if (slot == 0 || generation == 0 || static_cast<size_t>(slot) > Capacity) {
            return {};
        }
        return {static_cast<size_t>(slot - 1U), generation, true};
    }

    static void advance_generation(Slot& slot) noexcept {
        if (slot.generation == std::numeric_limits<Generation>::max()) {
            slot.retired = true;
            return;
        }
        ++slot.generation;
    }

    uint64_t next_sequence() noexcept {
        ++next_sequence_;
        if (next_sequence_ == 0) {
            next_sequence_ = 1;
        }
        return next_sequence_;
    }

    std::array<Slot, Capacity> slots_{};
    size_t size_ = 0;
    uint64_t next_sequence_ = 0;
};

}  // namespace saccade::core

#endif
