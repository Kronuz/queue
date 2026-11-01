// Differential test: the current Queue against the frozen copy of queue.h in test/queue_frozen.h.
//
// The same seeded sequence of single-threaded operations runs on both and records every observable
// result: each call's return value, the value it popped, and size(), count() and empty() after it.
// The traces must be identical. The sequences cover every limit shape (unbounded, hard limit,
// soft limit below and above the hard limit, a hard limit of zero), both ends, force, the non-blocking
// and timed regimes, blocking calls that cannot block, clear, end and finish. Move assignment is the
// one class whose results differ (move_trace below).
//
// Build: c++ -std=c++20 -I.. test/differential.cc

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include "queue.h"
#include "queue_frozen.h"

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); if (++failures > 20) std::exit(1); } \
	} while (0)

constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

struct Shape {
	std::size_t hard, soft, threshold;
};

template <typename Q>
static std::vector<long long> trace(const Shape& shape, std::uint64_t seed, int steps, bool timed) {
	Q q(shape.hard, shape.soft, shape.threshold);
	std::mt19937_64 rng(seed);
	std::vector<long long> out;
	bool ended = false, finished = false;
	long long next = 1;
	auto observe = [&] {
		out.push_back(static_cast<long long>(q.size()));
		out.push_back(static_cast<long long>(q.count()));
		out.push_back(q.empty() ? 1 : 0);
		int front = -1;
		out.push_back(q.front(front) ? front : -1);
	};
	for (int i = 0; i < steps; ++i) {
		const unsigned op = static_cast<unsigned>(rng() % 100);
		int v = 0;
		bool r = false;
		if (op < 20) {
			r = q.push_back(static_cast<int>(next), 0.0);
			out.push_back(r);
			next += r;
		} else if (op < 28) {
			r = q.push_back(static_cast<int>(next), 0.0, true);
			out.push_back(r);
			next += r;
		} else if (op < 38) {
			r = q.push_front(static_cast<int>(next), 0.0);
			out.push_back(r);
			next += r;
		} else if (op < 44) {
			r = q.push_front(static_cast<int>(next), 0.0, true);
			out.push_back(r);
			next += r;
		} else if (op < 56) {
			r = q.pop_front(v, 0.0);
			out.push_back(r);
			out.push_back(r ? v : -1);
		} else if (op < 64) {
			r = q.pop_back(v, 0.0);
			out.push_back(r);
			out.push_back(r ? v : -1);
		} else if (op < 70) {
			// Blocking calls, only where they cannot block: room below the hard limit, or shut down.
			if (q.count() < shape.hard || ended || finished) {
				r = q.push_back(static_cast<int>(next));
				out.push_back(r);
				next += r;
			}
		} else if (op < 76) {
			if (!q.empty() || ended || finished) {
				r = q.pop_front(v);
				out.push_back(r);
				out.push_back(r ? v : -1);
			}
		} else if (op < 78) {
			// A timed call on a full or empty queue waits the whole timeout and gives up. Under TSan a
			// timed wait costs about 100 ms, so only a few runs take them.
			if (timed) {
				r = (op == 77) ? q.push_back(static_cast<int>(next), 0.0005) : q.pop_back(v, 0.0005);
				out.push_back(r);
				if (op == 77) { next += r; } else { out.push_back(r ? v : -1); }
			}
		} else if (op < 80) {
			r = q.pop_back(v, 0.0);
			out.push_back(r);
			out.push_back(r ? v : -1);
		} else if (op < 84) {
			q.clear();
		} else if (op < 85) {
			q.end();
			ended = true;
		} else if (op < 86 && i > steps / 2) {
			q.finish();
			finished = true;
		}
		observe();
	}
	return out;
}

// Move assignment, the one deliberate difference. A is replaced by B; C stays on A's old state. The
// frozen copy left A's items in the old state's count, so there count() reads high by A's size at
// the assignment; everything else matches. The oracle is the items the state holds: A's and C's
// before the assignment, C's after. The old
// state is held here, so the frozen copy's assignment (which unlocked a destroyed mutex when it
// dropped the last owner) runs on a live state.
struct MoveTrace {
	std::vector<long long> same;      // must match the frozen copy
	std::vector<long long> c_count;   // C's count() after each step: differs from the move on
	std::vector<long long> live;      // items on the old state (A's while it is there, and C's): the oracle
	long long leaked = 0;             // A's size at the assignment
	std::size_t move_step = 0;        // the step that assigned
};

