#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace uw {

// Bounded single-producer / single-consumer lock-free queue: the thread-crossing
// primitive in StageHand's docs/domain-model.md (MIDI->RT, Main->RT, RT->Main). Exactly one
// thread may push and exactly one other thread may pop. push(), peek() and pop()
// never block, lock, or allocate, so either side may be the real-time thread.
//
// Capacity must be a power of two. Indices are free-running counters, so every
// slot is usable. T must be trivially copyable (messages, events).
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscQueue capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue holds trivially copyable values");

public:
    // Producer. Returns false when full (the value is not queued).
    bool push(const T& value) noexcept {
        const std::size_t tail = tailIndex.load(std::memory_order_relaxed);
        if (tail - headCache == Capacity) {
            headCache = headIndex.load(std::memory_order_acquire);
            if (tail - headCache == Capacity) return false;
        }
        slots[tail & (Capacity - 1)] = value;
        tailIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Producer. Whether push() would succeed right now. (Room only grows until
    // the producer pushes, so a true result stays true for the producer.)
    bool canPush() noexcept {
        const std::size_t tail = tailIndex.load(std::memory_order_relaxed);
        if (tail - headCache == Capacity)
            headCache = headIndex.load(std::memory_order_acquire);
        return tail - headCache != Capacity;
    }

    // Consumer. The oldest value without removing it, or null when empty. Valid
    // until the next pop(). (M3 uses it to leave MIDI stamped after the current
    // block's start in the queue for the next block.)
    const T* peek() noexcept {
        const std::size_t head = headIndex.load(std::memory_order_relaxed);
        if (head == tailCache) {
            tailCache = tailIndex.load(std::memory_order_acquire);
            if (head == tailCache) return nullptr;
        }
        return &slots[head & (Capacity - 1)];
    }

    // Consumer. Returns false when empty.
    bool pop(T& value) noexcept {
        const T* front = peek();
        if (front == nullptr) return false;
        value = *front;
        headIndex.store(headIndex.load(std::memory_order_relaxed) + 1,
                        std::memory_order_release);
        return true;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kCacheLine = 64;

    // Consumer-owned line: head index (read by the producer) + cached tail.
    alignas(kCacheLine) std::atomic<std::size_t> headIndex{0};
    std::size_t tailCache = 0;

    // Producer-owned line: tail index (read by the consumer) + cached head.
    alignas(kCacheLine) std::atomic<std::size_t> tailIndex{0};
    std::size_t headCache = 0;

    alignas(kCacheLine) T slots[Capacity];
};

} // namespace uw
