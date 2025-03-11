#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <optional>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
    ThreadPool(size_t thread_number, size_t queue_size = 1024);
    template <typename F, typename... Args>
    auto spawn(F&& f, Args&&... args)
        -> std::optional<std::future<typename std::invoke_result_t<F, Args...>>>;

    virtual ~ThreadPool();
private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    size_t queue_size_;
    std::atomic_flag quit_ {};
    std::mutex mtx_;
    std::condition_variable cond_ {};
};

inline ThreadPool::ThreadPool(size_t thread_number, size_t queue_size) : queue_size_(queue_size) {
    workers_.reserve(thread_number);
    for (size_t i = 0; i < thread_number; i++) {
        workers_.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mtx_);
                    this->cond_.wait(lock, [this] { 
                        return this->quit_.test() || !this->tasks_.empty(); 
                    });
                    if (quit_.test() && tasks_.empty()) {
                        return;
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

template <typename F, typename... Args>
std::optional<std::future<typename std::invoke_result_t<F, Args...>>> ThreadPool::spawn(
    F&& f,
    Args&&... args) {
    using return_type = typename std::invoke_result_t<F, Args...>;
    if (quit_.test() || tasks_.size() >= queue_size_) {
        return std::nullopt;
    }

    // F can contain ref-capture by lambda, so need use forward<F>(f)
    // auto task = std::make_shared<std::packaged_task<return_type()>>(
    //     [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
    //         return func(std::move(largs)...);
    //     });
    // auto task = std::make_shared<std::packaged_task<return_type()>>(
    //     [func = std::forward<F>(f), tup = std::tuple<Args...>(std::forward<Args>(args)...)] {
    //         return std::apply(func, tup);
    //     });
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
            return std::invoke(func, largs...);
        });

    auto result = task->get_future();
    {
        std::lock_guard lock(mtx_);
        if (tasks_.size() >= queue_size_) {
            return std::nullopt;
        }
        // just create a new lambda function to capture the task
        tasks_.emplace([task] { (*task)(); });
    }
    cond_.notify_one();
    return result;
}

inline ThreadPool::~ThreadPool() {
    quit_.test_and_set();
    cond_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

#endif /* end of include guard: __THREADPOOL_H__ */
