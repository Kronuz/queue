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

The design is deliberately conventional. There is no lock-free cleverness here;
the value is in the feature surface (both ends, timeouts, bounds, finish), not
in the synchronization. Everything mutable lives behind one mutex, which is what
makes it trivially race-free and also what caps its throughput.

## Shared state: `QueueState`

`QueueState` (`queue.h:40`) holds everything shared between the queue and any
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
  so `count()` can be read without taking the lock.

The state is held by `std::shared_ptr` (`queue.h:68`), so several `Queue`
objects can share one `QueueState`. The two-arg constructor
(`Queue(const std::shared_ptr<QueueState>&)`, `queue.h:192`) is how a second
view onto the same underlying state is constructed.

Each `Queue` additionally owns its own `Container _items_queue` and two atomics,
`_ending` and `_finished` (`queue.h:63`). Note that the shutdown flags are
per-`Queue`, not on the shared state, while the count and the condition
variables are shared. For the common single-`Queue` case this distinction does
not matter.

## The wait machinery

Two private helpers carry all the blocking logic; every public push/pop routes
through one of them while holding a `std::unique_lock` on the mutex.

### `_push_wait(timeout, lk)` (`queue.h:70`)

The predicate is "the queue is shutting down, or there is room":
`_finished || _ending || _cnt < _hard_limit`. The timeout argument selects one
of three behaviors:

- **`timeout == 0`** (the `else` branch, `queue.h:83`): non-blocking. Evaluate
  the predicate once; if it is false, return false immediately. No waiting.
- **`timeout > 0`** (`queue.h:75`): compute `now + timeout` and
  `wait_until(lk, deadline, pred)`. If the deadline passes with the predicate
  still false, return false.
- **`timeout < 0`** (still inside `if (timeout)` since the value is non-zero,
  `queue.h:81`): block forever, implemented as
  `while (!wait_for(lk, 1s, pred)) {}` — a one-second re-check loop rather than
  an unbounded `wait`, so a missed notification cannot wedge it permanently.

After the wait, if the queue is finished or ending it returns false
(`queue.h:89`); otherwise it returns `_cnt < _soft_limit` (`queue.h:93`). With
the default limits (`hard == soft == SIZE_MAX`) the soft check is always true,
so an unbounded push always succeeds once it is not shutting down.

### `_pop_wait(timeout, lk)` (`queue.h:96`)

Mirror image. The predicate is "shutting down, or there is an item":
`_finished || _ending || !_items_queue.empty()`, with the same three timeout
behaviors. After the wait it returns false if `_finished`, or if `_ending` and
the queue is empty (`queue.h:116`) — so an `end()`ing queue still drains its
remaining items before pops start failing, while a `finish()`ed queue fails
immediately even with items left.

## The public ops

Each public method follows the same shape (`push_back`, `queue.h:257`, is
representative):

1. Take a `std::unique_lock` on the mutex.
2. Call the `_*_impl` helper, which runs the wait and, on success, mutates the
   container and bumps `_cnt`.
3. `lk.unlock()` explicitly.
4. Notify a condition variable based on the result.

On a successful push it does `_pop_cond.notify_one()` — wake one waiting
consumer. On a successful pop it does `_push_cond.notify_one()`, but **only if**
the post-pop size dropped below `_threshold` (`queue.h:307`), so a queue held
near its bound does not wake producers it will only re-block.

On a **failed** op, every method runs this block:

```cpp
// FIXME: This block shouldn't be needed!
_state->_pop_cond.notify_all();
_state->_push_cond.notify_all();
```

It is a belt-and-suspenders broadcast on both condition variables whenever an op
did not go through (a full/empty timeout, or a shutdown). The `FIXME` is the
author's own note that it should be redundant given the shutdown paths already
notify. In practice it is defensive over-signalling, and under contention it is
the dominant cost in the throughput benchmark (see `README.md`). It is correct,
just not free.

