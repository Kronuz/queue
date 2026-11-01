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

`benchmark/bench.cc` times the single-ended MPMC path (`push_back` / `pop_front`) with 2,000,000
items, best of 3, in million items per second, two runs each. Columns are producers x consumers.
The baseline is a `std::mutex` + `std::condition_variable` + `std::deque`; moodycamel is
cameron314/concurrentqueue at commit `1e2def4`. Built with `-O3 -DNDEBUG`.

Apple M4 Pro (arm64, Apple clang 17.0.0):

| | 1x1 | 2x2 | 4x4 | 8x8 |
|---|---|---|---|---|
| `queue::Queue<int>` | 41.4, 39.4 | 18.0, 19.8 | 20.0, 21.1 | 35.1, 34.0 |
| mutex+condvar+deque | 31.3, 31.6 | 18.9, 17.9 | 18.2, 18.4 | 18.5, 19.8 |
| moodycamel `BlockingConcurrentQueue` | 10.0, 9.6 | 5.8, 5.5 | 5.4, 6.0 | 5.5, 5.3 |

Intel Core i9-9980HK (x86_64, 8 cores / 16 threads, Apple clang 17.0.0):

| | 1x1 | 2x2 | 4x4 | 8x8 |
|---|---|---|---|---|
| `queue::Queue<int>` | 9.9, 9.0 | 6.7, 6.1 | 5.2, 4.7 | 3.7, 4.1 |
| mutex+condvar+deque | 8.2, 7.6 | 5.3, 5.0 | 3.9, 3.8 | 3.3, 3.4 |
| moodycamel `BlockingConcurrentQueue` | 8.5, 8.6 | 6.2, 6.4 | 10.0, 9.7 | 14.1, 13.3 |

On arm64 `Queue` runs at 0.95x to 1.9x the baseline and 3.1x to 6.4x moodycamel. On x86-64 it
runs at 1.1x to 1.3x the baseline and level with moodycamel at 1x1 and 2x2; at 4x4 and 8x8
moodycamel is 1.9x to 3.8x ahead, because its lock-free design scales with the thread count and
every `Queue` call takes one mutex. The comparison covers only the shared subset: moodycamel has
no `pop_back`, no per-call deadline, and no `end()` or `finish()`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQUEUE_BUILD_BENCH=ON
cmake --build build --target queue_bench && ./build/queue_bench
```

`QUEUE_BUILD_BENCH` is off by default; it fetches the comparator for the benchmark target only.

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
