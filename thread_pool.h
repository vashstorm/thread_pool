#ifndef _THREAD_POOL_H_
#define _THREAD_POOL_H_

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_map>
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
        -> std::optional<std::future<std::invoke_result_t<F, Args...>>>;

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
    std::vector<std::thread> workers_;
    std::unordered_map<std::thread::id, std::atomic_bool> workers_quit_;
    std::queue<ThreadPoolFunction> tasks_;
    size_t queue_size_;
    std::atomic_bool quit_ {false};
    mutable std::mutex mtx_;
    std::condition_variable cond_ {};
};

inline void ThreadPool::create_new_worker() {
    workers_.emplace_back([this] {
        // std::println("thread_id: {} thread map size: {}", std::this_thread::get_id(),
        //  this->workers_quit_.size());
        auto id = std::this_thread::get_id();
        {
            std::lock_guard lock(mtx_);
            this->workers_quit_.emplace(id, false);
        }
        while (true) {
            ThreadPoolFunction task;
            {
                std::unique_lock lock(this->mtx_);
                this->cond_.wait(lock, [this] {
                    return this->is_quit() || !this->tasks_.empty();
                });
                if (this->workers_quit_[id].load(std::memory_order_acquire)) {
                    this->workers_quit_.erase(id);
                    return;
                }

                if (this->is_quit() && this->tasks_.empty()) {
                    break;
                }
                task = std::move(this->tasks_.front());
                this->tasks_.pop();
            }

            task();
        }

        // std::println("thread_id: {} quit, map size: {} ", std::this_thread::get_id(),
        // this->workers_quit_.size());
    });
    // std::this_thread::sleep_for(std::chrono::seconds(1));
    // std::println("workers_.back(): {}", workers_.back().get_id());
    // std::this_thread::sleep_for(std::chrono::seconds(1));
}

inline void ThreadPool::shutdown() {
    quit_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(mtx_);
        for (auto& worker : workers_) {
            workers_quit_[worker.get_id()].store(true, std::memory_order_release);
        }
    }
    cond_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

inline void ThreadPool::shutdown_async() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
}

inline ThreadPool::ThreadPool(const size_t thread_number, const size_t queue_size) :
    queue_size_(queue_size) {
    workers_.reserve(thread_number);
    std::lock_guard lock(mtx_);
    for (size_t i = 0; i < thread_number; i++) {
        create_new_worker();
    }
}

template <typename F, typename... Args>
std::optional<std::future<std::invoke_result_t<F, Args...>>> ThreadPool::spawn(
    F&& f, Args&&... args) {
    using R = std::invoke_result_t<F, Args...>;
    if (quit_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    // F can contain ref-capture by lambda, so need use forward<F>(f)
    // auto task = std::make_shared<std::packaged_task<return_type()>>(
    //     [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
    //         return func(std::move(largs)...);
    //     });
    // auto task = std::make_shared<std::packaged_task<return_type()>>(
    //     [func = std::forward<F>(f), tup =
    //     std::tuple<Args...>(std::forward<Args>(args)...)] {
    //         return std::apply(func, tup);
    //     });
    // transform args into noname class member data
    auto newtask = std::make_unique<std::packaged_task<R()>>(
        [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
            return std::invoke(func, largs...);
        });

    auto result = newtask->get_future();
    {
        std::lock_guard lock(mtx_);
        if (tasks_.size() >= queue_size_) {
            return std::nullopt;
        }
        // create a new lambda function to capture the task
        tasks_.emplace([task = std::move(newtask)] { (*task)(); });
    }
    cond_.notify_one();
    return result;
}

inline ThreadPool::~ThreadPool() {
    quit_.store(true, std::memory_order_release);
    cond_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}
inline size_t ThreadPool::get_running_thread_count() const {
    std::lock_guard lock(mtx_);
    // 当前没有跟踪“正在运行”的精确度量，返回工作线程总数作为近似值
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

    std::lock_guard lock(mtx_);
    if (const size_t current_size = workers_.size(); new_size > current_size) {
        workers_.reserve(new_size);
        for (size_t i = current_size; i < new_size; i++) {
            create_new_worker();
        }
    } else if (new_size < current_size) {
        for (size_t i = new_size; i < current_size; i++) {
            auto& worker = workers_.back();
            workers_quit_[worker.get_id()].store(true, std::memory_order_release);
            worker.detach();
            workers_.pop_back();
        }
        workers_.reserve(new_size);
        cond_.notify_all();
    }
}

#endif /* end of include guard: _THREAD_POOL_H_ */