`push_back` / `push_front` differ only in which end of the deque they touch;
likewise `pop_back` / `pop_front`. `push`/`pop` are aliases for the back/front
pair. The `force` flag on the push path (`queue.h:124`) skips the wait and the
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

`count()` reads `_cnt` lock-free (`queue.h:380`); `size()` reads
`_items_queue.size()` under the lock (`queue.h:375`). They can momentarily
disagree because `_cnt` is updated inside the locked region but read outside it;
`count()` trades exactness for not taking the lock.

## Shutdown protocol

Two stages, two atomics (`queue.h:234` and `queue.h:246`):

- **`end()`** sets `_ending` and notifies both condition variables. New pushes
  fail (the push predicate sees `_ending`), but pops keep succeeding while items
  remain, because `_pop_wait` only fails on `_ending` once the queue is empty.
  This is the drain: stop feeding, let consumers finish.
- **`finish()`** sets `_finished` and notifies both. Now both push and pop fail
  immediately, even with items still queued. This is the hard stop.

Both are idempotent (they early-out if the flag is already set) and `noexcept`.
Because the wait predicates test these flags, flipping one and broadcasting
wakes every parked thread, which re-checks its predicate and returns false. The
destructor (`queue.h:224`) calls `finish()` after subtracting the remaining
items from `_cnt`, so tearing down a queue releases any thread still blocked on
it.

## Move, copy, lifetime

Copy is deleted (`queue.h:219`). Move is supported and locks correctly: the move
constructor locks the source's mutex (`queue.h:198`); move assignment locks both
mutexes with `std::lock` to avoid a deadlock (`queue.h:208`). A moved-from queue
keeps its `_ending`/`_finished` reset to false.

`QueueSet<Key>` (`queue.h:407`) extends `Queue<Key, std::list<Key>>` with a
`std::unordered_map` from key to list iterator, giving a queue of unique values.
`push` deduplicates: an existing key is either left, updated in place, or renewed
(moved to front) per a `DupAction` policy (`queue.h:400`). It reuses the parent's
`QueueState`, mutex, and condition variables, adding only the map bookkeeping
under the same lock.

## Concurrency model & invariants

- **One mutex, all shared mutation behind it.** The container and `_cnt` are
  only ever touched while holding `_state->_mutex`. `count()`'s lock-free read
  of the atomic `_cnt` is the single exception, and it is safe because the read
  is atomic and only ever approximate. This is why the structure is trivially
  race-free: ASan and TSan are clean because there is nothing to race on outside
  the lock (see `README.md`).
- **Notify after unlock.** Every public op unlocks before it notifies
  (`queue.h:261`), so a woken thread does not immediately re-block on a lock the
  notifier still holds. This is a standard condition-variable optimization and a
  correctness aid, not just a perf one.
- **The block-forever loop re-checks every second.** A `timeout < 0` wait is
  `while (!wait_for(1s, pred)) {}`, not a bare `wait`. A lost wakeup costs at
  most a one-second latency, never a permanent hang.
- **`force` bypasses the bound.** `push_*(elem, timeout, force=true)` skips the
  wait and the capacity check. It can push past `_hard_limit`; that is the
  caller's responsibility.

The cost of this model is throughput: a single lock serializes every producer
and consumer, and the `FIXME` over-signalling adds condition-variable traffic on
the failure path. The benchmark in `README.md` quantifies it. The structure
trades raw items/sec for a feature set (double-ended, timeouts, bounds,
graceful finish) that lock-free queues do not provide.

## Standalone vs. Xapiand

This is a verbatim extraction of `queue.h` from
[Xapiand](https://github.com/Kronuz/Xapiand). Unlike `stash.h`, `queue.h` has no
dependency on Xapiand's `log.h` or any other local header — its includes are
pure `std` (`<atomic>`, `<condition_variable>`, `<deque>`, `<list>`, `<mutex>`,
`<unordered_map>`, and friends). So there was nothing to decouple: the file is
copied as-is, license header included. Any change here (for instance, tightening
the `FIXME` over-signalling) should be made so it can be reconciled with
upstream.
