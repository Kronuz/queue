# Changelog

All notable changes to `queue`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-11-01

### Added

- Four `test/liveness.cc` cases for wake-ups the existing ones did not reach (30 cases
  now): a pop with a deadline and a non-blocking pop each wake a parked producer; destroying
  a running `Queue` wakes the producers of another `Queue` on its state; `clear()` of one slot
  with two producers parked wakes one and leaves the other parked until the next pop.
- `test/race.cc` (CTest `queue_race`): `end()` and `finish()` run between a waiter's last look
  at its predicate and its block, for a consumer and a producer. `QUEUE_TEST_HOOK(point)`,
  empty unless a test defines it, marks the spot, and the untimed waits are written as
  `while (!pred()) { hook; wait(lk); }`, which is what `wait(lk, pred)` does. Taking the mutex
  in `_wake_all` is what keeps the broadcast out of that window; removing it was caught only
  by a race in `test/concurrent.cc` that a run on a quiet machine can miss.
- `tools/mutants.py`: 24 edits of `queue.h` (a dropped or misplaced notify, an uncounted wait,
  a wake not handed on, a count not given back, a shared consumer condition variable), each of
  which a test must fail on. It replaces the hand-copied mutant headers of the review. Each
  new case has a mutant that fails it. `LIVENESS_WATCHDOG_MS` shortens the 60 s a
  `test/liveness.cc` run may go without finishing a case, for the mutation run.
- `QUEUE_BUILD_BENCH` (off by default). `benchmark/bench.cc` fetches
  cameron314/concurrentqueue at commit `1e2def4` for the bench target alone; the copy under
  `third_party/` is removed.

### Documented

- A `Queue` is destroyed after the threads blocked in its calls have returned: the destructor
  wakes the producers of other `Queue`s on its state, not its own callers. A moved-from `Queue`
  can be destroyed or assigned to and nothing else.

### Fixed

- Move assignment dropped the target's items without taking them out of its state's count,
  so that state kept their slots for good and its other `Queue`s' producers could sleep
  with room. It gives them back and wakes those producers, as the destructor does. It also
  destroyed the target's old state while holding that state's mutex and then unlocked it,
  and on self-assignment or between two `Queue`s on one state it passed one mutex to
  `std::lock` twice: undefined behavior both, which glibc runs without an ASan or TSan
  report. It keeps the old state alive until the unlock and takes one lock for one state.
  `test/differential.cc` lists move assignment as the one class whose results differ from
  the frozen copy (`count()` on the old state read high by the target's size there);
  `test/test.cc` and a `test/liveness.cc` case cover the rest.

## 2026-10-31

### Fixed

Untimed waits have had no periodic re-check since 2026-10-30, so a state change that let
a parked thread go but woke nobody, or woke a thread that could not use it, left that
thread asleep for good. Before 2026-10-30 each of these recovered within the one-second
re-check. Every change that can release a waiter now wakes one that acts on it
(ARCHITECTURE.md, "Wake-ups"):

- With two `Queue`s on one `QueueState`, a push on one could wake the other's consumer,
  which went back to sleep, and the pushed item waited with its own consumer asleep. The
  consumers' condition variable and waiter count are per `Queue` (`_pop_cond`,
  `_pop_waiters`); `QueueState` no longer has them.
- `clear()` freed every slot and woke one parked producer, and none when its own `Queue`
  was ending or finished. It wakes all of them (one when it freed one slot), whatever its
  own flags: producers of other `Queue`s on the state can use the room.
- `QueueSet::erase` freed a slot and woke nobody. It wakes one producer.
- A pop that left the size at or above `threshold` woke no producer, so a producer parked
  at the hard limit slept with room. A pop wakes one producer whenever it leaves the count
  below the hard limit, and `threshold` no longer affects waking.
- With the soft limit below the hard limit, a pop woke one of the producers parked at the
  hard limit; it refused at the soft limit and the others slept on. A producer that waited
  and then does not push (soft limit, shutdown, or an exception from the insert) wakes the
  next one, and so does a consumer whose take throws.
- Destroying a finished `Queue` on a shared state gave its slots back without waking the
  other `Queue`s' producers.
- `QueueSet::clear()` did not compile: it passed a `lock_guard` to a `unique_lock&`.

### Added

- `test/liveness.cc` (CTest `queue_liveness`, `queue_liveness_stress`): one case per state
  change that can release a parked waiter. Each parks threads, confirms them parked
  through the waiter counts, makes the change, and fails at a 10 s deadline if one is still
  asleep; a watchdog fails a case that cannot release its threads. The stress mode runs
  random rounds of producers, consumers and clears on one `Queue` or two sharing a state,
  and checks at quiescence that no item is left with its consumers asleep. Nine of the 25
  cases fail on the header before this change (the `QueueSet::clear()` case did not compile
  there), and so does the stress mode; the other cases cover wake-ups that already worked.

### Changed

- The four `_impl` bodies are force-inlined (`QUEUE_HOT`) like the operations that call
  them, and the waits stay inside them. Grown by the wake-up hand-on, `_push_back_impl` was
  left out of line by Apple clang, and with the waits in separate out-of-line functions the
  queue lost 15-20% at 4x4 and 8x8 on x86. Throughput against the inline fast path below
  (`44211ce`), interleaved, 1.5M items, the A/A row a second copy of `44211ce`; both
  binaries from Apple clang 17 (GitHub macos-15 runner):

  | ratio to `44211ce` | 1x1 | 2x2 | 4x4 | 8x8 |
  |---|---:|---:|---:|---:|
  | x86-64 i9-9980HK, 12 rounds | 1.03 | 1.01 | 0.98 | 0.99 |
  | x86-64 i9-9980HK, A/A | 0.99 | 0.99 | 0.95 | 0.99 |
  | arm64 M4 Pro, 15 rounds | 1.00 | 0.98 | 1.00 | 1.09 |
  | arm64 M4 Pro, A/A | 1.01 | 0.97 | 1.01 | 1.07 |
