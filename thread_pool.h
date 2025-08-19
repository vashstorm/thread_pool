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
    explicit ThreadPool(size_t thread_number = 0, size_t queue_size = 1024);

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
    void create_new_worker();
    std::vector<std::jthread> workers_;
    std::queue<ThreadPoolFunction> tasks_;
    size_t queue_size_;
    std::atomic_bool quit_ {false};
    mutable std::mutex mtx_;
    std::condition_variable_any cond_ {};
};

inline void ThreadPool::create_new_worker() {
    workers_.emplace_back([this](std::stop_token st) {
        while (true) {
            ThreadPoolFunction task;
            {
                std::unique_lock lock(this->mtx_);
                this->cond_.wait(lock, st, [this, &st] {
                    return st.stop_requested() || this->is_quit()
                           || !this->tasks_.empty();
                });

                if (st.stop_requested()) {
                    break;
                }

                // During global shutdown, keep draining the task queue; exit
                // after the queue becomes empty
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
    }
    // Wake up all threads to continue draining the queue; they will exit after
    // the queue becomes empty
    cond_.notify_all();

    for (auto& jt : workers_) {
        if (jt.joinable()) {
            jt.join();
        }
    }
    workers_.clear();
}

inline void ThreadPool::shutdown_async() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
}

inline ThreadPool::ThreadPool(size_t thread_number, size_t queue_size) :
    queue_size_(queue_size) {
    if (thread_number <= 0) {
        thread_number = std::thread::hardware_concurrency();
    }
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
    auto task = std::make_shared<std::packaged_task<R()>>(
        [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
            return std::invoke(func, largs...);
        });

    auto result = task->get_future();
    {
        std::lock_guard lock(mtx_);
        if (tasks_.size() >= queue_size_) {
            return std::unexpected("ThreadPool task queue is full");
        }
        // create a new lambda function to capture the task
        tasks_.emplace([task = std::move(task)] { (*task)(); });
    }
    cond_.notify_one();
    return result;
}

inline ThreadPool::~ThreadPool() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
    for (auto& jt : workers_) {
        if (jt.joinable()) {
            jt.join();
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

    // Temporarily store threads to stop; join them after releasing the lock
    std::vector<std::jthread> to_join;
    {
        std::lock_guard lock(mtx_);
        const size_t current_size = workers_.size();
        if (new_size > current_size) {
            workers_.reserve(new_size);
            for (size_t i = current_size; i < new_size; ++i) {
                create_new_worker();
            }
        } else if (new_size < current_size) {
            const size_t remove_count = current_size - new_size;
            to_join.reserve(remove_count);
            for (size_t i = 0; i < remove_count; ++i) {
                std::jthread jt = std::move(workers_.back());
                workers_.pop_back();
                // Request this worker to stop immediately
                jt.request_stop();
                to_join.push_back(std::move(jt));
            }
        }
    }
    // Wake waiting threads so they can respond promptly
    cond_.notify_all();

    for (auto& jt : to_join) {
        if (jt.joinable()) {
            jt.join();
        }
    }
}

#endif /* end of include guard: THREAD_POOL_H */
