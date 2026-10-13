#include "taskforge/scheduler.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

template <class Future>
decltype(auto) get_before_timeout(Future& future) {
    if (future.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("future did not become ready within five seconds");
    }
    return future.get();
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_value_return_types() {
    taskforge::Scheduler scheduler(2, 8);
    auto integer = scheduler.submit([] { return 42; });
    auto text = scheduler.submit(0, [] { return std::string("ready"); });
    expect(get_before_timeout(integer) == 42, "integer result mismatch");
    expect(get_before_timeout(text) == "ready", "string result mismatch");
}

void test_void_task() {
    taskforge::Scheduler scheduler(1, 4);
    std::atomic<int> result{0};
    auto task = scheduler.submit(0, [&result] { result.store(7); });
    get_before_timeout(task);
    expect(result.load() == 7, "void task did not execute");
}

void test_arguments_and_forwarding() {
    taskforge::Scheduler scheduler(1, 4);
    auto sum = scheduler.submit(0, [](int left, int right) {
        return left + right;
    }, 12, 30);
    auto moved_argument = scheduler.submit(
        0,
        [](std::unique_ptr<int> value) { return *value; },
        std::make_unique<int>(19));
    expect(get_before_timeout(sum) == 42, "callable arguments mismatch");
    expect(get_before_timeout(moved_argument) == 19, "move-only argument mismatch");
}

void test_move_only_capture() {
    taskforge::Scheduler scheduler(1, 4);
    auto task = scheduler.submit(0, [value = std::make_unique<int>(23)] {
        return *value;
    });
    expect(get_before_timeout(task) == 23, "move-only capture mismatch");
}

void test_priority_ordering() {
    taskforge::Scheduler scheduler(1, 8);
    std::promise<void> entered;
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    auto blocker = scheduler.submit(0, [&] {
        entered.set_value();
        (void)release_signal.wait_for(5s);
    });
    expect(entered.get_future().wait_for(5s) == std::future_status::ready,
           "blocking task did not start");

    std::mutex results_mutex;
    std::vector<int> order;
    auto low = scheduler.submit(1, [&] {
        std::lock_guard lock(results_mutex);
        order.push_back(1);
    });
    auto high = scheduler.submit(10, [&] {
        std::lock_guard lock(results_mutex);
        order.push_back(10);
    });
    auto medium = scheduler.submit(5, [&] {
        std::lock_guard lock(results_mutex);
        order.push_back(5);
    });
    release.set_value();
    get_before_timeout(blocker);
    get_before_timeout(low);
    get_before_timeout(high);
    get_before_timeout(medium);
    expect(order == std::vector<int>({10, 5, 1}),
           "higher priority queued tasks must execute first");
}

void test_fifo_for_equal_priorities() {
    taskforge::Scheduler scheduler(1, 8);
    std::promise<void> entered;
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    auto blocker = scheduler.submit(0, [&] {
        entered.set_value();
        (void)release_signal.wait_for(5s);
    });
    expect(entered.get_future().wait_for(5s) == std::future_status::ready,
           "blocking task did not start");

    std::mutex results_mutex;
    std::vector<int> order;
    std::vector<std::future<void>> results;
    for (int index = 0; index < 5; ++index) {
        results.push_back(scheduler.submit(3, [&order, &results_mutex, index] {
            std::lock_guard lock(results_mutex);
            order.push_back(index);
        }));
    }
    release.set_value();
    get_before_timeout(blocker);
    for (auto& result : results) {
        get_before_timeout(result);
    }
    expect(order == std::vector<int>({0, 1, 2, 3, 4}),
           "equal-priority tasks must preserve FIFO order");
}

void test_exception_propagation_and_worker_survival() {
    taskforge::Scheduler scheduler(1, 4);
    auto failed = scheduler.submit(0, []() -> int {
        throw std::runtime_error("task failure");
    });
    auto subsequent = scheduler.submit(0, [] { return 71; });
    bool saw_expected_exception = false;
    try {
        (void)get_before_timeout(failed);
    } catch (const std::runtime_error& error) {
        saw_expected_exception = std::string(error.what()) == "task failure";
    }
    expect(saw_expected_exception, "task exception did not reach its future");
    expect(get_before_timeout(subsequent) == 71,
           "worker did not survive a failed task");
}

void test_worker_cannot_shutdown_its_own_scheduler() {
    taskforge::Scheduler scheduler(1, 4);
    auto attempted_shutdown = scheduler.submit(0, [&scheduler] {
        scheduler.shutdown();
    });
    bool rejected = false;
    try {
        get_before_timeout(attempted_shutdown);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, "worker-initiated shutdown must be rejected");
    auto subsequent = scheduler.submit(0, [] { return 8; });
    expect(get_before_timeout(subsequent) == 8,
           "worker did not survive a rejected shutdown call");
}

void test_concurrent_producers() {
    constexpr int producer_count = 4;
    constexpr int tasks_per_producer = 75;
    taskforge::Scheduler scheduler(4, producer_count * tasks_per_producer);
    std::barrier producer_start(producer_count);
    std::vector<std::vector<std::future<int>>> results(producer_count);
    std::vector<std::thread> producers;
    for (int producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            producer_start.arrive_and_wait();
            auto& producer_results = results[producer];
            producer_results.reserve(tasks_per_producer);
            for (int index = 0; index < tasks_per_producer; ++index) {
                producer_results.push_back(scheduler.submit(
                    index % 3, [producer, index] {
                        return producer * tasks_per_producer + index;
                    }));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    for (int producer = 0; producer < producer_count; ++producer) {
        for (int index = 0; index < tasks_per_producer; ++index) {
            expect(get_before_timeout(results[producer][index]) ==
                       producer * tasks_per_producer + index,
                   "concurrent producer result mismatch");
        }
    }
}

void test_queue_capacity_rejection() {
    taskforge::Scheduler scheduler(1, 1);
    std::promise<void> entered;
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    auto blocker = scheduler.submit(0, [&] {
        entered.set_value();
        (void)release_signal.wait_for(5s);
    });
    expect(entered.get_future().wait_for(5s) == std::future_status::ready,
           "blocking task did not start");
    auto queued = scheduler.submit(0, [] { return 2; });

    bool rejected = false;
    try {
        (void)scheduler.submit(0, [] { return 3; });
    } catch (const std::overflow_error&) {
        rejected = true;
    }
    release.set_value();
    get_before_timeout(blocker);
    expect(get_before_timeout(queued) == 2, "accepted queued task did not run");
    expect(rejected, "submission to a full queue was not rejected");
}

void test_graceful_shutdown_drains_and_rejects_new_work() {
    taskforge::Scheduler scheduler(1, 4);
    std::promise<void> entered;
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    auto blocker = scheduler.submit(0, [&] {
        entered.set_value();
        (void)release_signal.wait_for(5s);
    });
    expect(entered.get_future().wait_for(5s) == std::future_status::ready,
           "blocking task did not start");
    std::atomic<int> completed{0};
    std::vector<std::future<void>> accepted;
    for (int index = 0; index < 3; ++index) {
        accepted.push_back(scheduler.submit(0, [&completed] {
            completed.fetch_add(1);
        }));
    }

    std::promise<void> shutdown_started;
    std::thread shutdown_thread([&] {
        shutdown_started.set_value();
        scheduler.shutdown();
    });
    expect(shutdown_started.get_future().wait_for(5s) == std::future_status::ready,
           "shutdown thread did not start");
    release.set_value();
    get_before_timeout(blocker);
    for (auto& task : accepted) {
        get_before_timeout(task);
    }
    shutdown_thread.join();
    scheduler.shutdown();
    expect(completed.load() == 3, "shutdown did not drain accepted tasks");

    bool rejected = false;
    try {
        (void)scheduler.submit(0, [] {});
    } catch (const std::logic_error&) {
        rejected = true;
    }
    expect(rejected, "submission after shutdown was not rejected");
}

void test_invalid_configuration() {
    bool rejected_workers = false;
    bool rejected_capacity = false;
    try {
        taskforge::Scheduler scheduler(0, 1);
    } catch (const std::invalid_argument&) {
        rejected_workers = true;
    }
    try {
        taskforge::Scheduler scheduler(1, 0);
    } catch (const std::invalid_argument&) {
        rejected_capacity = true;
    }
    expect(rejected_workers, "zero workers must be rejected");
    expect(rejected_capacity, "zero queue capacity must be rejected");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"integer and string results", test_value_return_types},
        {"void task", test_void_task},
        {"arguments and forwarding", test_arguments_and_forwarding},
        {"move-only capture", test_move_only_capture},
        {"priority ordering", test_priority_ordering},
        {"equal-priority FIFO", test_fifo_for_equal_priorities},
        {"exceptions and worker survival", test_exception_propagation_and_worker_survival},
        {"worker shutdown rejection", test_worker_cannot_shutdown_its_own_scheduler},
        {"concurrent producers", test_concurrent_producers},
        {"bounded queue rejection", test_queue_capacity_rejection},
        {"graceful shutdown", test_graceful_shutdown_drains_and_rejects_new_work},
        {"invalid configuration", test_invalid_configuration},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
        }
    }

    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
