// Correctness test for the standalone queue library.
//
// Single-threaded coverage of the whole API surface: both ends as FIFO and
// LIFO, bounded capacity blocking then unblocking, the three timeout regimes
// (block-forever, timed, non-blocking), clear, the count/empty/size accessors,
// front(), and the graceful-shutdown semantics (end()/finish()) that wake
// waiters and reject further ops. Concurrency lives in test/concurrent.cc; this
// file only pins down what each call is supposed to return.
//
// Build: c++ -std=c++20 -I.. test.cc -o test && ./test

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "queue.h"

using namespace std::chrono_literals;

// Push at both ends, pop from both ends, and check the ordering the deque gives
// us: pop_front sees the oldest push_back (FIFO), pop_back sees the newest
// push_back (LIFO). push_front / push_back put items on opposite ends.
static void test_both_ends_ordering() {
	queue::Queue<int> q;

	// Build [10, 20, 30] front-to-back via push_back.
	assert(q.push_back(10));
	assert(q.push_back(20));
	assert(q.push_back(30));
	assert(q.size() == 3);
	assert(q.count() == 3);

	int v = 0;
	// FIFO from the front: 10, 20, 30.
	assert(q.pop_front(v) && v == 10);
	assert(q.pop_front(v) && v == 20);
	// LIFO from the back: 30 is the newest remaining.
	assert(q.pop_back(v) && v == 30);
	assert(q.empty());

	// push_front grows the front: [3, 2, 1] after pushing 1, 2, 3 to front.
	assert(q.push_front(1));
	assert(q.push_front(2));
	assert(q.push_front(3));
	// pop_front now sees the most-recently-fronted item first (3).
	assert(q.pop_front(v) && v == 3);
	// pop_back sees the oldest front-push (1).
	assert(q.pop_back(v) && v == 1);
	assert(q.size() == 1);
	assert(q.pop_back(v) && v == 2);
	assert(q.empty());

	std::printf("queue OK: both-ends FIFO/LIFO ordering\n");
}

// push() and pop() are the single-ended aliases (push_back / pop_front), the
// MPMC fast path the benchmark exercises.
static void test_push_pop_aliases() {
	queue::Queue<int> q;
	assert(q.push(1));
	assert(q.push(2));
	int v = 0;
	assert(q.pop(v) && v == 1);   // FIFO
	assert(q.pop(v) && v == 2);
	assert(q.empty());
	std::printf("queue OK: push/pop aliases are push_back/pop_front\n");
}

// front() peeks without consuming; the bool overload reports emptiness.
static void test_front_peek() {
	queue::Queue<int> q;
	int v = -1;
	assert(!q.front(v));          // empty: false, v untouched-by-contract
	assert(q.push_back(7));
	assert(q.push_back(8));
	assert(q.front(v) && v == 7); // front is the oldest, still there
	assert(q.size() == 2);
	assert(q.front() == 7);       // reference overload
	assert(q.size() == 2);        // peek did not consume
	std::printf("queue OK: front() peeks without consuming\n");
}

// A non-blocking call (timeout == 0) returns immediately: false on an empty pop
// or a full push, true when it can proceed. No waiting on either path.
static void test_nonblocking_timeout_zero() {
	queue::Queue<int> q(/*hard_limit*/ 2);

	int v = 0;
	// Empty pop, non-blocking: false right away.
	assert(!q.pop_front(v, 0.0));
	assert(!q.pop_back(v, 0.0));

	// Non-blocking pushes up to the bound succeed.
	assert(q.push_back(1, 0.0));
	assert(q.push_back(2, 0.0));
	// Full now (_cnt == hard_limit): non-blocking push fails immediately.
	assert(!q.push_back(3, 0.0));
	assert(!q.push_front(3, 0.0));
	assert(q.size() == 2);

	// A non-blocking pop on a non-empty queue succeeds.
	assert(q.pop_front(v, 0.0) && v == 1);
	// And now there is room for one more push.
	assert(q.push_back(4, 0.0));
	assert(q.size() == 2);

	std::printf("queue OK: timeout==0 is non-blocking on both push and pop\n");
}

// A positive timeout on an empty pop / full push returns false after waiting
// roughly that long, rather than blocking forever.
static void test_timed_timeout_returns_false() {
	queue::Queue<int> q(/*hard_limit*/ 1);
	int v = 0;

	auto t0 = std::chrono::steady_clock::now();
	assert(!q.pop_front(v, 0.05));   // empty, times out -> false
	auto waited = std::chrono::steady_clock::now() - t0;
	assert(waited >= 40ms && "timed pop returned too early to have waited");

	// Fill to the bound, then a timed push must time out (full) -> false.
	assert(q.push_back(1));
	t0 = std::chrono::steady_clock::now();
	assert(!q.push_back(2, 0.05));
	waited = std::chrono::steady_clock::now() - t0;
	assert(waited >= 40ms && "timed push returned too early to have waited");

	std::printf("queue OK: positive timeout returns false on empty/full after waiting\n");
}

