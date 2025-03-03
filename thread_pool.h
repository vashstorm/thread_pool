#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <iterator>
#include <optional>
#include <queue>
#include <type_traits>
#include <vector>
#include <print>

class ThreadPool {
public:
    ThreadPool(size_t thread_number, size_t queue_size = 1024);
    template<typename F, typename... Args> 
    auto spawn(F&& f, Args&&... args) -> std::optional<std::future<typename std::result_of_t<F(Args...)>>>;

    virtual ~ThreadPool ();
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    size_t queue_size;
    std::atomic_flag quit{};
    std::mutex mtx;
    std::condition_variable cond {};
};

inline ThreadPool::ThreadPool(size_t thread_number, size_t _queue_size): queue_size(_queue_size) {
    workers.reserve(thread_number);
    for (size_t i = 0; i < thread_number; i++) {
        workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mtx);
                    this->cond.wait(lock, [this] { return this->quit.test() || !this->tasks.empty(); });
                    if (quit.test() && tasks.empty()) return;
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
        });
    }
}

template<typename F, typename... Args>
std::optional<std::future<typename std::result_of_t<F(Args...)>>>
ThreadPool::spawn(F&& f, Args&&... args) {
    using return_type = typename std::result_of_t<F(Args...)>;

    if (quit.test() || tasks.size() >= queue_size) return std::nullopt;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [func = std::forward<F>(f), ... largs = std::forward<Args>(args)] {
            return func(std::move(largs)...);
        }
    );
    auto result = task->get_future(); 
    {
        std::lock_guard lock(mtx);
        tasks.emplace([task] { (*task)(); });
    }
    cond.notify_one();
    return result;
}

inline ThreadPool::~ThreadPool() {
    quit.test_and_set();
    cond.notify_all();
    for(std::thread &worker: workers) {
        worker.join();
    }
}

#endif /* end of include guard: __THREADPOOL_H__ */
