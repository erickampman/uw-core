// SpscQueue tests: single-threaded semantics, then a two-thread stress run that
// checks every value arrives exactly once and in order.

#include <cstdint>
#include <memory>
#include <thread>

#include "Check.h"
#include "uw/base/SpscQueue.h"

using namespace uw;

namespace {

void testEmptyAndFull() {
    SpscQueue<int, 4> queue;
    int value = -1;
    CHECK(queue.peek() == nullptr);
    CHECK(!queue.pop(value));

    for (int i = 0; i < 4; ++i) CHECK(queue.push(i));
    CHECK(!queue.push(99));                 // every slot usable, then full
    CHECK(queue.peek() != nullptr && *queue.peek() == 0);

    for (int i = 0; i < 4; ++i) {
        CHECK(queue.pop(value));
        CHECK(value == i);
    }
    CHECK(!queue.pop(value));
}

void testWrapAround() {
    SpscQueue<int, 4> queue;
    int value = -1;
    for (int round = 0; round < 10; ++round) {   // indices run far past capacity
        CHECK(queue.push(round * 2));
        CHECK(queue.push(round * 2 + 1));
        CHECK(queue.pop(value) && value == round * 2);
        CHECK(queue.pop(value) && value == round * 2 + 1);
    }
    CHECK(queue.peek() == nullptr);
}

void testPeekLeavesValue() {
    SpscQueue<int, 8> queue;
    queue.push(7);
    CHECK(queue.peek() != nullptr && *queue.peek() == 7);
    CHECK(queue.peek() != nullptr && *queue.peek() == 7);   // still there
    int value = 0;
    CHECK(queue.pop(value) && value == 7);
}

void testTwoThreads() {
    constexpr std::uint64_t kCount = 2'000'000;
    auto queue = std::make_unique<SpscQueue<std::uint64_t, 1024>>();

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i)
            while (!queue->push(i)) std::this_thread::yield();
    });

    std::uint64_t expected = 1;
    bool          inOrder  = true;
    while (expected <= kCount) {
        std::uint64_t value = 0;
        if (!queue->pop(value)) {
            std::this_thread::yield();
            continue;
        }
        if (value != expected) inOrder = false;
        ++expected;
    }
    producer.join();
    CHECK(inOrder);
    CHECK(queue->peek() == nullptr);
}

} // namespace

int main() {
    testEmptyAndFull();
    testWrapAround();
    testPeekLeavesValue();
    testTwoThreads();
    return test::finish("SpscQueue");
}
