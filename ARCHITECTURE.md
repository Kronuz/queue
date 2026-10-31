# Architecture

This document describes the internals of `queue`: the shared state, the wait
machinery, the bound, the shutdown protocol, and the trade-offs baked in. All
file references point at `queue.h` unless noted otherwise.

## Overview

`queue::Queue<T, Container>` is a blocking MPMC queue: a `Container` (a
`std::deque<T>` by default) guarded by a single mutex, with two condition
variables so producers and consumers can wait on independent conditions. It is
double-ended, optionally bounded, and supports per-operation timeouts and a
two-stage graceful shutdown.

The design is conventional, with no lock-free code. The value is in the
feature surface: both ends, timeouts, bounds, and a two-stage finish.
Everything mutable lives behind one mutex, which makes it race-free by
construction and caps its throughput.

## Shared state: `QueueState`

`QueueState` holds everything shared between the queue and any
copies that share it:

- `_hard_limit`, `_soft_limit`, `_threshold` — the three bound parameters
  (below).
- `std::mutex _mutex` — the single lock guarding the container and the count.
  It is `mutable` so the `const` accessors (`empty`, `size`) can take it.
- `std::condition_variable _pop_cond` — consumers wait here for an item;
  producers and `clear` notify it.
- `std::condition_variable _push_cond` — producers wait here for room; consumers
  notify it when the queue drains below `_threshold`.
- `std::atomic_size_t _cnt` — the live item count, kept alongside the container
  so `count()` can be read without taking the lock. It changes only under the
  mutex, so `_cnt_add` and `_cnt_sub` use a relaxed load plus a store instead of
  a locked read-modify-write.
- `size_t _pop_waiters`, `_push_waiters` — the number of threads blocked in a
  pop or push wait. They change only under the mutex and let a notifier skip
  the condition-variable call when nobody waits.

The state is held by `std::shared_ptr`, so several `Queue`
objects can share one `QueueState`. The two-arg constructor
(`Queue(const std::shared_ptr<QueueState>&)`) is how a second
view onto the same underlying state is constructed.

Each `Queue` additionally owns its own `Container _items_queue` and two atomics,
`_ending` and `_finished`. Note that the shutdown flags are
per-`Queue`, not on the shared state, while the count and the condition
variables are shared. For the common single-`Queue` case this distinction does
not matter.

## The wait machinery

Two private helpers carry all the blocking logic; every public push/pop routes
through one of them while holding a `std::unique_lock` on the mutex.

### `_push_wait(timeout, lk)`

The predicate is "the queue is shutting down, or there is room":
`_finished || _ending || _cnt < _hard_limit`. If it already holds, the wait is
skipped, with no clock read. Otherwise the timeout argument selects one of
three behaviors:

- **`timeout == 0`**: non-blocking. Return false immediately.
- **`timeout > 0`**: compute `now + timeout` and
  `wait_until(lk, deadline, pred)`. If the deadline passes with the predicate
  still false, return false.
- **`timeout < 0`**: block until the predicate holds, with
  `wait(lk, pred)` and no clock read.

Both blocking branches increment `_push_waiters` before the wait and decrement
it after.

After the wait, if the queue is finished or ending it returns false; otherwise it returns `_cnt < _soft_limit`. With
the default limits (`hard == soft == SIZE_MAX`) the soft check is always true,
so an unbounded push always succeeds once it is not shutting down.

### `_pop_wait(timeout, lk)`

Mirror image. The predicate is "shutting down, or there is an item":
`_finished || _ending || !_items_queue.empty()`, with the same three timeout
behaviors and the same `_pop_waiters` count. After the wait it returns false if
`_finished`, or if `_ending` and the queue is empty — so an `end()`ing queue still drains its
remaining items before pops start failing, while a `finish()`ed queue fails
immediately even with items left.

## The public ops

Each public method follows the same shape (`push_back` is
representative):

1. Take a `std::unique_lock` on the mutex.
2. Call the `_*_impl` helper, which runs the wait and, on success, mutates the
   container and bumps `_cnt`.
3. Decide under the lock whether to wake anyone.
4. `lk.unlock()` explicitly.
5. Notify a condition variable if step 3 said so.

A successful push does `_pop_cond.notify_one()` when `_pop_waiters` is
non-zero. A successful pop does `_push_cond.notify_one()` when `_push_waiters`
is non-zero **and** the post-pop size is below `_threshold`, so a queue held near
its bound does not wake producers it will only re-block. An unbounded queue
never has a blocked pusher, so its pops never touch `_push_cond`.

A **failed** op (a full or empty timeout, or a shutdown) notifies nothing. It
changes no state, so no waiter can make progress. Shutdown wake-ups come from
`end()` and `finish()`.

`push_back` / `push_front` differ only in which end of the deque they touch;
likewise `pop_back` / `pop_front`. `push`/`pop` are aliases for the back/front
pair. The `force` flag on the push path skips the wait and the
bound entirely, pushing unconditionally.

