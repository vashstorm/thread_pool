#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __cpp_lib_move_only_function
using ThreadPoolFunction = std::move_only_function<void()>;
#else
using ThreadPoolFunction = std::function<void()>;
#endif

class ThreadPool {
public:
    explicit ThreadPool(size_t thread_number, size_t queue_size = 1024);

    template <typename F, typename... Args>
    auto spawn(F&& f, Args&&... args)
        -> std::expected<std::future<std::invoke_result_t<F, Args...>>,
            std::string>;

    void set_queue_size(size_t new_size);
    void set_worker_count(size_t new_size);
    void shutdown();
    void shutdown_async();

    size_t get_running_thread_count() const;
    size_t get_worker_count() const;
    size_t get_task_count() const;
    size_t get_queue_size() const;

    bool is_quit() const {
        return quit_.load(std::memory_order_acquire);
    }

    ~ThreadPool();
private:
    struct Worker {
        std::thread th;
        std::atomic_bool work_quit {false};

        bool is_quit() const {
            return work_quit.load(std::memory_order_acquire);
        }
    };

    void create_new_worker();
    std::vector<std::unique_ptr<Worker>> workers_;
    std::queue<ThreadPoolFunction> tasks_;
    size_t queue_size_;
    std::atomic_bool quit_ {false};
    mutable std::mutex mtx_;
    std::condition_variable cond_ {};
};

inline void ThreadPool::create_new_worker() {
    // Create a new Worker on the heap to ensure a stable address while the
    // thread captures its pointer
    workers_.emplace_back(std::make_unique<Worker>());
    Worker* wp = workers_.back().get();

    wp->th = std::thread([this, wp] {
        while (true) {
            ThreadPoolFunction task;
            {
                std::unique_lock lock(this->mtx_);
                this->cond_.wait(lock, [this, wp] {
                    return this->is_quit() || !this->tasks_.empty()
                           || wp->is_quit();
                });

                // Shrink on demand: if this worker is marked to quit, exit the
                // loop/thread immediately
                if (wp->is_quit()) {
                    break;
                }

                // Global shutdown: if quitting and the task queue is empty,
                // exit; otherwise keep draining the queue
                if (this->is_quit() && this->tasks_.empty()) {
                    break;
                }

                task = std::move(this->tasks_.front());
                this->tasks_.pop();
            }

            task();
        }
    });
}

inline void ThreadPool::shutdown() {
    {
        std::lock_guard lock(mtx_);
        quit_.store(true, std::memory_order_release);
        for (const auto& w : workers_) {
            w->work_quit.store(true, std::memory_order_release);
        }
    }
    cond_.notify_all();
    for (const auto& w : workers_) {
        if (w->th.joinable()) {
            w->th.join();
        }
    }
    workers_.clear();
}

inline void ThreadPool::shutdown_async() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
}

inline ThreadPool::ThreadPool(const size_t thread_number,
    const size_t queue_size) : queue_size_(queue_size) {
    workers_.reserve(thread_number);
    std::lock_guard lock(mtx_);
    for (size_t i = 0; i < thread_number; i++) {
        create_new_worker();
    }
}

template <typename F, typename... Args>
auto ThreadPool::spawn(F&& f, Args&&... args)
    -> std::expected<std::future<std::invoke_result_t<F, Args...>>,
        std::string> {
    using R = std::invoke_result_t<F, Args...>;
    if (quit_.load(std::memory_order_acquire)) {
        return std::unexpected("ThreadPool is shutting down");
    }

    // Use shared_ptr to keep the task copyable so it is compatible when
    // ThreadPoolFunction is std::function
    auto new_task = std::make_shared<std::packaged_task<R()>>(
        [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
            return std::invoke(func, largs...);
        });

    auto result = new_task->get_future();
    {
        std::lock_guard lock(mtx_);
        if (tasks_.size() >= queue_size_) {
            return std::unexpected("ThreadPool task queue is full");
        }
        // create a new lambda function to capture the task
        tasks_.emplace([task = std::move(new_task)] { (*task)(); });
    }
    cond_.notify_one();
    return result;
}

inline ThreadPool::~ThreadPool() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
    for (const auto& w : workers_) {
        if (w->th.joinable()) {
            w->th.join();
        }
    }
}
inline size_t ThreadPool::get_running_thread_count() const {
    std::lock_guard lock(mtx_);
    // We don't track an exact "running" metric; return the worker count as an
    // approximation
    return workers_.size();
}

inline size_t ThreadPool::get_worker_count() const {
    std::lock_guard lock(mtx_);
    return workers_.size();
}

inline size_t ThreadPool::get_task_count() const {
    std::lock_guard lock(mtx_);
    return tasks_.size();
}

inline size_t ThreadPool::get_queue_size() const {
    std::lock_guard lock(mtx_);
    return queue_size_;
}

inline void ThreadPool::set_queue_size(size_t new_size) {
    if (quit_.load(std::memory_order_acquire)) {
        return;
    }
    new_size = std::clamp(new_size, static_cast<size_t>(1),
        std::numeric_limits<size_t>::max() / 2);
    std::lock_guard lock(mtx_);
    queue_size_ = new_size;
}

inline void ThreadPool::set_worker_count(size_t new_size) {
    if (quit_.load(std::memory_order_acquire)) {
        return;
    }
    new_size = std::clamp(new_size, static_cast<size_t>(1),
        std::numeric_limits<size_t>::max() / 2);

    // Temporarily hold workers to be reclaimed to keep them alive until their
    // threads exit
    std::vector<std::unique_ptr<Worker>> to_join;
    {
        std::lock_guard lock(mtx_);
        if (const size_t current_size = workers_.size();
            new_size > current_size) {
            workers_.reserve(new_size);
            for (size_t i = current_size; i < new_size; i++) {
                create_new_worker();
            }
        } else if (new_size < current_size) {
            const size_t remove_count = current_size - new_size;
            to_join.reserve(remove_count);
            for (size_t i = 0; i < remove_count; ++i) {
                auto& up = workers_.back();
                up->work_quit.store(true, std::memory_order_release);
                // Transfer ownership to to_join to avoid dangling references
                to_join.push_back(std::move(up));
                workers_.pop_back();
            }
        }
    }
    // Notify all workers so that marked threads can exit promptly
    cond_.notify_all();
    // Wait for target threads to exit without holding the lock, then destroy
    // corresponding Workers
    for (const auto& w : to_join) {
        if (w->th.joinable()) {
            w->th.join();
        }
    }
}

#endif /* end of include guard: THREAD_POOL_H */
