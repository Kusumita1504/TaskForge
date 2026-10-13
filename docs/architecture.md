# TaskForge architecture

## Components

`Scheduler` owns a fixed vector of worker threads and a bounded
`std::priority_queue`. Each queue entry contains an integer priority, a
monotonically increasing sequence number, and a move-only task wrapper. The
wrapper owns a `std::packaged_task` through `std::unique_ptr`-based type
erasure, so submitted callables do not need to be copyable.

`submit` uses decayed owned argument storage, perfect forwarding,
`std::invoke_result_t`, `std::apply`, and `std::invoke`. It produces a future
before attempting enqueue. A successful enqueue transfers exclusive task
ownership to the queue; callers own the returned future. An exception thrown
by the callable is captured by `std::packaged_task` and observed when the
future is read.

## Synchronization and ordering

One mutex protects the queue, next sequence number, and stopping state.
Workers wait on a condition variable until work arrives or shutdown begins.
The worker removes the highest-priority/oldest equal-priority item while
holding the mutex, then releases the mutex before running user code. This
avoids executing arbitrary callbacks inside the scheduler's critical section.

Priority applies only while work is queued. It cannot affect a task that has
already been removed by a worker. Queue capacity counts waiting tasks only.
Submission is deliberately nonblocking: when the waiting queue is at
capacity, enqueue throws `std::overflow_error`.

## Shutdown and failures

Shutdown serializes concurrent shutdown calls, marks the scheduler as
stopping, and wakes all workers. Workers continue removing queued entries;
they exit only when stopping is set and the queue is empty. Shutdown then joins
the threads. Later submissions throw `std::logic_error`. Shutdown from a
worker is rejected to prevent self-join deadlock.

Callable exceptions are delivered through their futures. Task invocation is
contained inside a `noexcept` type-erased wrapper so an exception cannot escape
the worker entry point. Scheduler destruction calls shutdown.

## Benchmark interpretation

The benchmark measures elapsed completion throughput, submission-to-start
queue delay, and submission-to-completion latency using `std::chrono::steady_clock`.
It bounds outstanding futures by queue capacity to avoid measuring rejected
submissions. Work is a deterministic integer recurrence, configurable by
iteration count. These figures include submission/future overhead and are not
isolated measurements of queue operations. Benchmark output reports its
configuration, compiler, platform, and hardware concurrency; compare runs only
under equivalent build and machine conditions.
