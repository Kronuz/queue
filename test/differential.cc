// Differential test: the current Queue against the frozen copy of queue.h in test/queue_frozen.h.
//
// The same seeded sequence of single-threaded operations runs on both and records every observable
// result: each call's return value, the value it popped, and size(), count() and empty() after it.
// The traces must be identical. The sequences cover every limit shape (unbounded, hard limit,
// soft limit below and above the hard limit, a hard limit of zero), both ends, force, the non-blocking
// and timed regimes, blocking calls that cannot block, clear, end and finish.
//
// Build: c++ -std=c++20 -I.. test/differential.cc

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
	if (failures) { return 1; }
	std::puts("all queue differential tests passed");
	return 0;
}
