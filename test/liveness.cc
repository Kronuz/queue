// Liveness: every state change that lets a parked waiter go must wake it.
//
// Untimed waits have no periodic re-check, so a change that makes a parked thread's predicate
// true and wakes nobody (or wakes a thread that cannot use it) leaves that thread asleep for
// good. Each case parks threads in a known wait, confirmed through the waiter counts read under
// the mutex (a counted thread is inside wait() and needs a notify to leave it), makes one state
// change, and requires every parked thread to return before a deadline. A case that misses it
// FAILs and releases its threads with finish(); a watchdog turns anything still stuck into a FAIL
// exit, so a lost wake-up is a test failure, not a CI timeout.
//
// The cases, one per class of change (ARCHITECTURE.md, "Wake-ups"):
//   consumers: push_back, push_front, a forced push, QueueSet::push, a push on one of two Queues
//              sharing a QueueState, end(), finish(), a woken consumer whose take throws, and a
//              timed consumer parked beside an untimed one.
//   producers: pop_front, pop_back, a pop that leaves the size at or above the threshold, clear(),
//              QueueSet::clear(), QueueSet::erase(), QueueSet::pop(), a pop that leaves the count
//              below the hard limit but at the soft limit (every woken producer refuses), a pop on
//              one Queue and a clear() or destruction of a stopped Queue sharing the state, end(),
//              finish(), a woken producer whose insert throws, and a timed producer parked beside
//              an untimed one.
// The limits have no setter (they are fixed when the QueueState is built), and a timed-out
// waiter changes nothing, so neither is a class of its own.
//
// `queue_liveness` runs every case; `queue_liveness stress N` runs N rounds of random producers,
// consumers and clears on one Queue or two sharing a state, and checks at quiescence that no item
// is left behind with its consumers asleep (the binary to run under TSan).
// LIVENESS_DEADLINE_MS overrides the 10 s deadline (a correct wake-up takes microseconds).
//
// Build: c++ -std=c++20 -O1 -pthread -I.. test/liveness.cc

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

#include "queue.h"

using namespace std::chrono;

static milliseconds deadline{10000};
static std::atomic<long> progress{0};
static std::atomic<const char*> current{"start"};

// Reads the waiter counts under the mutex. The consumer count is per Queue; an older header kept
// one count on the shared state, which the accessor falls back to so the revert check compiles.
template <typename Q>
struct Probe : Q {
	using Q::Q;

	std::size_t pop_waiters() const {
		std::lock_guard<std::mutex> lk(this->_state->_mutex);
		if constexpr (requires(const Probe& p) { p._pop_waiters; }) {
			return this->_pop_waiters;
		} else {
			return this->_state->_pop_waiters;
		}
	}

	std::size_t push_waiters() const {
		std::lock_guard<std::mutex> lk(this->_state->_mutex);
		return this->_state->_push_waiters;
	}

	static constexpr bool per_queue_pop_waiters() {
		return requires(const Probe& p) { p._pop_waiters; };
	}
};

template <typename F>
static bool wait_until(F pred, milliseconds limit = deadline) {
	auto end = steady_clock::now() + limit;
	while (!pred()) {
		if (steady_clock::now() > end) {
			return false;
		}
		std::this_thread::sleep_for(microseconds(200));
	}
	return true;
}

// Threads that each run one blocking call and record how it ended.
struct Threads {
	std::vector<std::thread> threads;
	std::atomic<int> done{0}, ok{0}, threw{0};
	std::atomic<long> sum{0};

	template <typename F>
	void spawn(F f) {
		threads.emplace_back([this, f]() mutable {
			try {
				if (f()) {
					++ok;
				}
			} catch (...) {
				++threw;
			}
			++done;
		});
	}

	bool finished(int n) { return wait_until([&] { return done.load() >= n; }); }

	void join() {
		for (auto& t : threads) {
			t.join();
		}
		threads.clear();
	}
};

static int failures = 0;

static bool check(const char* name, bool cond, const char* what) {
	if (!cond) {
		std::printf("  FAIL %s: %s\n", name, what);
		++failures;
	}
	return cond;
}

static void pass(const char* name) {
	std::printf("  %-58s OK\n", name);
}