// Bounded capacity: a full push blocks until a consumer makes room, then
// unblocks and succeeds. Drive it with a real second thread so the block is the
// genuine condition-variable wait, not a spin.
static void test_bounded_blocks_then_unblocks() {
	queue::Queue<int> q(/*hard_limit*/ 2);
	assert(q.push_back(1));
	assert(q.push_back(2));        // queue is now full

	std::atomic<bool> pushed{false};
	std::thread producer([&] {
		// Block-forever push (default timeout < 0). It cannot proceed until the
		// consumer below pops one item.
		assert(q.push_back(3));
		pushed.store(true);
	});

	// Give the producer time to actually park on the full queue.
	std::this_thread::sleep_for(50ms);
	assert(!pushed.load() && "push should still be blocked on a full queue");

	int v = 0;
	assert(q.pop_front(v) && v == 1);   // make room -> wakes the producer
	producer.join();
	assert(pushed.load());

	// Contents are now [2, 3].
	assert(q.pop_front(v) && v == 2);
	assert(q.pop_front(v) && v == 3);
	assert(q.empty());

	std::printf("queue OK: bounded push blocks when full, unblocks on pop\n");
}

// A blocked pop wakes the moment an item arrives.
static void test_blocking_pop_wakes_on_push() {
	queue::Queue<int> q;
	std::atomic<int> got{-1};
	std::thread consumer([&] {
		int v = 0;
		assert(q.pop_front(v));    // block-forever until a push arrives
		got.store(v);
	});

	std::this_thread::sleep_for(50ms);
	assert(got.load() == -1 && "pop should still be blocked on an empty queue");

	assert(q.push_back(42));
	consumer.join();
	assert(got.load() == 42);

	std::printf("queue OK: blocking pop wakes on push\n");
}

// clear() drops every item and keeps count() consistent.
static void test_clear() {
	queue::Queue<int> q;
	for (int i = 0; i < 5; ++i) assert(q.push_back(i));
	assert(q.size() == 5 && q.count() == 5);
	q.clear();
	assert(q.empty() && q.size() == 0 && q.count() == 0);
	// Still usable after a clear.
	assert(q.push_back(99));
	int v = 0;
	assert(q.pop_front(v) && v == 99);
	std::printf("queue OK: clear empties the queue and keeps count consistent\n");
}

// count/empty/size track the live item set, and the empty(f) callback overload
// observes the emptiness under the lock.
static void test_accessors() {
	queue::Queue<std::string> q;
	assert(q.empty());
	assert(q.size() == 0);
	assert(q.count() == 0);

	assert(q.push_back("a"));
	assert(q.push_back("b"));
	assert(!q.empty());
	assert(q.size() == 2);
	assert(q.count() == 2);

	bool seen_empty = true;
	q.empty([&](bool e) { seen_empty = e; });
	assert(!seen_empty && "empty(f) should report non-empty here");

	std::string s;
	assert(q.pop_front(s) && s == "a");
	assert(q.size() == 1 && q.count() == 1);

	std::printf("queue OK: count/empty/size accessors track the item set\n");
}

// finish() is the hard shutdown: it wakes every waiter, those waiters return
// false, and all subsequent ops (push and pop, blocking or not) are rejected.
static void test_finish_wakes_and_rejects() {
	queue::Queue<int> q;

	// Park a consumer on an empty queue, then finish() and confirm it returns.
	std::atomic<bool> returned{false};
	std::atomic<bool> result{true};
	std::thread waiter([&] {
		int v = 0;
		bool ok = q.pop_front(v);   // block-forever; finish() must wake it
		result.store(ok);
		returned.store(true);
	});

	std::this_thread::sleep_for(50ms);
	assert(!returned.load() && "consumer should be parked before finish()");

	q.finish();
	waiter.join();
	assert(returned.load());
	assert(!result.load() && "a pop woken by finish() must return false");

	// After finish(): every push and pop is rejected, regardless of timeout.
	assert(!q.push_back(1));
	assert(!q.push_front(1));
	assert(!q.push_back(1, 0.0));
	int v = 0;
	assert(!q.pop_front(v));
	assert(!q.pop_back(v, 0.0));

	std::printf("queue OK: finish() wakes waiters and rejects all further ops\n");
}

// end() is the graceful drain: it wakes waiters and stops new pushes, but pops
// still succeed while items remain, and only fail once the queue drains.
static void test_end_drains_then_rejects() {
	queue::Queue<int> q;
	assert(q.push_back(1));
	assert(q.push_back(2));

	q.end();

	// Pushes are rejected once ending.
	assert(!q.push_back(3));
	assert(!q.push_front(3));

	// But the items already queued can still be popped.
	int v = 0;
	assert(q.pop_front(v) && v == 1);
	assert(q.pop_front(v) && v == 2);

	// Now empty and ending: further pops fail.
	assert(!q.pop_front(v));
	assert(!q.pop_back(v));

	std::printf("queue OK: end() stops pushes, drains remaining, then rejects pops\n");
}

// A pop parked on an empty queue must also wake on end() (ending + empty -> false).
static void test_end_wakes_empty_waiter() {
	queue::Queue<int> q;
	std::atomic<bool> returned{false};
	std::atomic<bool> result{true};
	std::thread waiter([&] {
		int v = 0;
		bool ok = q.pop_front(v);
		result.store(ok);
		returned.store(true);
	});

	std::this_thread::sleep_for(50ms);
	assert(!returned.load());

	q.end();
	waiter.join();
	assert(returned.load());
	assert(!result.load() && "pop woken by end() on an empty queue must return false");

	std::printf("queue OK: end() wakes a pop parked on an empty queue\n");
}

int main() {
	test_both_ends_ordering();
	test_push_pop_aliases();
	test_front_peek();
	test_nonblocking_timeout_zero();
	test_timed_timeout_returns_false();
	test_bounded_blocks_then_unblocks();
	test_blocking_pop_wakes_on_push();
	test_clear();
	test_accessors();
	test_finish_wakes_and_rejects();
	test_end_drains_then_rejects();
	test_end_wakes_empty_waiter();
	std::printf("all queue tests passed\n");
	return 0;
}
