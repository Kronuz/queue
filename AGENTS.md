# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage and the benchmark numbers read `README.md`; for
what changed read `CHANGELOG.md`.
This file covers the repo layout, how to build and test (including under
sanitizers), the invariants you must not break, and the traps.

## Repo map

```
queue.h                       The library. Queue<T, Container> + QueueSet<Key>, on a shared QueueState. Header-only.
test/test.cc                  Correctness: single-threaded API coverage. CTest test `queue`.
test/concurrent.cc            Concurrent stress: N producers x M consumers, exactly-once accounting. CTest test `queue_concurrent`. The binary you run under sanitizers.
benchmark/bench.cc            Throughput: queue vs mutex+condvar baseline vs moodycamel, single-ended MPMC path.
third_party/concurrentqueue/  Vendored cameron314/concurrentqueue (header-only), for the benchmark only.
CMakeLists.txt                INTERFACE library queue + alias queue::queue; test + sanitizer + bench targets, top-level only.
LICENSE                       MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                     What it is, install, quick start, current benchmark numbers.
CHANGELOG.md                  Dated changes, with earlier benchmark tables.
docs/API.md                   The API reference.
docs/queue.d2                 The README diagram's source (docs/queue.svg is rendered from it).
ARCHITECTURE.md               Internal design: shared state, wait machinery, bound, shutdown, stability.
```

Everything ships in `queue.h`. There is no `.cc` to compile except the tests and
the benchmark.

## Build and run the tests

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Two CTest tests: `queue` (from `test/test.cc`) prints a line per API area and
ends with `all queue tests passed`; `queue_concurrent` (from
`test/concurrent.cc`) prints a per-scenario accounting table and ends with
`RESULT: OK`. Or directly:

```sh
c++ -std=c++20 -I. test/test.cc -o test/test && ./test/test
c++ -std=c++20 -O2 -I. test/concurrent.cc -o test/concurrent && ./test/concurrent
```

`test/concurrent.cc` takes an optional item count (default 1,000,000); pass a
smaller number to run it faster under a sanitizer.

## Run under sanitizers (the stability check)

Use **Homebrew LLVM 22**, not Apple clang (Apple clang's TSan is not suitable
here). The CMake targets `queue_concurrent_asan` / `queue_concurrent_tsan` build
these, or do it by hand:

```sh
CXX=$(brew --prefix llvm)/bin/clang++
$CXX -std=c++20 -fsanitize=address -g -O1 -I. test/concurrent.cc -o /tmp/asan && /tmp/asan
$CXX -std=c++20 -fsanitize=thread  -g -O1 -I. test/concurrent.cc -o /tmp/tsan && /tmp/tsan 200000
```

Both are expected to be **clean** (no leaks, no races, exit 0) with exact
exactly-once accounting in every scenario. If you touch `queue.h`, re-run both;
a regression here is the thing most worth catching. ARCHITECTURE.md's Stability
section records the expected result.

## Run the benchmark

```sh
CXX=$(brew --prefix llvm)/bin/clang++
cmake -B build -DCMAKE_CXX_COMPILER=$CXX
cmake --build build --target queue_bench && ./build/queue_bench
# or: $CXX -std=c++20 -O3 -DNDEBUG -I. -Ithird_party/concurrentqueue benchmark/bench.cc -o bench && ./bench
```

It prints a Mitems/sec table (impl x thread shape). Always build it `-O3
-DNDEBUG`; a debug build inverts the conclusions.

## Conventions

- **C++20**, header-only. Keep it that way. Anything that forces a `.cc` for the
  library itself does not belong here.
- **No external dependencies in `queue.h`.** Its includes are pure `std`. Do not
  add a third-party or Xapiand header. (`third_party/concurrentqueue` exists only
  to give the benchmark something to compare against; the library never includes
  it.)
- **Xapiand consumes this repository.** Xapiand fetches this library at `main`,
  so a change here ships to Xapiand. Record behavior changes in CHANGELOG.md.
- Double quotes in code; no em dashes in prose.

## Load-bearing invariants

These are the rules the queue's correctness rests on. Breaking one tends to
produce a lost wakeup, a deadlock, or a torn read rather than a compile error.

- **All shared mutation under `_state->_mutex`.** The container and the in-lock
  updates to `_cnt` happen only while holding the mutex. The single sanctioned
  lock-free read is `count()` reading the atomic `_cnt`; it is approximate by
  design. Do not add any other unlocked access to the container.
- **Notify after unlocking.** Every public op does `lk.unlock()` before it
  notifies a condition variable. Keep that order: notifying
  while holding the lock makes the woken thread immediately re-block.
- **The three timeout regimes.** A satisfied predicate returns at once.
  Otherwise `timeout < 0` blocks in `wait(lk, pred)`, `timeout == 0` returns
  false, and `timeout > 0` waits to a deadline. The dispatch hinges on
  `if (timeout)` then `if (timeout > 0.0)` in `_push_wait`/`_pop_wait`.
  Changing this branch structure changes the public contract the tests pin down.
- **`end()` drains, `finish()` stops.** `_pop_wait` fails on `_ending` only when
  the queue is empty but fails on `_finished` immediately. Keep
  that asymmetry: it is what lets `end()` mean "drain" and `finish()` mean "stop
  now."
- **Count every blocked waiter.** Each blocking branch increments
  `_pop_waiters`/`_push_waiters` before the wait and decrements it after, under
  the mutex. A notifier skips the condition variable when the count is zero, so
  an uncounted waiter misses its wake-up and an untimed one hangs.
  `test/concurrent.cc`'s all-untimed scenario catches this.
- **Shutdown locks before it broadcasts.** `_wake_all()` takes and releases the
  mutex before notifying. Untimed waits have no periodic re-check, so dropping
  that empty critical section can lose the shutdown wake-up.

## Traps

- **Don't drop the `_cnt` accounting.** Every push does `++_cnt`, every pop and
  `clear` subtracts. `count()`, the bound check, and the destructor's
  `_cnt -= size` all rely on it staying in step with the container. An asymmetric
  edit desyncs the bound silently.
- **Only a successful op notifies.** A failed push or pop changes no state, so no
  waiter can make progress, and shutdown wake-ups come from `end()`/`finish()`.
  Do not add a failure-path notify.
- **`force` bypasses the bound.** `push_*(..., force=true)` skips the wait and
  the capacity check entirely, so it can push past `_hard_limit`. That is by
  design; just know it sidesteps back-pressure.
- **Always extend the tests.** A behavioral change should grow an assertion in
  `test/test.cc` and, if it touches the concurrent path, survive
  `test/concurrent.cc` under both sanitizers.

## Standalone vs. Xapiand

`queue.h` comes from [Xapiand](https://github.com/Kronuz/Xapiand), where it backs
the thread pools, the async logger hand-off, and other producer/consumer
pipelines. It has zero local includes (pure `std`), so unlike `stash` it needs
no `log.h` replacement and no trace hooks. It belongs to the same concurrency family as
[`stash`](https://github.com/Kronuz/stash) and the scheduler/thread-pool built
on it.
