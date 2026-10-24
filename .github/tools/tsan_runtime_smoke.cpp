#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

// Timed waits must release and reacquire the mutex in the sanitizer's model as well as in
// the standard library. Older libtsan runtimes omit pthread_cond_clockwait interception.
int main() {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> entered{};
    bool ready{};
    int result{};
    std::thread waiter([&] {
        std::unique_lock lock{mutex};
        entered.store(true, std::memory_order_release);
        if (!changed.wait_for(lock, std::chrono::seconds{2}, [&] { return ready; }))
            result = 1;
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    {
        std::lock_guard lock{mutex};
        ready = true;
    }
    changed.notify_one();
    waiter.join();
    if (!result)
        std::puts("GDPP_TSAN_TIMED_WAIT_PASSED");
    return result;
}