static void begin(const char* name) {
	current = name;
	++progress;
}

// Throws from its next move construction or move assignment when armed.
struct Throwy {
	int v = 0;
	static inline std::atomic<int> throw_construct{0}, throw_assign{0};

	static bool fire(std::atomic<int>& armed) {
		int n = armed.load();
		while (n > 0) {
			if (armed.compare_exchange_weak(n, n - 1)) {
				return true;
			}
		}
		return false;
	}

	Throwy() = default;
	explicit Throwy(int x) : v(x) { }
	Throwy(const Throwy&) = default;
	Throwy(Throwy&& o) : v(o.v) {
		if (fire(throw_construct)) {
			throw std::runtime_error("move construct");
		}
	}
	Throwy& operator=(const Throwy&) = default;
	Throwy& operator=(Throwy&& o) {
		if (fire(throw_assign)) {
			throw std::runtime_error("move assign");
		}
		v = o.v;
		return *this;
	}
};

// ---- consumers ---------------------------------------------------------------------------

// how: 0 push_back, 1 push_front, 2 forced push_back (hard limit 0), 3 forced push_front.
static void push_wakes_consumers(int how) {
	static const char* names[] = {"push_back wakes parked consumers", "push_front wakes parked consumers",
	                              "forced push_back wakes parked consumers", "forced push_front wakes parked consumers"};
	const char* name = names[how];
	begin(name);
	Probe<queue::Queue<int>> q(how >= 2 ? 0 : static_cast<std::size_t>(-1));
	Threads t;
	const int n = 3;
	for (int i = 0; i < n; ++i) {
		t.spawn([&, i] {
			int v = 0;
			bool r = (i & 1) ? q.pop_back(v) : q.pop_front(v);
			t.sum += v;
			return r;
		});
	}
	if (check(name, wait_until([&] { return q.pop_waiters() == n; }), "consumers did not park")) {
		for (int k = 1; k <= n; ++k) {
			bool force = how >= 2;
			bool r = (how & 1) ? q.push_front(k, -1.0, force) : q.push_back(k, -1.0, force);
			check(name, r, "push failed");
		}
		if (check(name, t.finished(n), "a parked consumer was not woken") &&
		    check(name, t.ok == n && t.sum == 6, "wrong items")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

static void queueset_push_wakes_consumers() {
	const char* name = "QueueSet::push wakes parked consumers";
	begin(name);
	Probe<queue::QueueSet<int>> q;
	Threads t;
	for (int i = 0; i < 2; ++i) {
		t.spawn([&] { int v = 0; bool r = q.pop(v); t.sum += v; return r; });
	}
	if (check(name, wait_until([&] { return q.pop_waiters() == 2; }), "consumers did not park")) {
		q.push(1);
		q.push(2);
		if (check(name, t.finished(2), "a parked consumer was not woken") && check(name, t.sum == 3, "wrong items")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// CA1: with one shared condition variable a push on A could wake B's consumer, which went back to
// sleep, and A's consumer slept on with an item in A.
static void shared_state_push_wakes_own_consumer() {
	const char* name = "push wakes its own Queue's consumer on a shared state";
	begin(name);
	bool ok = true;
	for (int iter = 0; iter < 20 && ok; ++iter) {
		auto st = std::make_shared<queue::QueueState>();
		Probe<queue::Queue<int>> a(st), b(st);
		auto parked = [&](std::size_t n) {
			return wait_until([&] {
				if constexpr (Probe<queue::Queue<int>>::per_queue_pop_waiters()) {
					return a.pop_waiters() + b.pop_waiters() == n;
				} else {
					return a.pop_waiters() == n;  // one count on the shared state
				}
			});
		};
		Threads ta, tb;
		tb.spawn([&] { int v = 0; return b.pop_front(v); });
		ok = check(name, parked(1), "B's consumer did not park");
		ta.spawn([&] { int v = 0; bool r = a.pop_front(v); ta.sum += v; return r; });
		ok = ok && check(name, parked(2), "A's consumer did not park");
		if (ok) {
			a.push_back(7);
			ok = check(name, ta.finished(1) && ta.sum == 7, "A's consumer was not woken by a push on A");
		}
		if (ok) {
			b.push_back(8);
			ok = check(name, tb.finished(1), "B's consumer was not woken by a push on B");
		}
		a.finish();
		b.finish();
		ta.join();
		tb.join();
	}
	if (ok) {
		pass(name);
	}
}

// stop: 0 end(), 1 finish(). One untimed and one timed consumer.
static void stop_wakes_consumers(int stop) {
	const char* name = stop ? "finish() wakes parked consumers" : "end() wakes parked consumers";
	begin(name);
	Probe<queue::Queue<int>> q;
	Threads t;
	t.spawn([&] { int v; return q.pop_front(v); });
	t.spawn([&] { int v; return q.pop_back(v, 600.0); });
	if (check(name, wait_until([&] { return q.pop_waiters() == 2; }), "consumers did not park")) {
		if (stop) {
			q.finish();
		} else {
			q.end();
		}
		if (check(name, t.finished(2), "a parked consumer was not woken") && check(name, t.ok == 0, "a pop succeeded")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

static void throwing_take_hands_wake_on() {
	const char* name = "a woken consumer whose take throws wakes the next";
	begin(name);
	Probe<queue::Queue<Throwy>> q;
	Threads t;
	for (int i = 0; i < 2; ++i) {
		t.spawn([&] { Throwy v; bool r = q.pop_front(v); t.sum += v.v; return r; });
	}
	if (check(name, wait_until([&] { return q.pop_waiters() == 2; }), "consumers did not park")) {
		Throwy::throw_assign = 1;
		q.push_back(Throwy(5));
		if (check(name, t.finished(2), "the other parked consumer was not woken") &&
		    check(name, t.threw == 1 && t.ok == 1 && t.sum == 5, "wrong outcome")) {
			pass(name);
		}
	}
	Throwy::throw_assign = 0;
	q.finish();
	t.join();
}

static void timed_and_untimed_consumers() {
	const char* name = "pushes wake a timed and an untimed consumer";
	begin(name);
	Probe<queue::Queue<int>> q;
	Threads t;
	t.spawn([&] { int v = 0; bool r = q.pop_front(v, 600.0); t.sum += v; return r; });
	t.spawn([&] { int v = 0; bool r = q.pop_front(v); t.sum += v; return r; });
	if (check(name, wait_until([&] { return q.pop_waiters() == 2; }), "consumers did not park")) {
		q.push_back(1);
		q.push_back(2);
		if (check(name, t.finished(2), "a parked consumer was not woken") && check(name, t.ok == 2 && t.sum == 3, "wrong items")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// ---- producers ---------------------------------------------------------------------------

// how: 0 pop_front, 1 pop_back. Two parked producers, two pops.
static void pop_wakes_producers(int how) {
	const char* name = how ? "pop_back wakes parked producers" : "pop_front wakes parked producers";
	begin(name);
	Probe<queue::Queue<int>> q(2);
	q.push_back(1);
	q.push_back(2);
	Threads t;
	t.spawn([&] { return q.push_back(10); });
	t.spawn([&] { return q.push_back(11); });
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		int v;
		for (int k = 0; k < 2; ++k) {
			check(name, how ? q.pop_back(v) : q.pop_front(v), "pop failed");
		}
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 2 && q.count() == 2, "wrong count")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// CA1: a pop that leaves the size at or above the threshold notified nobody, and a producer
// parked at the hard limit slept on with room.
static void pop_above_threshold_wakes_producers() {
	const char* name = "a pop above the threshold wakes parked producers";
	begin(name);
	Probe<queue::Queue<int>> q(4, 4, 2);
	for (int k = 0; k < 4; ++k) {
		q.push_back(k);
	}
	Threads t;
	t.spawn([&] { return q.push_back(10); });
	t.spawn([&] { return q.push_back(11); });
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		int v;
		q.pop_front(v);  // size 3: at or above the threshold
		q.pop_front(v);  // size 2 or 3
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 2 && q.count() == 4, "wrong count")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// CA1: clear() frees every slot but notified one producer.
template <typename Q>
static void clear_wakes_producers(const char* name) {
	begin(name);
	Probe<Q> q(2);
	q.push(1);  // QueueSet::push keeps its map; Queue::push is push_back
	q.push(2);
	Threads t;
	for (int i = 0; i < 2; ++i) {
		t.spawn([&, i] { return q.push(10 + i); });
	}
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		q.clear();
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 2 && q.size() == 2, "wrong size")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// how: 0 erase (CA1: it notified nobody), 1 pop.
static void queueset_frees_slot(int how) {
	const char* name = how ? "QueueSet::pop wakes a parked producer" : "QueueSet::erase wakes a parked producer";
	begin(name);
	Probe<queue::QueueSet<int>> q(1);
	q.push(1);
	Threads t;
	t.spawn([&] { return q.push(2); });
	if (check(name, wait_until([&] { return q.push_waiters() == 1; }), "producer did not park")) {
		int v;
		if (how) {
			q.pop(v);
		} else {
			q.erase(1);
		}
		if (check(name, t.finished(1), "the parked producer was not woken") && check(name, t.ok == 1 && q.size() == 1, "wrong size")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// Soft limit below the hard limit: producers park at the hard limit (reached by force); a pop
// leaves room below it but not below the soft limit, so every woken producer refuses. Each one
// must still leave, not only the one the pop woke.
static void soft_limit_refusals_all_leave() {
	const char* name = "producers refused at the soft limit all return";
	begin(name);
	Probe<queue::Queue<int>> q(2, 1);
	q.push_back(1, -1.0, true);
	q.push_back(2, -1.0, true);
	Threads t;
	t.spawn([&] { return q.push_back(10); });
	t.spawn([&] { return q.push_back(11); });
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		int v;
		q.pop_front(v);
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 0 && q.count() == 1, "a refused push went in")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// how: 0 a pop on A, 1 clear() on an ended A (CA1 class: clear skipped the wake-up when its own
// Queue was stopping), 2 destroying a finished A (the destructor skipped it for a finished Queue).
static void shared_state_frees_slot(int how) {
	static const char* names[] = {"a pop on A wakes B's producer on a shared state",
	                              "clear() on an ended A wakes B's producer",
	                              "destroying a finished A wakes B's producer"};
	const char* name = names[how];
	begin(name);
	auto st = std::make_shared<queue::QueueState>(1);
	auto a = std::make_unique<Probe<queue::Queue<int>>>(st);
	Probe<queue::Queue<int>> b(st);
	a->push_back(1);
	// A stops before B's producer parks: the broadcast of end() or finish() must not be the
	// wake-up that lets it through.
	if (how == 1) {
		a->end();
	} else if (how == 2) {
		a->finish();
	}
	Threads t;
	t.spawn([&] { return b.push_back(2); });
	if (check(name, wait_until([&] { return b.push_waiters() == 1; }), "B's producer did not park")) {
		if (how == 0) {
			int v;
			a->pop_front(v);
		} else if (how == 1) {
			a->clear();
		} else {
			a.reset();
		}
		if (check(name, t.finished(1), "B's parked producer was not woken") && check(name, t.ok == 1 && b.size() == 1, "wrong size")) {
			pass(name);
		}
	}
	b.finish();
	if (a) {
		a->finish();
	}
	t.join();
}

// stop: 0 end(), 1 finish(). One untimed and one timed producer.
static void stop_wakes_producers(int stop) {
	const char* name = stop ? "finish() wakes parked producers" : "end() wakes parked producers";
	begin(name);
	Probe<queue::Queue<int>> q(1);
	q.push_back(1);
	Threads t;
	t.spawn([&] { return q.push_back(2); });
	t.spawn([&] { return q.push_front(3, 600.0); });
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		if (stop) {
			q.finish();
		} else {
			q.end();
		}
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 0, "a push succeeded")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

static void throwing_insert_hands_wake_on() {
	const char* name = "a woken producer whose insert throws wakes the next";
	begin(name);
	Probe<queue::Queue<Throwy>> q(1);
	q.push_back(Throwy(1));
	Threads t;
	for (int i = 0; i < 2; ++i) {
		t.spawn([&, i] { return q.push_back(Throwy(10 + i)); });
	}
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		Throwy::throw_construct = 1;
		Throwy v;
		q.pop_front(v);
		if (check(name, t.finished(2), "the other parked producer was not woken") &&
		    check(name, t.threw == 1 && t.ok == 1 && q.count() == 1, "wrong outcome")) {
			pass(name);
		}
	}
	Throwy::throw_construct = 0;
	q.finish();
	t.join();
}

static void timed_and_untimed_producers() {
	const char* name = "pops wake a timed and an untimed producer";
	begin(name);
	Probe<queue::Queue<int>> q(1);
	q.push_back(1);
	Threads t;
	t.spawn([&] { return q.push_back(2, 600.0); });
	t.spawn([&] { return q.push_back(3); });
	if (check(name, wait_until([&] { return q.push_waiters() == 2; }), "producers did not park")) {
		int v;
		q.pop_front(v);
		// The second slot opens once the first producer has pushed.
		check(name, q.pop_front(v), "pop failed");
		if (check(name, t.finished(2), "a parked producer was not woken") && check(name, t.ok == 2, "a push failed")) {
			pass(name);
		}
	}
	q.finish();
	t.join();
}

// ---- random stress -----------------------------------------------------------------------

static bool stress_round(std::mt19937_64& rng, int round) {
	static const std::size_t hards[] = {1, 2, 3, 8, static_cast<std::size_t>(-1)};
	const std::size_t hard = hards[rng() % 5];
	const std::size_t threshold = (rng() & 1) ? 1 : static_cast<std::size_t>(-1);
	const bool shared = rng() & 1;
	const int nq = shared ? 2 : 1;
	const int producers = 1 + static_cast<int>(rng() % 3);
	const int consumers = 1 + static_cast<int>(rng() % 3);  // per Queue
	const int per_producer = 150;
	const bool clears = rng() % 3 == 0;

	auto st = std::make_shared<queue::QueueState>(hard, static_cast<std::size_t>(-1), threshold);
	std::vector<std::unique_ptr<Probe<queue::Queue<int>>>> qs;
	for (int i = 0; i < nq; ++i) {
		qs.push_back(std::make_unique<Probe<queue::Queue<int>>>(st));
	}
	const int total = producers * per_producer;
	std::vector<std::atomic<unsigned char>> seen(static_cast<std::size_t>(total) + 1);
	std::atomic<int> popped{0}, dup{0}, pushed{0}, producers_done{0};
	std::atomic<bool> clearing{clears};

	std::vector<std::thread> threads;
	for (int p = 0; p < producers; ++p) {
		std::uint64_t seed = rng();
		threads.emplace_back([&, p, seed] {
			std::mt19937_64 r(seed);
			for (int k = 0; k < per_producer; ++k) {
				int id = 1 + p * per_producer + k;
				auto& q = *qs[r() % qs.size()];
				unsigned mode = static_cast<unsigned>(r() % 8);
				bool ok = mode == 0 ? q.push_front(id, 600.0) : mode == 1 ? q.push_front(id) : mode == 2 ? q.push_back(id, 600.0) : q.push_back(id);
				pushed += ok;
			}
			++producers_done;
		});
	}
	for (int qi = 0; qi < nq; ++qi) {
		for (int c = 0; c < consumers; ++c) {
			std::uint64_t seed = rng();
			threads.emplace_back([&, qi, seed] {
				std::mt19937_64 r(seed);
				auto& q = *qs[qi];
				int v = 0;
				for (;;) {
					unsigned mode = static_cast<unsigned>(r() % 4);
					bool ok = mode == 0 ? q.pop_back(v, 600.0) : mode == 1 ? q.pop_back(v) : mode == 2 ? q.pop_front(v, 600.0) : q.pop_front(v);
					if (!ok) {
						break;
					}
					if (v < 1 || v > total || seen[static_cast<std::size_t>(v)].exchange(1)) {
						++dup;
					}
					++popped;
				}
			});
		}
	}
	std::thread clearer;
	if (clears) {
		clearer = std::thread([&] {
			while (clearing) {
				qs[0]->clear();
				std::this_thread::sleep_for(microseconds(300));
			}
		});
	}

	// Producers finish only if every slot freed by a pop or a clear reaches a parked producer.
	bool ok = wait_until([&] { return producers_done.load() == producers; });
	clearing = false;
	if (clearer.joinable()) {
		clearer.join();
	}
	// Then every item must be taken: none may sit in a Queue whose consumers are all asleep.
	ok = ok && wait_until([&] {
		for (auto& q : qs) {
			if (!q->empty()) {
				return false;
			}
		}
		if constexpr (Probe<queue::Queue<int>>::per_queue_pop_waiters()) {
			for (auto& q : qs) {
				if (q->pop_waiters() != static_cast<std::size_t>(consumers)) {
					return false;
				}
			}
			return true;
		} else {
			return qs[0]->pop_waiters() == static_cast<std::size_t>(consumers * nq);  // one count on the state
		}
	});
	if (!ok) {
		std::printf("  FAIL stress round %d: hang (hard=%zu threshold=%zu queues=%d producers=%d consumers=%d clears=%d)"
		            " pushed=%d popped=%d\n", round, hard, threshold, nq, producers, consumers, clears, pushed.load(), popped.load());
	}
	for (auto& q : qs) {
		q->end();
	}
	for (auto& t : threads) {
		t.join();
	}
	if (ok && (dup || pushed != total || popped > total || (!clears && popped != total))) {
		std::printf("  FAIL stress round %d: pushed=%d popped=%d duplicates=%d\n", round, pushed.load(), popped.load(), dup.load());
		ok = false;
	}
	return ok;
}

static void stress(int rounds) {
	const char* name = "random producers, consumers and clears";
	begin(name);
	std::mt19937_64 rng(0x5eed);
	bool ok = true;
	for (int r = 0; r < rounds && ok; ++r) {
		ok = stress_round(rng, r);
		++progress;
	}
	if (check(name, ok, "see above")) {
		std::printf("  %-58s OK (%d rounds)\n", name, rounds);
	}
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (const char* ms = std::getenv("LIVENESS_DEADLINE_MS")) {
		deadline = milliseconds(std::atoi(ms));
	}
	// A case that cannot release its own threads (finish() itself lost) makes no progress.
	std::atomic<bool> stop{false};
	std::thread watchdog([&] {
		long last = -1;
		auto limit = std::max(deadline * 6, milliseconds(60000));
		for (;;) {
			auto end = steady_clock::now() + limit;
			while (!stop && steady_clock::now() < end) {
				std::this_thread::sleep_for(milliseconds(50));
			}
			if (stop) {
				return;
			}
			long p = progress.load();
			if (p == last) {
				std::printf("  FAIL %s: hang\n  RESULT: FAIL\n", current.load());
				std::fflush(stdout);
				_Exit(3);
			}
			last = p;
		}
	});

	std::printf("queue liveness\n");
	if (argc > 2 && !std::strcmp(argv[1], "stress")) {
		stress(std::atoi(argv[2]));
	} else {
		for (int how = 0; how < 4; ++how) {
			push_wakes_consumers(how);
		}
		queueset_push_wakes_consumers();
		shared_state_push_wakes_own_consumer();
		stop_wakes_consumers(0);
		stop_wakes_consumers(1);
		throwing_take_hands_wake_on();
		timed_and_untimed_consumers();

		pop_wakes_producers(0);
		pop_wakes_producers(1);
		pop_above_threshold_wakes_producers();
		clear_wakes_producers<queue::Queue<int>>("clear() wakes every parked producer");
#ifndef LIVENESS_NO_QUEUESET_CLEAR  // QueueSet::clear did not compile before the fix
		clear_wakes_producers<queue::QueueSet<int>>("QueueSet::clear() wakes every parked producer");
#endif
		queueset_frees_slot(0);
		queueset_frees_slot(1);
		soft_limit_refusals_all_leave();
		shared_state_frees_slot(0);
		shared_state_frees_slot(1);
		shared_state_frees_slot(2);
		stop_wakes_producers(0);
		stop_wakes_producers(1);
		throwing_insert_hands_wake_on();
		timed_and_untimed_producers();
	}

	stop = true;
	watchdog.join();
	std::printf("  RESULT: %s\n", failures ? "FAIL" : "OK");
	return failures ? 1 : 0;
}
