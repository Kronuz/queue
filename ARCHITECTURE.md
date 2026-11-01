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
- `std::condition_variable _push_cond` — producers wait here for room. Room is
  `_cnt` against the limits, which every `Queue` on the state shares, so every
  producer waits for the same thing and any of them can take a freed slot.
- `std::atomic_size_t _cnt` — the live item count, kept alongside the container
  so `count()` can be read without taking the lock. It changes only under the
  mutex, so `_cnt_add` and `_cnt_sub` use a relaxed load plus a store instead of
  a locked read-modify-write.
- `size_t _push_waiters` — the number of threads blocked in a push wait. It
  changes only under the mutex and lets a notifier skip the condition-variable
  call when nobody waits.
- `_room_freed(freed)` and `_wake_pushers(wake)` — the one place that decides
  which producers a drop of `_cnt` wakes (see "Wake-ups").

The state is held by `std::shared_ptr`, so several `Queue`
objects can share one `QueueState`. The two-arg constructor
(`Queue(const std::shared_ptr<QueueState>&)`) is how a second
view onto the same underlying state is constructed.

Each `Queue` additionally owns its own `Container _items_queue`, two atomics,
`_ending` and `_finished`, and the consumer side of the waiting: `_pop_cond`, where
its consumers wait for an item, and `_pop_waiters`, their count. A consumer waits
for this `Queue`'s items and flags, so its condition variable is per `Queue`: a
shared one would let a push on one `Queue` wake a consumer of another, which goes
back to sleep while the item waits. The count, the limits, the mutex and the
producers' condition variable are shared. For the common single-`Queue` case this
distinction does not matter.

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
it after, and set the caller's `waited` flag: a thread that blocked may hold a
`notify_one` meant for whoever takes the freed slot.

After the wait, if the queue is finished or ending it returns false; otherwise it returns `_cnt < _soft_limit`. With
the default limits (`hard == soft == SIZE_MAX`) the soft check is always true,
so an unbounded push always succeeds once it is not shutting down.

### `_pop_wait(timeout, lk)`

Mirror image. The predicate is "shutting down, or there is an item":
`_finished || _ending || !_items_queue.empty()`, with the same three timeout
behaviors, the `Queue`'s own `_pop_waiters` count and the same `waited` flag. After the wait it returns false if
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

A successful push does `_pop_cond.notify_one()` when the `Queue`'s `_pop_waiters`
is non-zero. A successful pop asks `_room_freed(1)`, which wakes one producer
when `_push_waiters` is non-zero and the pop left `_cnt` below the hard limit. An
unbounded queue never has a blocked pusher, so its pops never touch `_push_cond`.

A **failed** op (a full or empty timeout, or a shutdown) changes no state, so it
notifies nothing, with one exception: a thread that blocked, was woken and then
does not use what woke it hands the wake-up on (see "Wake-ups"). Shutdown
wake-ups come from `end()` and `finish()`.

`push_back` / `push_front` differ only in which end of the deque they touch;
likewise `pop_back` / `pop_front`. `push`/`pop` are aliases for the back/front
pair. The `force` flag on the push path skips the wait and the
bound entirely, pushing unconditionally.

**The fast path stays in the caller.** `_push_back_impl` first asks `_push_room()`
(`!(_finished || _ending) && _cnt < _hard_limit && _cnt < _soft_limit`), which is exactly
the case where `_push_wait` returns true without waiting; only the other cases call the
out-of-line `_push_wait`, and its `unique_lock` reference then escapes only on that path.
The four hot operations and their `_impl` bodies are `QUEUE_HOT` (`always_inline`) so
they compile into the producer or consumer loop. With the call to `_push_wait` on every
push and the operations out of line, Apple clang on x86-64 ran the queue at 0.55x of a
plain mutex queue at 8x8; with the waiting paths moved into separate out-of-line functions
it lost 15-20% at 4x4 and 8x8.
`_cnt` is stored with release ordering: a sequentially consistent store is an `xchg` on
x86, a full barrier inside the critical section.

## The bound

Three numbers, all on `QueueState`, all defaulting to `SIZE_MAX`:

- **`_hard_limit`** — a push blocks while `_cnt >= _hard_limit`. This is the
  capacity: the queue will not grow past it (absent `force`).
- **`_soft_limit`** — after a push wakes, it only proceeds while
  `_cnt < _soft_limit`. With `soft == hard` (the usual `Queue(N)` case, since
  both default to `SIZE_MAX` and only `hard` is passed) this collapses to the
  hard limit. A `soft < hard` configuration creates a hysteresis band.
- **`_threshold`** — stored and accepted by the constructors, with no effect on
  waiting or waking. A producer parked at the hard limit can push as soon as
  `_cnt < _hard_limit`, so the pop that makes room wakes it; holding the wake-up
  until the queue drains below a threshold would leave it asleep with room.
  Each freed slot wakes at most one producer, so a near-full queue wakes one
  producer per pop, not all of them.

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
the `Queue`'s `_pop_cond` and on the shared `_push_cond`. A waiter holds the mutex
from its predicate check until it is inside `wait()`, so the empty critical
section orders the flag store before that waiter blocks and the broadcast cannot
be lost. `test/race.cc` forces a stop into that window through `QUEUE_TEST_HOOK`, which
marks it in the untimed waits and is empty unless a test defines it. Every parked thread
of that `Queue` wakes, re-checks its predicate, and
returns false; producers of other `Queue`s on the state re-check and sleep again.
The destructor takes the mutex, subtracts the remaining items from `_cnt`, marks
the queue finished and broadcasts on both, which gives its slots to producers of
other `Queue`s on the state. It does not release threads blocked in calls on the
destroyed `Queue` itself: they would return into an object that no longer exists.
`finish()` the `Queue` and join those threads before destroying it.

