# Changelog

All notable changes to `queue`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

## 2026-10-31

### Added

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
