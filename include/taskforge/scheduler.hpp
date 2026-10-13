#pragma once

#include "taskforge/task.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace taskforge {

class Scheduler {
public:
    Scheduler(std::size_t worker_count, std::size_t queue_capacity);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    template <class Function, class... Args>
        requires std::is_invocable_v<std::decay_t<Function>,
                                     std::decay_t<Args>...>
    auto submit(Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>,
                                            std::decay_t<Args>...>> {
        return submit(0, std::forward<Function>(function),
                      std::forward<Args>(args)...);
    }

    template <class Function, class... Args>
    auto submit(int priority, Function&& function, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<Function>,
                                            std::decay_t<Args>...>> {
        using Result = std::invoke_result_t<std::decay_t<Function>,
                                            std::decay_t<Args>...>;

        auto invocation = [
            callable = std::decay_t<Function>(std::forward<Function>(function)),
            arguments = std::tuple<std::decay_t<Args>...>(
                std::forward<Args>(args)...)
        ]() mutable -> Result {
            return std::apply(
                [&callable](auto&&... unpacked) -> Result {
                    return std::invoke(std::move(callable),
                                       std::forward<decltype(unpacked)>(unpacked)...);
                },
                std::move(arguments));
        };

        std::packaged_task<Result()> packaged(std::move(invocation));
        auto result = packaged.get_future();
        enqueue(detail::MoveOnlyTask(std::move(packaged)), priority);
        return result;
    }

    void shutdown();
    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] std::size_t queue_capacity() const noexcept;

private:
    void enqueue(detail::MoveOnlyTask task, int priority);
    void worker_loop() noexcept;

    const std::size_t queue_capacity_;
    std::vector<std::thread> workers_;

    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable work_available_;
    std::priority_queue<detail::TaskEntry,
                        std::vector<detail::TaskEntry>,
                        detail::TaskEntryLess> tasks_;
    std::uint64_t next_sequence_ = 0;
    bool stopping_ = false;
};

} // namespace taskforge
