#include "taskforge/scheduler.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Settings {
    std::size_t tasks = 20'000;
    std::size_t capacity = 1024;
    std::size_t workload = 250;
    std::vector<std::size_t> workers{1, 2, 4};
};

std::size_t parse_size(std::string_view value, const char* option) {
    std::size_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    if (parsed == 0) {
        throw std::invalid_argument(std::string(option) + " must be greater than zero");
    }
    return parsed;
}

std::vector<std::size_t> parse_workers(const std::string& value) {
    std::vector<std::size_t> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto comma = value.find(',', begin);
        const auto end = comma == std::string::npos ? value.size() : comma;
        if (end == begin) {
            throw std::invalid_argument("--workers contains an empty count");
        }
        result.push_back(parse_size(std::string_view(value).substr(begin, end - begin),
                                    "--workers"));
        if (comma == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (result.empty()) {
        throw std::invalid_argument("--workers requires at least one count");
    }
    return result;
}

Settings parse_settings(int argc, char** argv) {
    Settings settings;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--help") {
            std::cout << "Usage: taskforge_benchmark [--tasks N] [--workers N,N,...] "
                         "[--capacity N] [--work N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = argv[++index];
        if (option == "--tasks") {
            settings.tasks = parse_size(value, "--tasks");
        } else if (option == "--workers") {
            settings.workers = parse_workers(value);
        } else if (option == "--capacity") {
            settings.capacity = parse_size(value, "--capacity");
        } else if (option == "--work") {
            settings.workload = parse_size(value, "--work");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    return settings;
}

void print_environment(const Settings& settings) {
#if defined(_MSC_VER)
    std::cout << "compiler=MSVC " << _MSC_VER;
#elif defined(__clang__)
    std::cout << "compiler=Clang " << __clang_version__;
#elif defined(__GNUC__)
    std::cout << "compiler=GCC " << __VERSION__;
#else
    std::cout << "compiler=unknown";
#endif
#if defined(_WIN32)
    std::cout << " platform=Windows";
#elif defined(__linux__)
    std::cout << " platform=Linux";
#elif defined(__APPLE__)
    std::cout << " platform=macOS";
#endif
#ifdef NDEBUG
    std::cout << " build=Release";
#else
    std::cout << " build=Debug";
#endif
    std::cout << " logical_hardware_threads=" << std::thread::hardware_concurrency()
              << " task_count=" << settings.tasks
              << " queue_capacity=" << settings.capacity
              << " workload_iterations=" << settings.workload << '\n';
}

void run_benchmark(std::size_t worker_count, const Settings& settings) {
    taskforge::Scheduler scheduler(worker_count, settings.capacity);
    std::vector<Nanoseconds> queue_wait(settings.tasks);
    std::vector<Nanoseconds> end_to_end(settings.tasks);
    std::deque<std::future<std::uint64_t>> pending;
    const auto benchmark_start = Clock::now();

    for (std::size_t index = 0; index < settings.tasks; ++index) {
        const auto submitted_at = Clock::now();
        pending.push_back(scheduler.submit(
            static_cast<int>(index % 5),
            [&, index, submitted_at, workload = settings.workload] {
                const auto started_at = Clock::now();
                queue_wait[index] =
                    std::chrono::duration_cast<Nanoseconds>(started_at - submitted_at);
                std::uint64_t value = index + 1;
                for (std::size_t iteration = 0; iteration < workload; ++iteration) {
                    value = value * 1'664'525U + iteration + 1'013'904'223U;
                }
                const auto completed_at = Clock::now();
                end_to_end[index] =
                    std::chrono::duration_cast<Nanoseconds>(completed_at - submitted_at);
                return value;
            }));

        if (pending.size() >= settings.capacity) {
            (void)pending.front().get();
            pending.pop_front();
        }
    }
    for (auto& task : pending) {
        (void)task.get();
    }
    scheduler.shutdown();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - benchmark_start);

    std::uint64_t total_queue_wait = 0;
    std::uint64_t total_latency = 0;
    for (std::size_t index = 0; index < settings.tasks; ++index) {
        total_queue_wait += static_cast<std::uint64_t>(queue_wait[index].count());
        total_latency += static_cast<std::uint64_t>(end_to_end[index].count());
    }
    const auto mean_queue_wait =
        static_cast<double>(total_queue_wait) / settings.tasks / 1'000'000.0;
    const auto mean_latency =
        static_cast<double>(total_latency) / settings.tasks / 1'000'000.0;

    std::cout << "workers=" << worker_count
              << " tasks_per_second=" << settings.tasks / elapsed.count()
              << " elapsed_seconds=" << elapsed.count()
              << " mean_queue_wait_ms=" << mean_queue_wait
              << " mean_end_to_end_latency_ms=" << mean_latency << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto settings = parse_settings(argc, argv);
        print_environment(settings);
        for (const auto workers : settings.workers) {
            run_benchmark(workers, settings);
        }
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
