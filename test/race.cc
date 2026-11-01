// end() and finish() must reach a waiter that has looked at its predicate and not yet blocked.
//
// A waiter holds the mutex from its last look at the predicate until it is inside wait(). end() and
// finish() set their flag without the mutex, so a broadcast that did not take it could land in that
// window and be lost: an untimed waiter would sleep for good. `_wake_all` takes and releases the
// mutex first, which holds the broadcast back until the waiter has released it inside wait().
//
// No schedule hits the window reliably, so the test makes it: QUEUE_TEST_HOOK (queue.h) runs in
// the waiter with the mutex held, right before it blocks, and releases a second thread that calls
// end() or finish(); the hook then waits up to 300 ms for that call to return. With the mutex taken
// in `_wake_all` the call cannot return while the waiter holds it, the hook times out, the waiter
// blocks and the call completes and wakes it. Without it the call returns inside the window, the
// notify finds nobody waiting, and the waiter sleeps through the flag it already missed: the case
// fails at its deadline.
//
// Four cases: a consumer on an empty queue and a producer on a full one, each stopped by end() and
// by finish(). `queue_race` runs them; it is quick, and deterministic.
//
// Build: c++ -std=c++20 -O1 -pthread -I.. test/race.cc

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

enum RacePoint { no_point = 0, pop_blocking, push_blocking };

namespace race {
	std::atomic<int> target{no_point};
	std::atomic<bool> fired{false}, at_window{false}, stop_returned{false};

	// Called by the waiter in the window; only the first call at the target point acts.
	void hook(int point) {
		if (point != target.load() || fired.exchange(true)) {
			return;
		}
		at_window = true;
		auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		while (!stop_returned && std::chrono::steady_clock::now() < end) {
			std::this_thread::yield();
		}
	}
}  // namespace race

#define QUEUE_TEST_HOOK(point) race::hook(point)
#include "queue.h"

using namespace std::chrono;

// consumer: a pop on an empty queue, else a push on a full one. finish: finish(), else end().
static bool run(bool consumer, bool finish) {
	const char* name = consumer ? (finish ? "finish() reaches a consumer in the window" : "end() reaches a consumer in the window")
	                            : (finish ? "finish() reaches a producer in the window" : "end() reaches a producer in the window");
	queue::Queue<int> q(consumer ? static_cast<std::size_t>(-1) : 1);
	if (!consumer) {
		q.push_back(1);
	}
	race::fired = race::at_window = race::stop_returned = false;
	race::target = consumer ? pop_blocking : push_blocking;

	std::atomic<bool> returned{false}, result{true};
	std::thread stopper([&] {
		while (!race::at_window) {
			std::this_thread::yield();
		}
		if (finish) {
			q.finish();
		} else {
			q.end();
		}
		race::stop_returned = true;
	});
	std::thread waiter([&] {
		int v = 0;
		result = consumer ? q.pop_front(v) : q.push_back(2);
		returned = true;
	});

	auto deadline = steady_clock::now() + seconds(5);
	while (!returned && steady_clock::now() < deadline) {
		std::this_thread::sleep_for(milliseconds(1));
	}
	bool ok = returned && !result;
	if (!returned) {
		q.finish();  // release the waiter that missed the wake-up
	}
	waiter.join();
	if (!race::fired) {
		race::at_window = true;  // the waiter never reached the hook: free the stopper
	}
	stopper.join();
	std::printf("  %-48s %s\n", name, ok ? "OK" : (returned ? "FAIL: the call did not fail" : "FAIL: the waiter slept through it"));
	return ok;
}

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("queue race: a stop between the last look at the predicate and the block\n");
	bool ok = true;
	for (bool consumer : {true, false}) {
		for (bool finish : {false, true}) {
			ok &= run(consumer, finish);
		}
	}
	std::printf("  RESULT: %s\n", ok ? "OK" : "FAIL");
	return ok ? 0 : 1;
}
