# queue

A thread-safe, bounded, blocking, **double-ended** MPMC queue. Header-only C++20.

## What it is

`queue::Queue<T>` is a multi-producer / multi-consumer queue built on a
`std::mutex`, two `std::condition_variable`s, and a `std::deque`. It is
double-ended (push and pop at either end), bounded (a push blocks when the queue
is full), and blocking with per-operation timeouts: every push/pop takes an
optional `timeout` in seconds, where a negative value blocks forever, `0` is
non-blocking, and a positive value waits up to that long before giving up. It
also has a two-stage graceful shutdown: `end()` stops new pushes but lets
consumers drain what is left, and `finish()` wakes every waiter and rejects all
further ops.

That feature set is the whole point. It is not trying to be the fastest queue;
it is the queue you reach for when you want a blocking channel with timeouts and
a clean shutdown, and you are happy to pay a mutex for it.

## How it works

A `Queue` owns a `Container` (a `std::deque<T>` by default) and shares a
`QueueState`: one `std::mutex`, a `_push_cond` and a `_pop_cond` condition
variable, and an atomic count (`queue.h:40`). The two condition variables let a
push wait for room and a pop wait for an item independently. Every mutating
operation takes the mutex, does its work on the deque, releases the lock, and
then notifies the appropriate condition variable.

The bound is three numbers on the state: `_hard_limit` (a push blocks while
`count >= hard_limit`), `_soft_limit` (after waiting, a push only proceeds while
`count < soft_limit`), and `_threshold` (a pop only signals a waiting producer
when the post-pop size drops below it, to avoid a notify storm on a queue that
stays near full). All three default to `-1` (i.e. `SIZE_MAX`), which is the
"effectively unbounded" configuration; pass `Queue(hard_limit)` to cap it.

Shutdown rides on two atomics, `_ending` and `_finished` (`queue.h:65`). The
wait predicates check both, so flipping either one and notifying wakes every
parked thread. `finish()` is the hard stop (push and pop both return false);
`end()` is the soft one (pushes stop, pops keep succeeding until the queue
drains).

## When to use it / when not

Use it when you want a blocking hand-off channel and you need at least one of:
both-ended access (a work-stealing or LIFO-priority consumer), per-operation
timeouts (a consumer that polls with a deadline), bounded capacity for
back-pressure, or a clean two-stage shutdown that wakes blocked threads. This is
the queue Xapiand uses for exactly those reasons.

