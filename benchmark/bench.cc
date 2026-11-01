// Throughput benchmark for the single-ended MPMC path (push_back / pop_front).
//
// The same harness drives three blocking-queue implementations so the numbers
// are apples-to-apples on the one feature they all share -- a bounded-ish
// blocking MPMC channel of ints:
//
//   (a) queue::Queue<int>          -- this library, run effectively unbounded.
//   (b) MutexDeque<int>            -- a ~30-line std::mutex + condition_variable
//                                     + std::deque baseline. The control: it
//                                     answers "is our mutex impl in the right
//                                     ballpark for a naive lock-based queue?".
//   (c) moodycamel::BlockingConcurrentQueue<int>
//                                  -- the fast lock-free-ish alternative.
//
// Each run: `total` items handed out across `nprod` producers, drained by
// `ncons` consumers that each pop a fixed share of them (the shares add up to
// `total`, so no consumer needs a stop marker: a blocking queue without a
// global order, like (c), could deliver a marker before the last items).
// Throughput is total / wall-clock of the push+drain, in million items/sec,
// best of a few runs. Every run checks that the consumers received each item
// exactly once (the sum of the ids), so all three deliver the same items.
// The comparison is only fair on this shared subset: (c) has no
// double-ended access, no per-op timeouts, and no graceful finish(), which are
// the reasons (a) exists.
//
// Build: cmake -B build -DQUEUE_BUILD_BENCH=ON && cmake --build build --target queue_bench
// (fetches cameron314/concurrentqueue at a pinned commit, for this target only).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "queue.h"
#include "blockingconcurrentqueue.h"

using Clock = std::chrono::steady_clock;

// (b) The minimal baseline: a textbook blocking MPMC queue. Unbounded (we are
// measuring the hand-off path, not back-pressure), single condition variable.
template <typename T>
class MutexDeque {
	std::mutex _m;
	std::condition_variable _cv;
	std::deque<T> _q;
public:
	void push(T v) {
		{
			std::lock_guard<std::mutex> lk(_m);
			_q.push_back(std::move(v));
		}
		_cv.notify_one();
	}
	T pop() {
		std::unique_lock<std::mutex> lk(_m);
		_cv.wait(lk, [&] { return !_q.empty(); });
		T v = std::move(_q.front());
		_q.pop_front();
		return v;
	}
};

// Adapters give all three the same push(int) / pop()->int shape so one harness
// drives them. queue::Queue and moodycamel both report success via bool; the
// baseline returns by value. A pop that fails returns 0, which the id-sum check
// counts as a missing item.

struct QueueAdapter {
	static constexpr const char* name = "queue::Queue<int>";
	queue::Queue<int> q;                         // effectively unbounded
	void push(int v) { q.push_back(v); }
	int pop() { int v = 0; q.pop_front(v); return v; }
};

struct MutexAdapter {
	static constexpr const char* name = "mutex+condvar+deque";
	MutexDeque<int> q;
	void push(int v) { q.push(v); }
	int pop() { return q.pop(); }
};

struct MoodycamelAdapter {
	static constexpr const char* name = "moodycamel::Blocking";
	moodycamel::BlockingConcurrentQueue<int> q;
	void push(int v) { q.enqueue(v); }
	int pop() { int v = 0; q.wait_dequeue(v); return v; }
};

template <typename Adapter>
static double run_once(int nprod, int ncons, int total) {
	Adapter a;
	std::atomic<int> next_id{0};

	auto t0 = Clock::now();

	std::vector<std::thread> producers;
	for (int p = 0; p < nprod; ++p) {
		producers.emplace_back([&] {
			for (;;) {
				int id = next_id.fetch_add(1, std::memory_order_relaxed);
				if (id >= total) break;
				a.push(id + 1);                      // ids 1..total
			}
		});
	}

	std::atomic<long long> received{0}, id_sum{0};
	std::vector<std::thread> consumers;
	for (int c = 0; c < ncons; ++c) {
		const int share = total / ncons + (c < total % ncons ? 1 : 0);
		consumers.emplace_back([&, share] {
			long long sum = 0;
			for (int i = 0; i < share; ++i) sum += a.pop();
			received.fetch_add(share, std::memory_order_relaxed);
			id_sum.fetch_add(sum, std::memory_order_relaxed);
		});
	}

	for (auto& t : producers) t.join();
	for (auto& t : consumers) t.join();

	auto dt = std::chrono::duration<double>(Clock::now() - t0).count();
	if (received != total || id_sum != (long long)total * (total + 1) / 2) {
		std::fprintf(stderr, "MISMATCH in %s (%dx%d): id sum %lld, expected %lld\n", Adapter::name, nprod, ncons,
		             id_sum.load(), (long long)total * (total + 1) / 2);
		std::abort();
	}
	return (double)total / dt / 1e6;             // million items/sec
}

template <typename Adapter>
static double best_of(int nprod, int ncons, int total, int runs) {
	double best = 0.0;
	for (int r = 0; r < runs; ++r) {
		double m = run_once<Adapter>(nprod, ncons, total);
		if (m > best) best = m;
	}
	return best;
}

int main(int argc, char** argv) {
	int total = (argc > 1) ? atoi(argv[1]) : 2000000;
	int runs = (argc > 2) ? atoi(argv[2]) : 3;

	struct Shape { int p, c; };
	const Shape shapes[] = {{1, 1}, {2, 2}, {4, 4}, {8, 8}};

	std::printf("single-ended MPMC throughput (push_back/pop_front), Mitems/sec\n");
	std::printf("total=%d items, best of %d runs\n\n", total, runs);
	std::printf("%-26s %10s %10s %10s %10s\n",
	            "impl", "1x1", "2x2", "4x4", "8x8");

	auto row = [&](const char* name, auto tag) {
		using A = typename decltype(tag)::type;
		std::printf("%-26s", name);
		for (auto s : shapes) {
			double m = best_of<A>(s.p, s.c, total, runs);
			std::printf(" %10.2f", m);
		}
		std::printf("\n");
	};

	// A tiny tag wrapper so `row` can take a type without spelling the template
	// arg at every call.
	struct QTag { using type = QueueAdapter; };
	struct MTag { using type = MutexAdapter; };
	struct CTag { using type = MoodycamelAdapter; };

	row("queue::Queue<int>", QTag{});
	row("mutex+condvar+deque", MTag{});
	row("moodycamel::Blocking", CTag{});

	return 0;
}
