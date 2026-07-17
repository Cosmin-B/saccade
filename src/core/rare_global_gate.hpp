#ifndef SACCADE_CORE_RARE_GLOBAL_GATE_HPP
#define SACCADE_CORE_RARE_GLOBAL_GATE_HPP

#include <atomic>

namespace saccade::core {

class RareGlobalGate final {
  public:
    RareGlobalGate() = default;
    RareGlobalGate(const RareGlobalGate&) = delete;
    RareGlobalGate& operator=(const RareGlobalGate&) = delete;

    void enter() noexcept {
        bool expected = false;
        while (!claimed_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = false;
            claimed_.wait(true, std::memory_order_relaxed);
        }
    }

    void leave() noexcept {
        claimed_.store(false, std::memory_order_release);
        claimed_.notify_one();
    }

  private:
    std::atomic<bool> claimed_{false};
};

class RareGlobalGuard final {
  public:
    explicit RareGlobalGuard(RareGlobalGate& gate) noexcept : gate_(gate) { gate_.enter(); }

    ~RareGlobalGuard() { gate_.leave(); }

    RareGlobalGuard(const RareGlobalGuard&) = delete;
    RareGlobalGuard& operator=(const RareGlobalGuard&) = delete;

  private:
    RareGlobalGate& gate_;
};

} // namespace saccade::core

#endif