Do not reach for it as a raw throughput pipe. If all you need is a fast,
single-ended, push-and-pop MPMC channel and you do not care about timeouts,
double-ended access, or graceful finish, a lock-free queue
(`moodycamel::ConcurrentQueue`) or even a one-line mutex+deque will move more
items per second. See [Stability & performance](#stability--performance) for the
honest numbers.

## API

`Queue<T, Container = std::deque<T>>`. Every push and pop takes
`double timeout = -1.0`: **`< 0` blocks forever, `0` is non-blocking (returns
immediately), `> 0` waits up to that many seconds**, then returns `false` on
timeout.

| call | does |
| --- | --- |
| `push_back(elem, timeout=-1, force=false)` | push to the back; blocks if full. `force` skips the wait/bound check. Returns false if it could not push (full+timeout, or finished/ending). |
| `push_front(elem, timeout=-1, force=false)` | push to the front, same rules. |
| `push(elem, timeout=-1)` | alias for `push_back`. |
| `pop_front(out, timeout=-1)` | pop from the front (FIFO with `push_back`); blocks if empty. Returns false on timeout or shutdown. |
| `pop_back(out, timeout=-1)` | pop from the back (LIFO with `push_back`). |
| `pop(out, timeout=-1)` | alias for `pop_front`. |
| `front()` / `front(out)` | peek the front without consuming. The `out` overload returns false on an empty queue. |
| `clear()` | drop every item; wakes a producer waiting on a full queue. |
| `empty()` / `empty(f)` | emptiness; the `f` overload runs a callback under the lock. |
| `size()` | item count, taken under the lock (`_items_queue.size()`). |
| `count()` | item count, read lock-free from the atomic (`_state->_cnt`). |
| `end()` | stop new pushes, let consumers drain; wakes all waiters. |
| `finish()` | hard stop: wake all waiters, reject every further push and pop. |

The constructor is `Queue(hard_limit=-1, soft_limit=-1, threshold=-1)`. There is
also a `QueueSet<Key>` in the header: a `Queue` with unique values backed by a
`std::list` plus a hash map, with duplicate-handling policies
(`update` / `leave` / `renew`). It rides on the same `QueueState` machinery.

## Install

Header-only. Drop `queue.h` on your include path:

```cpp
#include "queue.h"
```

Requires C++20 and a `Threads` link. With CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  queue
  GIT_REPOSITORY https://github.com/Kronuz/queue.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(queue)

target_link_libraries(your_target PRIVATE queue::queue)
```

The `queue` target is an `INTERFACE` library that adds the include path,
requests `cxx_std_20`, and links `Threads::Threads` (`CMakeLists.txt`).

## Usage

```cpp
#include "queue.h"

queue::Queue<int> q(/*hard_limit*/ 1024);   // bounded at 1024 items

// Producer: block until there is room.
q.push_back(42);

// Consumer: block until an item arrives.
int v = 0;
if (q.pop_front(v)) {
    // got v
}

// Poll with a 50 ms deadline instead of blocking forever.
if (q.pop_front(v, 0.05)) {
    // got an item within 50 ms
}

// Non-blocking try-pop.
if (q.pop_front(v, 0.0)) {
    // had one ready
}

// Shutdown: stop producers, let consumers drain, then hard-stop.
q.end();      // pushes now fail; pops keep draining
// ... consumers finish what is left ...
q.finish();   // any remaining blocked thread wakes and returns false
```

## Build & test

Header-only. To build and run the tests and the self-checking example:

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

`ctest` runs three checks: `queue` (single-threaded API coverage in
`test/test.cc`), `queue_concurrent` (the N-producer / M-consumer stress test in
`test/concurrent.cc`), and `queue_work_queue` (a Xapiand-shaped bounded worker
queue example in `examples/work_queue.cc`). Or directly:

```sh
c++ -std=c++20 -I. test/test.cc -o test/test && ./test/test
c++ -std=c++20 -O2 -I. test/concurrent.cc -o test/concurrent && ./test/concurrent
c++ -std=c++20 -O2 -I. examples/work_queue.cc -o examples/work_queue && ./examples/work_queue
```

## Stability & performance

**Stability: clean.** The concurrent stress test (`test/concurrent.cc`) runs
4x4 and 8x8 producer/consumer shapes pushing a million items through unbounded,
bounded, and `finish()`-mid-flight scenarios, verifying every item is received
exactly once with a count plus an XOR checksum over the id space. Built with
Homebrew LLVM 22 and run under both sanitizers, it is clean:

- **ASan** (`-fsanitize=address`, full 1,000,000 items): no leaks, no heap
  corruption, no use-after-free, exit 0. Every scenario reconciles exactly.
- **TSan** (`-fsanitize=thread`, 200,000 items): no data races reported, exit 0.
  Every scenario reconciles exactly.

Nothing was suppressed or filtered. The mutex serializes all access to the
deque and the atomic count, so there is no shared mutable state outside the lock
to race on; TSan agrees.

**Performance: the rich feature set has a cost.** The benchmark
(`benchmark/bench.cc`) measures the single-ended MPMC path (`push_back` /
`pop_front`) of three blocking queues on one harness: this `queue::Queue<int>`
(unbounded), a ~30-line `mutex + condition_variable + std::deque` baseline, and
`moodycamel::BlockingConcurrentQueue<int>`. 2,000,000 items, best of 3 runs,
Homebrew LLVM 22 `-O3 -DNDEBUG`, Apple Silicon.

**Throughput, million items/sec (higher is better):**

| impl | 1x1 | 2x2 | 4x4 | 8x8 |
| --- | --- | --- | --- | --- |
| `queue::Queue<int>` | 12-15 | ~7.4 | ~7.1 | ~8.8 |
| mutex+condvar+deque (baseline) | **36-38** | **~22** | **~17** | **~17** |
| moodycamel::BlockingConcurrentQueue | ~12 | ~10.5 | ~7.5 | ~8.0 |

Two things stand out. First, the naive `mutex+condvar+deque` baseline is the
**fastest** option here, by roughly 2x under contention and more at one
producer/consumer. Second, `queue::Queue` lands roughly tied with moodycamel and
about half the speed of the naive baseline.

That `queue::Queue` is slower than the most naive possible mutex queue is real
and worth understanding. It does strictly more work per operation: it maintains
a separate atomic `_cnt` alongside the deque, it juggles two condition variables
instead of one, and on the failure path of every push and pop it does a
`notify_all()` on **both** condition variables (the `FIXME: This block
shouldn't be needed!` blocks in `queue.h`). Under contention that extra
signalling is the bulk of the gap. The baseline does one `notify_one` and
touches nothing else.

### Verdict

**Keep it, but note the cost.** The mutex+condvar approach is stable (ASan/TSan
clean, exact accounting under heavy contention) and correct, and for the job it
is actually used for, throughput is not the binding constraint. A blocking
channel that hands off thousands to low-millions of items per second is far past
what a logger queue, a job queue, or a debounce table needs.

Would a lock-free queue be materially better? On raw single-ended throughput,
**no** here: moodycamel ties `queue::Queue` on this machine, and a plain locked
deque beats both. The win a lock-free queue usually brings, a flat tail under
heavy producer contention, does not show up as a throughput advantage in this
hand-off benchmark. So replacing `queue::Queue` with moodycamel would not buy
speed.

More to the point, the comparison is only fair on the shared single-ended
subset. `queue::Queue` is **double-ended, has per-operation timeouts, bounded
back-pressure, and a two-stage graceful finish()**. `moodycamel`'s blocking
queue has none of those: no `pop_back`, no per-call deadline, no `end()`/`finish()`.
You cannot swap it in without losing the features that are the reason this queue
exists. If those features matter (and in Xapiand they do), this is the right
tool and its speed is adequate. If they do not, and you are on the pure
single-ended hot path chasing throughput, a simpler mutex queue or a lock-free
one is the better pick.

The one concrete improvement worth flagging in `queue.h` itself is the
double `notify_all()` on every failed push/pop (the `FIXME` blocks). It is
defensive over-signalling; tightening it would close much of the gap to the
naive baseline without changing the API. That is a change to make against
upstream Xapiand, not a divergence to introduce here.

To reproduce:

```sh
# sanitizers (Homebrew LLVM)
CXX=$(brew --prefix llvm)/bin/clang++
$CXX -std=c++20 -fsanitize=address -g -O1 -I. test/concurrent.cc -o /tmp/asan && /tmp/asan
$CXX -std=c++20 -fsanitize=thread  -g -O1 -I. test/concurrent.cc -o /tmp/tsan && /tmp/tsan 200000

# benchmark
cmake -B build -DCMAKE_CXX_COMPILER=$CXX
cmake --build build --target queue_bench && ./build/queue_bench
```

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand), where `queue.h` is
the blocking queue behind its thread pools, the async logger hand-off, and other
producer/consumer pipelines. The header is copied verbatim: it has zero local
includes (pure `std`), so no decoupling was needed. It belongs to the same
concurrency family as
[`stash`](https://github.com/Kronuz/stash) (the lock-free slot store / timer
wheel) and the scheduler and thread pool built on top of it.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