## Wake-ups

Untimed waits have no timeout and no periodic re-check, so every change that can
make a parked thread's predicate true must wake a thread that acts on it. The
predicates are, for a `Queue` Q:

- a consumer: `Q._finished || Q._ending || !Q._items_queue.empty()`;
- a producer: `Q._finished || Q._ending || _cnt < _hard_limit`.

Every change, the waiters it can release, and what it wakes:

| change | can release | wakes |
|---|---|---|
| `push_back`, `push_front`, forced or not; `QueueSet::push` | a consumer of this `Queue` | one on this `Queue`'s `_pop_cond`, if one is parked |
| `pop_back`, `pop_front`, `QueueSet::pop`, `QueueSet::erase` (one slot) | a producer of any `Queue` on the state | one, if one is parked and `_cnt < _hard_limit` (`_room_freed(1)`) |
| `clear()`, `QueueSet::clear()` (n slots) | producers of any `Queue` on the state | all for n > 1, one for n = 1, if any is parked and there is room; also when this `Queue` is stopping |
| `end()`, `finish()` | this `Queue`'s consumers and producers | all, on this `Queue`'s `_pop_cond` and on `_push_cond`, after the empty critical section |
| the destructor (its items leave `_cnt`) | its own waiters, and producers of other `Queue`s | all, on both |
| move assignment (the target's items leave its old state's `_cnt`) | producers on that state | as `clear()` |
| a timed waiter's deadline | nobody: no state changes | nothing |
| a woken waiter that does not use the wake-up | the next parked waiter | one, handed on (below) |

A forced push only raises `_cnt`, so it releases consumers alone, like any push.
The limits have no setter: they are fixed when the `QueueState` is built. Move
construction and move assignment need both `Queue`s free of waiters.

**Why one is enough.** The consumers parked on a `Queue`'s `_pop_cond` all wait
for an item in that `Queue`, and the producers parked on `_push_cond` all wait for
room in the shared count. A woken consumer that finds an item takes it; a woken
producer that finds room takes a slot. So whenever a thread is blocked, the
threads woken but not yet past their re-check number at least the items (or free
slots): a waiter blocks only when there are none, each push or freed slot wakes a
blocked thread when there is one, and each woken thread takes one or finds none
left. The waiter counts include those woken threads, so a notify skipped because
the count is zero is one that would have found nobody blocked.

**Handing a wake-up on.** A woken thread can leave without taking what it was
woken for in two ways: a producer that finds `_cnt < _hard_limit` but
`_cnt >= _soft_limit` refuses, and inserting or taking the element can throw. Then
the thread hands the wake-up to the next waiter before it leaves. `_push_wait`
does it when it refuses after waiting, and a `_hand_on` guard, armed by a thread
that waited, does it when the insert or take that follows throws: one producer if
`_room_freed(1)` finds one parked with room, one consumer if one is parked and an
item is left. Only a thread that blocked does this, and it notifies under the lock.
A producer that leaves because its own `Queue` is stopping hands on too: a pop's
`notify_one` can reach it between `end()`'s empty critical section and its
broadcast.

**Renewal.** `QueueSet::push` of a present key erases it and pushes it again at
the front. That push cannot wait or refuse unless the queue is stopping: the erase
leaves `_cnt` below both limits, and `QueueSet` has no `force` and no shared state.
A stopping queue has no parked producers. A failed re-push still reports its slot
to `_room_freed(1)`, like a pop.

`test/liveness.cc` parks threads in each of these situations, confirms them
parked through the waiter counts, makes the change, and fails if one is still
asleep at a deadline.

## Move, copy, lifetime

Copy is deleted. The move constructor locks the source's mutex and takes its
items and state. Move assignment first gives the target's own items back, as the
destructor does: they leave its state's `_cnt`, and `_room_freed()` wakes that
state's producers. It locks both mutexes with `std::lock`, or one when both
`Queue`s share a state, does nothing on self-assignment, and holds the old state
in a local `shared_ptr` until the lock is released, so the old mutex outlives the
unlock. The target's `_ending` and `_finished` are reset to false. Neither move
may run while a thread waits on either `Queue`. A moved-from `Queue` has no state: it
can be destroyed or assigned to, and nothing else (including being moved from again).

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
  The hand-on of an unused wake-up is the exception: it runs under the lock, on
  the slow path.
- **Notify only a counted waiter.** The waiter counts and the wake decision are
  read under the mutex, so a skipped notify never strands a blocked thread.
- **Untimed waits have no timeout.** A `timeout < 0` wait is a bare
  `wait(lk, pred)`. It relies on every state change that can satisfy a predicate
  either notifying a counted waiter or, for shutdown, locking the mutex before
  the broadcast ("Wake-ups").
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

`test/liveness.cc` covers every row of the table in "Wake-ups" with a
deterministic case under a deadline, and runs random rounds of producers,
consumers and clears on one `Queue` or two sharing a state, checking at
quiescence that no item is left with its consumers asleep. Both run under TSan.

## Standalone vs. Xapiand

`queue.h` comes from [Xapiand](https://github.com/Kronuz/Xapiand). Unlike
`stash.h`, it has no dependency on Xapiand's `log.h` or any other local header;
its includes are pure `std` (`<atomic>`, `<condition_variable>`, `<deque>`,
`<list>`, `<mutex>`, `<unordered_map>`, and friends). Xapiand fetches this
library at `main`, so both run the same code. CHANGELOG.md lists how the wake-up
path differs from the original.
