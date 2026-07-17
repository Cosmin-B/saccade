#ifndef SACCADE_CORE_HANDLE_TABLE_HPP
#define SACCADE_CORE_HANDLE_TABLE_HPP

#include <saccade/saccade.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace saccade::core {

template <typename T, size_t Capacity, typename Generation = uint32_t> class HandleTable final {
    static_assert(Capacity > 0);
    static_assert(Capacity <= std::numeric_limits<uint32_t>::max());
    static_assert(std::is_integral_v<Generation> && std::is_unsigned_v<Generation>);
    static_assert(sizeof(Generation) <= sizeof(uint32_t));
    static_assert(std::is_nothrow_destructible_v<T>);

  public:
    HandleTable() = default;

    ~HandleTable() {
        for (size_t index = 0; index < Capacity; ++index) {
            if (metadata_[index].occupied) {
                std::destroy_at(value(index));
            }
        }
    }

    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;
    HandleTable(HandleTable&&) = delete;
    HandleTable& operator=(HandleTable&&) = delete;

    template <typename... Arguments>
    SaccadeResult emplace(uint64_t* out_handle,
                          Arguments&&... arguments) noexcept(std::is_nothrow_constructible_v<T, Arguments&&...>) {
        if (out_handle == nullptr) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        *out_handle = 0;

        for (size_t index = 0; index < Capacity; ++index) {
            Metadata& metadata = metadata_[index];
            if (metadata.retired || metadata.occupied) {
                continue;
            }

            std::construct_at(value(index), std::forward<Arguments>(arguments)...);
            metadata.sequence = next_sequence();
            metadata.occupied = true;
            ++size_;
            *out_handle = encode(index, metadata.generation);
            return SACCADE_OK;
        }
        return SACCADE_ERROR_CAPACITY;
    }

    T* get(uint64_t handle) noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return nullptr;
        }
        Metadata& metadata = metadata_[decoded.index];
        if (!metadata.occupied || static_cast<uint32_t>(metadata.generation) != decoded.generation) {
            return nullptr;
        }
        return value(decoded.index);
    }

    const T* get(uint64_t handle) const noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return nullptr;
        }
        const Metadata& metadata = metadata_[decoded.index];
        if (!metadata.occupied || static_cast<uint32_t>(metadata.generation) != decoded.generation) {
            return nullptr;
        }
        return value(decoded.index);
    }

    SaccadeResult erase(uint64_t handle) noexcept {
        const Decoded decoded = decode(handle);
        if (!decoded.valid) {
            return SACCADE_ERROR_STALE_HANDLE;
        }
        Metadata& metadata = metadata_[decoded.index];
        if (!metadata.occupied || static_cast<uint32_t>(metadata.generation) != decoded.generation) {
            return SACCADE_ERROR_STALE_HANDLE;
        }

        std::destroy_at(value(decoded.index));
        metadata.occupied = false;
        metadata.sequence = 0;
        advance_generation(metadata);
        --size_;
        return SACCADE_OK;
    }

    template <typename Function> void for_each(Function&& function) {
        for (size_t index = 0; index < Capacity; ++index) {
            Metadata& metadata = metadata_[index];
            if (metadata.occupied) {
                function(encode(index, metadata.generation), *value(index));
            }
        }
    }

    template <typename Function> void for_each(Function&& function) const {
        for (size_t index = 0; index < Capacity; ++index) {
            const Metadata& metadata = metadata_[index];
            if (metadata.occupied) {
                function(encode(index, metadata.generation), *value(index));
            }
        }
    }

    template <typename Function>
        requires std::is_nothrow_invocable_v<Function&, uint64_t, T&>
    void clear_reverse(Function&& before_destroy) {
        std::array<size_t, Capacity> order{};
        size_t count = 0;
        for (size_t index = 0; index < Capacity; ++index) {
            if (metadata_[index].occupied) {
                order[count++] = index;
            }
        }

        for (size_t index = 1; index < count; ++index) {
            const size_t candidate = order[index];
            size_t position = index;
            while (position > 0 && metadata_[order[position - 1]].sequence < metadata_[candidate].sequence) {
                order[position] = order[position - 1];
                --position;
            }
            order[position] = candidate;
        }

        for (size_t position = 0; position < count; ++position) {
            const size_t index = order[position];
            Metadata& metadata = metadata_[index];
            before_destroy(encode(index, metadata.generation), *value(index));
            std::destroy_at(value(index));
            metadata.occupied = false;
            metadata.sequence = 0;
            advance_generation(metadata);
        }
        size_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }

  private:
    struct alignas(T) Storage {
        std::array<std::byte, sizeof(T)> bytes{};
    };

    struct Metadata {
        uint64_t sequence = 0;
        Generation generation = 1;
        bool occupied = false;
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

    static void advance_generation(Metadata& metadata) noexcept {
        if (metadata.generation == std::numeric_limits<Generation>::max()) {
            metadata.retired = true;
            return;
        }
        ++metadata.generation;
    }

    uint64_t next_sequence() noexcept {
        ++next_sequence_;
        if (next_sequence_ == 0) {
            next_sequence_ = 1;
        }
        return next_sequence_;
    }

    T* value(size_t index) noexcept { return std::launder(reinterpret_cast<T*>(storage_[index].bytes.data())); }

    const T* value(size_t index) const noexcept {
        return std::launder(reinterpret_cast<const T*>(storage_[index].bytes.data()));
    }

    static_assert(sizeof(Storage) == sizeof(T));

    std::array<Storage, Capacity> storage_{};
    std::array<Metadata, Capacity> metadata_{};
    size_t size_ = 0;
    uint64_t next_sequence_ = 0;
};

} // namespace saccade::core

#endif
