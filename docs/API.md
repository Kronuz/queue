# queue API reference

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
