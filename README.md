# TaskForge

TaskForge is a small C++20 concurrent job scheduler. It accepts move-capable
callables and arguments, stores up to a configured number of waiting tasks in a
priority queue, and executes them on a fixed worker pool. `submit` returns a
`std::future` for the callable's result.

## Build and run

Requirements: CMake 3.20 or newer and a C++20-capable compiler. The scheduler
has no third-party dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/taskforge_example
```

On Windows, run the executable as `build\Release\taskforge_example.exe` for a
multi-configuration generator, or `build\taskforge_example.exe` for MinGW/Make.

## Usage

```cpp
#include "taskforge/scheduler.hpp"

taskforge::Scheduler scheduler(4, 128); // workers, waiting-queue capacity
auto answer = scheduler.submit(10, [](int a, int b) { return a + b; }, 20, 22);
int value = answer.get();
scheduler.shutdown(); // optional; also called by the destructor
```

`submit(callable, args...)` uses priority zero; the
`submit(priority, callable, args...)` overload sets an explicit priority. Both
decay and own the callable and arguments, support move-only captures, and invoke
them with move semantics.
Use `std::ref`/`std::cref` when reference semantics are intentionally required.
The returned future propagates exceptions thrown by the callable. A failed task
does not terminate its worker.

## Scheduling and lifecycle semantics

- A larger integer priority runs before a smaller priority **among tasks still
  waiting in the queue**. Tasks already executing are not preempted.
- Equal-priority tasks are FIFO, ordered by successful enqueue order.
- Capacity limits queued/waiting tasks, not currently executing tasks. When
  the queue is full, submission throws `std::overflow_error` immediately;
  callers may retry or apply their own backpressure. Submission after shutdown
  throws `std::logic_error`.
- Workers sleep on a condition variable while the queue is empty. They remove
  a task under the queue mutex, then invoke it after releasing the mutex.
- Shutdown rejects later submissions, wakes workers, drains all accepted
  queued work, and joins every worker. Shutdown is idempotent. Do not invoke
  shutdown from one of the scheduler's own worker callbacks.
- The scheduler uniquely owns its worker threads and queued task wrappers.
  Submitted callable/argument state is owned by the task; reference wrappers
  remain the caller's responsibility. The scheduler must outlive its use and
  must not be destroyed by one of its own worker callbacks.

## Tests

The dependency-free test executable covers results, forwarding, move-only
state, priority/FIFO ordering, exceptions, concurrent producers, bounded queue
rejection, shutdown, and invalid configuration. Synchronization tests use
promises/futures and all future waits have a timeout.

## Benchmarks

Build the separate `taskforge_benchmark` target (included by default), then run:

```sh
./build/taskforge_benchmark --tasks 20000 --workers 1,2,4,8 --capacity 1024 --work 250
```

It prints compiler/platform, debug/release configuration, logical hardware
thread count, task count, queue capacity, workload iterations, throughput, mean
queue waiting time, and mean submission-to-completion latency. The producer
keeps at most `capacity` futures outstanding to avoid queue-full rejections.
Use multiple runs and comparable machine load for meaningful comparisons;
results depend on the compiler, build mode, operating system, scheduler,
hardware, and workload. More workers are not guaranteed to improve throughput.
No benchmark output is checked into the project.

## Sanitizers

With a compiler/runtime that supports them, configure separate builds:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

AddressSanitizer and ThreadSanitizer support varies by compiler and platform;
they may require separate builds and are not available in every Windows toolchain.
