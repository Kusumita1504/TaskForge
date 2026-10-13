#include "taskforge/scheduler.hpp"

#include <exception>
#include <stdexcept>

namespace taskforge {
namespace {

thread_local const Scheduler* active_scheduler = nullptr;

class WorkerContext {
public:
    explicit WorkerContext(const Scheduler* scheduler)
        : previous_(active_scheduler) {
        active_scheduler = scheduler;
    }

    ~WorkerContext() {
        active_scheduler = previous_;
    }

private:
    const Scheduler* previous_;
};

} // namespace

Scheduler::Scheduler(std::size_t worker_count, std::size_t queue_capacity)
    : queue_capacity_(queue_capacity) {
    if (worker_count == 0) {
        throw std::invalid_argument("worker count must be greater than zero");
    }
    if (queue_capacity == 0) {
        throw std::invalid_argument("queue capacity must be greater than zero");
    }

    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back(&Scheduler::worker_loop, this);
        }
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

Scheduler::~Scheduler() {
    shutdown();
}

void Scheduler::enqueue(detail::MoveOnlyTask task, int priority) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            throw std::logic_error("cannot submit a task after scheduler shutdown");
        }
        if (tasks_.size() >= queue_capacity_) {
            throw std::overflow_error("scheduler queue is full");
        }
        tasks_.push(detail::TaskEntry{priority, next_sequence_++, std::move(task)});
    }
    work_available_.notify_one();
}

void Scheduler::worker_loop() noexcept {
    WorkerContext context(this);
    for (;;) {
        detail::TaskEntry task;
        {
            std::unique_lock lock(mutex_);
            work_available_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            if (tasks_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }

            task = std::move(const_cast<detail::TaskEntry&>(tasks_.top()));
            tasks_.pop();
        }

        task.task.run();
    }
}

void Scheduler::shutdown() {
    if (active_scheduler == this) {
        throw std::logic_error("shutdown cannot be called from a scheduler worker");
    }

    std::lock_guard shutdown_lock(shutdown_mutex_);

    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_available_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t Scheduler::worker_count() const noexcept {
    return workers_.size();
}

std::size_t Scheduler::queue_capacity() const noexcept {
    return queue_capacity_;
}

} // namespace taskforge
