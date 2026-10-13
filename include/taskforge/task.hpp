#pragma once

#include <cstdint>
#include <memory>
#include <future>
#include <utility>

namespace taskforge::detail {

class TaskConcept {
public:
    virtual ~TaskConcept() = default;
    virtual void run() noexcept = 0;
};

template <class Result>
class PackagedTaskModel final : public TaskConcept {
public:
    explicit PackagedTaskModel(std::packaged_task<Result()>&& task)
        : task_(std::move(task)) {}

    void run() noexcept override {
        try {
            task_();
        } catch (...) {
            // User exceptions are stored in the packaged task's future.
        }
    }

private:
    std::packaged_task<Result()> task_;
};

class MoveOnlyTask {
public:
    MoveOnlyTask() = default;

    template <class Result>
    explicit MoveOnlyTask(std::packaged_task<Result()>&& task)
        : task_(std::make_unique<PackagedTaskModel<Result>>(std::move(task))) {}

    MoveOnlyTask(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask& operator=(MoveOnlyTask&&) noexcept = default;
    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    void run() noexcept {
        if (task_) {
            task_->run();
        }
    }

private:
    std::unique_ptr<TaskConcept> task_;
};

struct TaskEntry {
    int priority;
    std::uint64_t sequence;
    MoveOnlyTask task;
};

struct TaskEntryLess {
    bool operator()(const TaskEntry& left, const TaskEntry& right) const noexcept {
        if (left.priority != right.priority) {
            return left.priority < right.priority;
        }
        return left.sequence > right.sequence;
    }
};

} // namespace taskforge::detail
