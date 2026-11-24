#include "IRQCancellation.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::exit(1); } } while (0)

int main()
{
    using namespace extgpu_driver;
    for (bool success : {false, true}) {
        IRQCancellation callbackFirst{};
        CHECK(begin_irq_cancel(callbackFirst));
        CHECK(!begin_irq_cancel(callbackFirst));
        CHECK(!record_irq_cancel_completion(callbackFirst));
        CHECK(record_irq_cancel_return(callbackFirst, success) == success);
        CHECK(!record_irq_cancel_completion(callbackFirst));
        CHECK(!record_irq_cancel_return(callbackFirst, success));
        CHECK(!begin_irq_cancel(callbackFirst));

        IRQCancellation returnFirst{};
        CHECK(begin_irq_cancel(returnFirst));
        CHECK(!record_irq_cancel_return(returnFirst, success));
        CHECK(record_irq_cancel_completion(returnFirst) == success);
        CHECK(!record_irq_cancel_return(returnFirst, success));
        CHECK(!record_irq_cancel_completion(returnFirst));
        CHECK(!begin_irq_cancel(returnFirst));

        /* Exercise the same production atomics with racing API return and
         * completion. A false Cancel must retain every reference even when
         * its callback arrives, and true Cancel permits exactly one retire.
         */
        for (unsigned trial = 0; trial < 512; ++trial) {
            IRQCancellation concurrent{};
            CHECK(begin_irq_cancel(concurrent));
            std::atomic<bool> start{false};
            std::atomic<unsigned> retired{0};
            std::thread completion([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                if (record_irq_cancel_completion(concurrent)) retired.fetch_add(1);
            });
            std::thread returning([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                if (record_irq_cancel_return(concurrent, success)) retired.fetch_add(1);
            });
            start.store(true, std::memory_order_release);
            completion.join();
            returning.join();
            CHECK(retired.load() == (success ? 1u : 0u));
            CHECK(!begin_irq_cancel(concurrent));
        }
    }
    std::puts("PASS production IRQ cancellation handshake: inline/late/racing completion, exact-once retirement, failure retains ownership; no SDK/hardware emulation");
}