template <typename Q, typename S>
static MoveTrace move_trace(std::uint64_t seed, int steps) {
	auto old_state = std::make_shared<S>();
	Q a(old_state), c(old_state), b;
	std::mt19937_64 rng(seed);
	MoveTrace t;
	int next = 1;
	bool moved = false;
	for (int i = 0; i < steps; ++i) {
		const unsigned op = static_cast<unsigned>(rng() % 100);
		// B is moved from after the assignment, so only A and C take operations then.
		Q& q = (op & 1) ? c : ((op & 2) || moved ? a : b);
		int v = 0;
		if (op < 40) {
			t.same.push_back(q.push_back(next++, 0.0));
		} else if (op < 50) {
			t.same.push_back(q.push_front(next++, 0.0));
		} else if (op < 75) {
			bool r = q.pop_front(v, 0.0);
			t.same.push_back(r);
			t.same.push_back(r ? v : -1);
		} else if (op < 85) {
			bool r = q.pop_back(v, 0.0);
			t.same.push_back(r);
			t.same.push_back(r ? v : -1);
		} else if (op < 88) {
			q.clear();
		} else if (!moved && i > steps / 3) {
			t.leaked = static_cast<long long>(a.size());
			t.move_step = t.c_count.size();
			a = std::move(b);
			moved = true;
		}
		t.same.push_back(static_cast<long long>(a.size()));
		t.same.push_back(static_cast<long long>(a.count()));
		if (!moved) {
			t.same.push_back(static_cast<long long>(b.size()));
			t.same.push_back(static_cast<long long>(b.count()));
		}
		t.c_count.push_back(static_cast<long long>(c.count()));
		t.live.push_back(static_cast<long long>(c.size() + (moved ? 0 : a.size())));
	}
	t.same.push_back(moved);
	return t;
}

int main() {
	static const Shape shapes[] = {
		{unlimited, unlimited, unlimited},  // unbounded
		{4, unlimited, unlimited},          // hard limit only
		{4, 2, unlimited},                  // soft limit below the hard limit
		{3, 5, unlimited},                  // soft limit above the hard limit
		{8, 3, 2},                          // all three
		{1, 1, 1},                          // one slot
		{0, unlimited, unlimited},          // never any room, except by force
		{unlimited, 0, unlimited},          // soft limit of zero
	};
	for (const Shape& shape : shapes) {
		for (std::uint64_t seed = 1; seed <= 25; ++seed) {
			const bool timed = seed == 1;
			auto a = trace<queue::Queue<int>>(shape, seed, 300, timed);
			auto b = trace<queue_frozen::Queue<int>>(shape, seed, 300, timed);
			CHECK(a.size() == b.size());
			CHECK(a == b);
		}
	}
	int differing = 0;
	for (std::uint64_t seed = 1; seed <= 200; ++seed) {
		auto n = move_trace<queue::Queue<int>, queue::QueueState>(seed, 120);
		auto o = move_trace<queue_frozen::Queue<int>, queue_frozen::QueueState>(seed, 120);
		CHECK(n.same == o.same);
		CHECK(n.leaked == o.leaked && n.move_step == o.move_step);
		CHECK(n.live == o.live);
		CHECK(n.c_count == n.live);  // new == oracle: the state counts the items it holds
		for (std::size_t i = 0; i < o.c_count.size(); ++i) {
			// Before the move they match; from it on the frozen count reads high by A's size.
			bool after = o.leaked && i >= o.move_step;
			CHECK(o.c_count[i] == n.c_count[i] + (after ? o.leaked : 0));
		}
		differing += o.leaked > 0;
	}
	CHECK(differing > 50);  // old != new in most runs: A held items when it was replaced
	if (failures) { return 1; }
	std::puts("all queue differential tests passed");
	return 0;
}
