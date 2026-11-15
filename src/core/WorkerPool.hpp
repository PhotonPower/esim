#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace esim {

// A fixed set of threads for short jobs that follow each other closely:
// run(job) calls job(k) for k = 0 .. parts() - 1, part 0 on the calling
// thread, and returns when all parts are done.
//
// Between jobs the workers wait on a condition variable, but first they look
// for the next job for a short while without sleeping: jobs usually follow
// within microseconds (one per Newton iteration), and a wake-up through the
// operating system would cost more than many jobs take.
class WorkerPool {
public:
    explicit WorkerPool(int parts) : parts_(parts < 1 ? 1 : parts) {
        for (int k = 1; k < parts_; ++k) workers_.emplace_back([this, k] { work(k); });
    }
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            posted_.fetch_add(1, std::memory_order_release);
        }
        wake_.notify_all();
        for (std::thread& t : workers_) t.join();
    }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int parts() const { return parts_; }

    void run(const std::function<void(int)>& job) {
        if (parts_ == 1) {
            job(0);
            return;
        }
        job_ = &job;
        pending_.store(parts_ - 1, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            posted_.fetch_add(1, std::memory_order_release);
        }
        wake_.notify_all();
        job(0);
        // The other parts take about as long as this one: wait actively.
        while (pending_.load(std::memory_order_acquire) > 0) pause();
    }

private:
    static void pause() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#else
        std::this_thread::yield();
#endif
    }

    void work(int k) {
        unsigned long seen = 0;
        for (;;) {
            for (int spin = 0; spin < 20000 && posted_.load(std::memory_order_acquire) == seen; ++spin)
                pause();
            if (posted_.load(std::memory_order_acquire) == seen) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return posted_.load(std::memory_order_acquire) != seen; });
            }
            seen = posted_.load(std::memory_order_acquire);
            if (stop_) return;
            (*job_)(k);
            pending_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    const int parts_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<unsigned long> posted_{0};  // number of jobs posted so far
    std::atomic<int> pending_{0};
    const std::function<void(int)>* job_ = nullptr;
    std::atomic<bool> stop_{false};
};

}  // namespace esim
