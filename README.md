# queue

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/queue/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/queue/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Thread-safe bounded blocking deque for C++20: push and pop at either end, per-call timeouts,
two-stage shutdown. One mutex guards a `std::deque`, and a call blocks only when it has to: a
producer at the hard limit, a consumer on an empty queue. Reach for it as the hand-off channel
between threads that need back-pressure and a clean shutdown.

## ✨ Highlights

- **Both ends.** `push_front`, `push_back`, `pop_front` and `pop_back` give FIFO, LIFO, or a
  priority lane at the front.
- **A timeout on every call.** A negative timeout blocks, `0` never blocks, and a positive one
  waits that many seconds.
- **Back-pressure.** A hard limit blocks producers when the queue is full; a soft limit below it
  refuses pushes instead of blocking them.
- **Two-stage shutdown.** `end()` stops pushes and lets consumers drain; `finish()` wakes every
  waiter and stops.
- **Shared limits.** Several `Queue`s can share one `QueueState`, so one bound covers all of them.
- **`QueueSet`.** The same queue over unique values, with a policy for duplicates (update, leave,
  or renew to the front).
- **Tested for lost wake-ups.** A stress test runs under ASan and TSan with exactly-once
  accounting, and a liveness test covers every state change that can release a waiting thread.

## 🚀 Quick start

```cpp
#include "queue.h"

#include <cstdio>
#include <thread>

int main() {
	queue::Queue<int> q(/*hard_limit*/ 1024);

	long sum = 0;
	std::thread consumer([&] {
		int v;
		while (q.pop_front(v)) { sum += v; }  // false once ended and drained
	});

	for (int i = 0; i < 100; ++i) { q.push_back(i); }  // blocks while full
	q.end();                                          // stop pushes, let it drain
	consumer.join();

	std::printf("%ld\n", sum);  // 4950
}
```

`q.pop_front(v, 0.05)` waits at most 50 ms; `q.pop_front(v, 0)` never waits.
[`examples/work_queue.cc`](examples/work_queue.cc) is a bounded worker queue with timeouts, a
priority lane and a graceful shutdown.

## 📦 Installation

Requires a C++20 compiler, threads, and CMake 3.14 or newer; no other dependencies. CI builds and
tests it on Linux (x86_64 with GCC and Clang, arm64), macOS (arm64, x86_64) and FreeBSD, plus
ASan + UBSan and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(queue
  GIT_REPOSITORY https://github.com/Kronuz/queue.git
  GIT_TAG        64ba6a4c65d770577c7101f0ed05f64a587289a4)  # or main to track the tip
FetchContent_MakeAvailable(queue)

target_link_libraries(my_app PRIVATE queue::queue)
```

```cpp
#include "queue.h"
```

Or copy `queue.h` onto your include path.

## 🎯 When to use it

A blocking channel between threads that needs timeouts, both ends, a bound, or a clean shutdown.

Not for:

- A caller that cannot take a lock, such as a signal handler or a real-time thread: every call
  locks one mutex.
- Many producers and consumers on x86-64 where raw throughput matters most: a lock-free queue
  scales further there (see Performance).

## ⚡ Performance

[`queue_bench`](benchmark/bench.cc) moves 2,000,000 integers through one queue with
`push_back` and `pop_front`: the producers hand the ids out between them, and each consumer
pops its share of the items. Every run checks that the consumers received each item exactly
once (the sum of the ids), so the queues compared deliver the same items. A cell is million
items per second, the better of two runs of best-of-3; the row says how many producers and
consumers ran (`4x4` is four of each), and "× faster" is `Queue`'s rate over the baseline's, so
a value below 1 is a loss. The comparator is the queue most code writes, a ~30-line
`std::mutex` + `std::condition_variable` + `std::deque` (`MutexDeque` in the same file),
unbounded like the `Queue` it is run against. Both run on the subset they share: a blocking
channel of ints with no bound, deadline or `pop_back`.

Apple M4 Pro (macOS 26), Apple clang 17.0.0 with `-O3 -DNDEBUG`:

| Producers x consumers | `queue::Queue<int>` | mutex + condvar + deque | `Queue` × faster |
|---|---:|---:|---:|
| 1x1 | 41.4 | 31.6 | 1.31 |
| 2x2 | 19.8 | 18.9 | 1.05 |
| 4x4 | 21.1 | 18.4 | 1.15 |
| 8x8 | 35.1 | 19.8 | 1.77 |

Intel Core i9-9980HK (macOS 15, x86-64, 8 cores / 16 threads), Apple clang 17.0.0 with
`-O3 -DNDEBUG`:

| Producers x consumers | `queue::Queue<int>` | mutex + condvar + deque | `Queue` × faster |
|---|---:|---:|---:|
| 1x1 | 9.9 | 8.2 | 1.21 |
| 2x2 | 6.7 | 5.3 | 1.26 |
| 4x4 | 5.2 | 3.9 | 1.33 |
| 8x8 | 4.1 | 3.4 | 1.21 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQUEUE_BUILD_BENCH=ON
cmake --build build --target queue_bench && ./build/queue_bench
```

`QUEUE_BUILD_BENCH` is off by default; it fetches a third-party queue for the benchmark target
only. `Queue` is 1.05 to 1.77 times the baseline on arm64 and 1.21 to 1.33 times on x86-64.

## 📚 Documentation

- [docs/API.md](docs/API.md): every call, its timeout rule, and `QueueSet`.
- [ARCHITECTURE.md](ARCHITECTURE.md): the shared state, the limits, the waits and wake-ups, and
  the shutdown model.
- [CHANGELOG.md](CHANGELOG.md): dated changes, with earlier benchmark tables.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#queue) in Kronuz C++
  Libraries.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
cmake -B build -DQUEUE_BUILD_BENCH=ON && cmake --build build --target queue_bench
```

CTest runs the API tests, a many-producer, many-consumer stress test, a differential test against
a frozen copy of the header, the liveness and race tests (a lost wake-up fails them within seconds
instead of hanging), and the work-queue example.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; a change to `queue.h` reruns the stress test under ASan and TSan.

## 📄 License

MIT. See [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand)
(Copyright © 2015-2020 Dubalu LLC), where it feeds the thread pools and the logger.