- `push_back` and `push_front` decide the common case (running, below both limits)
  inline, in `_push_room()`, and call the out-of-line `_push_wait` only to wait or to
  refuse. The four hot operations (`push_back`, `push_front`, `pop_back`, `pop_front`) are
  force-inlined (`QUEUE_HOT`). Before, each was an out-of-line call that held the
  `unique_lock` in memory across a second call; on an Intel i9-9980HK with Apple clang that
  ran at 0.92x, 0.71x, 0.57x and 0.55x of a mutex+condvar+deque baseline at 1x1, 2x2, 4x4
  and 8x8 producers and consumers. Behavior is unchanged: `_push_room()` is the case where
  `_push_wait` returns true without waiting.
- `_cnt` is stored with release ordering instead of sequentially consistent ordering. The
  store is an `xchg` on x86, a full barrier inside the critical section, and a plain `mov`
  with release; arm64 compiles both to the same `stlr`.

  Million items/sec ratio to the baseline (>1 is faster), interleaved, 21 rounds, 1.5M
  items, A/A within 1.5%:

  | | 1x1 | 2x2 | 4x4 | 8x8 |
  |---|---:|---:|---:|---:|
  | x86-64 i9-9980HK, Apple clang, before | 0.92 | 0.71 | 0.57 | 0.55 |
  | x86-64 i9-9980HK, Apple clang, after | 1.16 | 1.25 | 1.16 | 1.17 |
  | x86-64 i9-9980HK, gcc 16, before | 1.05 | 1.17 | 1.11 | 1.07 |
  | x86-64 i9-9980HK, gcc 16, after | 1.31 | 1.47 | 1.29 | 1.25 |
  | arm64 M4 Pro, Apple clang, before | 1.28 | 1.05 | 1.25 | 1.65 |
  | arm64 M4 Pro, Apple clang, after | 1.66 | 1.24 | 1.38 | 1.96 |

  On a Linux x86-64 VM (g++ 11, four pinned cores) the ratio went from 0.77, 1.01, 1.03
  and 1.06 to 0.93, 1.05, 1.08 and 1.24.

### Added

- `test/differential.cc` and `test/queue_frozen.h` (a frozen copy of `queue.h` before this
  change): the same seeded single-threaded operation sequences run on both, over every limit
  shape (unbounded, hard, soft below and above the hard limit, zero), both ends, `force`, the
  non-blocking and timed calls, `clear`, `end` and `finish`; every return value, popped value,
  `size()`, `count()` and `front()` must match.
- `test/concurrent.cc` runs an all-untimed scenario under a watchdog that fails
  instead of hanging. Timed poppers masked a missed wake-up for an uncounted
  untimed waiter (mutant M2) (`47d4769`).

## 2026-10-30

### Changed

- A push or pop calls `notify_one` only when the matching waiter count is
  non-zero. The state counts blocked poppers and pushers under the mutex, so an
  unbounded queue never signals `_push_cond` (`b1b38b0`).
- A blocking wait with no timeout uses `wait(lk, pred)`. It used to loop on
  `wait_for(lk, 1s, pred)`, a one-second re-check that guarded against a lost
  wake-up. A wait whose predicate already holds skips the wait and the clock
  read.
- `end()` and `finish()` lock and unlock the mutex before notifying. This closes
  the lost wake-up window the one-second poll used to cover.
- `_cnt` is written with load plus store instead of a locked read-modify-write,
  since it changes only under the mutex.
- Blocking, timeout and shutdown semantics are unchanged.

### Added

- A shutdown-race loop in `test/concurrent.cc`. TSan (clang, Linux x86_64) is
  clean on it.

### Performance

Throughput, million items/sec, Apple arm64, best of 3:

| | 1x1 | 2x2 | 4x4 | 8x8 |
|---|---|---|---|---|
| `Queue` before | 12.8 | 5.1 | 6.5 | 9.4 |
| `Queue` after | 44.8 | 15.2 | 19.5 | 30.6 |
| mutex+condvar+deque | 33.5 | 15.4 | 14.7 | 19.3 |
| moodycamel Blocking | 8.5 | 8.5 | 6.1 | 5.0 |

## Before 2026-10-30

### Removed

- The failure-path `notify_all()` on both condition variables (the
  `FIXME: This block shouldn't be needed!` blocks inherited from Xapiand,
  `bc16081`). A failed push or pop changes no state, so no waiter could make
  progress; shutdown wake-ups come from `end()` and `finish()`. Checked under
  ASan and TSan with exactly-once accounting.

### Performance

The first benchmark ran on the code before `bc16081`: 2,000,000 items, best of
3, Homebrew LLVM 22 `-O3 -DNDEBUG`, Apple Silicon. Million items/sec:

| impl | 1x1 | 2x2 | 4x4 | 8x8 |
| --- | --- | --- | --- | --- |
| `queue::Queue<int>` | 12-15 | ~7.4 | ~7.1 | ~8.8 |
| mutex+condvar+deque (baseline) | 36-38 | ~22 | ~17 | ~17 |
| moodycamel::BlockingConcurrentQueue | ~12 | ~10.5 | ~7.5 | ~8.0 |

At the time the plain mutex queue was about 2x faster under contention, and
`Queue` tied moodycamel. The analysis blamed the extra signalling: a separate
atomic count, two condition variables, and the failure-path `notify_all()` on
both. The 2026-10-30 wake-up change and `bc16081` remove that gap.