## The bound

Three numbers, all on `QueueState`, all defaulting to `SIZE_MAX`:

- **`_hard_limit`** — a push blocks while `_cnt >= _hard_limit`. This is the
  capacity: the queue will not grow past it (absent `force`).
- **`_soft_limit`** — after a push wakes, it only proceeds while
  `_cnt < _soft_limit`. With `soft == hard` (the usual `Queue(N)` case, since
  both default to `SIZE_MAX` and only `hard` is passed) this collapses to the
  hard limit. A `soft < hard` configuration creates a hysteresis band.
- **`_threshold`** — a pop only notifies a waiting producer when the post-pop
  size is below it. It throttles wakeups so a near-full queue does not
  thundering-herd its producers on every single pop.

`count()` reads `_cnt` lock-free; `size()` reads
`_items_queue.size()` under the lock. They can momentarily
disagree because `_cnt` is updated inside the locked region but read outside it;
`count()` trades exactness for not taking the lock.

## Shutdown protocol

Two stages, two atomics:

- **`end()`** sets `_ending` and notifies both condition variables. New pushes
  fail (the push predicate sees `_ending`), but pops keep succeeding while items
  remain, because `_pop_wait` only fails on `_ending` once the queue is empty.
  This is the drain: stop feeding, let consumers finish.
- **`finish()`** sets `_finished` and notifies both. Now both push and pop fail
  immediately, even with items still queued. This is the hard stop.

Both are idempotent (they early-out if the flag is already set) and `noexcept`.
Both call `_wake_all()`, which locks and unlocks the mutex and then broadcasts on
both condition variables. A waiter holds the mutex from its predicate check
until it is inside `wait()`, so the empty critical section orders the flag store
before that waiter blocks and the broadcast cannot be lost. Every parked thread
wakes, re-checks its predicate, and returns false. The destructor takes the
mutex, subtracts the remaining items from `_cnt`, and runs `_finish_locked()`,
so tearing down a queue releases any thread still blocked on it.

## Move, copy, lifetime

Copy is deleted. Move is supported and locks correctly: the move
constructor locks the source's mutex; move assignment locks both
mutexes with `std::lock` to avoid a deadlock. A moved-from queue
keeps its `_ending`/`_finished` reset to false.

`QueueSet<Key>` extends `Queue<Key, std::list<Key>>` with a
`std::unordered_map` from key to list iterator, giving a queue of unique values.
`push` deduplicates: an existing key is either left, updated in place, or renewed
(moved to front) per a `DupAction` policy. It reuses the parent's
`QueueState`, mutex, and condition variables, adding only the map bookkeeping
under the same lock.

## Concurrency model & invariants

- **One mutex, all shared mutation behind it.** The container and `_cnt` are
  only ever touched while holding `_state->_mutex`. `count()`'s lock-free read
  of the atomic `_cnt` is the single exception, and it is safe because the read
  is atomic and only ever approximate. There is nothing to race on outside the
  lock.
- **Notify after unlock.** Every public op unlocks before it notifies, so a
  woken thread does not immediately re-block on a lock the notifier still holds.
- **Notify only a counted waiter.** The waiter counts and the wake decision are
  read under the mutex, so a skipped notify never strands a blocked thread.
- **Untimed waits have no timeout.** A `timeout < 0` wait is a bare
  `wait(lk, pred)`. It relies on every state change that can satisfy a predicate
  either notifying a counted waiter or, for shutdown, locking the mutex before
  the broadcast.
- **`force` bypasses the bound.** `push_*(elem, timeout, force=true)` skips the
  wait and the capacity check. It can push past `_hard_limit`; that is the
  caller's responsibility.

A single lock serializes every producer and consumer. The README's Performance
section measures the single-ended path against a plain mutex queue and
moodycamel's blocking queue.

## Stability

`test/concurrent.cc` runs 4x4 and 8x8 producer/consumer shapes through
unbounded, bounded, `finish()`-mid-flight, shutdown-race, and all-untimed
scenarios. It checks that every item arrives exactly once with a count plus an
XOR checksum over the id space; a watchdog fails a hung run. Built with Homebrew
LLVM 22, it runs clean under ASan (1,000,000 items: no leaks, no heap
corruption, no use-after-free) and TSan (200,000 items: no data races), with
nothing suppressed. AGENTS.md has the commands.

## Standalone vs. Xapiand

`queue.h` comes from [Xapiand](https://github.com/Kronuz/Xapiand). Unlike
`stash.h`, it has no dependency on Xapiand's `log.h` or any other local header;
its includes are pure `std` (`<atomic>`, `<condition_variable>`, `<deque>`,
`<list>`, `<mutex>`, `<unordered_map>`, and friends). Xapiand fetches this
library at `main`, so both run the same code. CHANGELOG.md lists how the wake-up
path differs from the original.
