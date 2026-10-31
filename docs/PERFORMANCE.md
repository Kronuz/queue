# queue: stability and performance

> **Measured before `bc16081`**, which removed the failure-path `notify_all`
> over-signalling this analysis blames for the gap to the baseline. The table
> describes that older code; rerun `queue_bench` for current numbers.

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

## Wake-up cost (perf pass)

Throughput on an Apple arm64 host, Mitems/s, 1x1 / 2x2 / 4x4 / 8x8 (best of 3):

| | 1x1 | 2x2 | 4x4 | 8x8 |
|---|---|---|---|---|
| `Queue` before | 12.8 | 5.1 | 6.5 | 9.4 |
| `Queue` now | 44.8 | 15.2 | 19.5 | 30.6 |
| mutex+condvar+deque | 33.5 | 15.4 | 14.7 | 19.3 |
| moodycamel Blocking | 8.5 | 8.5 | 6.1 | 5.0 |

How: the state counts blocked poppers and pushers under the mutex, and a
push/pop only calls `notify_one` when the matching count is non-zero. An
unbounded queue never has a blocked pusher, so pops never touch `_push_cond`.
A blocking wait with no timeout uses `wait(lk, pred)` (no 1 s polling, no clock
read) and skips the wait entirely when the predicate already holds. Without
polling, `end()`/`finish()` lock and unlock the mutex before notifying, so a
waiter between its predicate check and its block cannot miss the shutdown.
`_cnt` is only written under the mutex, so it uses load+store instead of a
locked read-modify-write. TSan (clang, Linux x86_64) is clean on the stress
test, which now includes a shutdown-race loop.

## Verdict

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
$CXX -std=c++20 -fsanitize=address -g -O1 -I. test/concurrent.cc -o build/asan && build/asan
$CXX -std=c++20 -fsanitize=thread  -g -O1 -I. test/concurrent.cc -o build/tsan && build/tsan 200000

# benchmark
cmake -B build -DCMAKE_CXX_COMPILER=$CXX
cmake --build build --target queue_bench && ./build/queue_bench
```
