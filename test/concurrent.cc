// Concurrent stress test for the standalone queue library.
//
// N producers push and M consumers pop concurrently; every item pushed must be
// received exactly once. The accounting is a count plus an XOR checksum over a
// dense id space: a lost item leaves the running XOR non-zero, a double-delivered
// item flips a bit back, and the consumed count catches plain duplication or
// loss. A torn read (two consumers popping the same slot) or a push racing a pop
// shows up as heap corruption under ASan and as a data race under TSan.
//
// Three scenarios run in sequence:
//   - unbounded MPMC: a large item count through a queue with no real cap, the
//     plain throughput-shaped path.
//   - bounded MPMC: a small hard_limit so producers actually park on a full
//     queue and get woken by consumers (exercises the push-blocking path).
//   - finish() mid-flight: producers and consumers running, then finish() from
//     a third party; the run must wind down with no hang and no crash, and every
//     item the consumers reported as received must reconcile against what was
//     actually pushed before the queue was finished.
//
// This is the binary to run under sanitizers:
//   c++ -std=c++20 -O1 -g -I.. test/concurrent.cc
//   + -fsanitize=address : torn reads / use-after-free / leaks  (Homebrew LLVM)
//   + -fsanitize=thread  : data races                           (Homebrew LLVM)
//
// A fatal signal self-reports "RESULT: FAIL (fatal signal)" and exits non-zero,
// so a crash can never be miscounted as a pass by a grep-for-FAIL harness.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <vector>
#include <unistd.h>

#include "queue.h"

// write() is the only async-signal-safe output here; printf is not. A fatal
// signal (a torn read manifests as SIGSEGV/SIGBUS) prints a FAIL line and exits
// non-zero so it is never read as a pass.
static void crash_handler(int) {
	static const char msg[] = "  RESULT: FAIL - crashed (fatal signal)\n";
	ssize_t r = ::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
	(void)r;
	_Exit(134);
}

// A sentinel pushed once per consumer so each consumer has a clean stop signal
// after every real item has been pushed. Real ids are 1..total; 0 is the poison.
static constexpr uint64_t POISON = 0;

// Scenario 1 + 2: push ids [1, total] across `nprod` producers, drain with
// `ncons` consumers, and verify every id is received exactly once. `cap` of 0
// means effectively unbounded; a small cap forces producers to block on full.
static bool run_mpmc(const char* name, int nprod, int ncons, uint64_t total, size_t cap) {
	queue::Queue<uint64_t> q(cap ? cap : (size_t)-1);

	std::atomic<uint64_t> next_id{1};                 // hand out ids 1..total
	std::atomic<uint64_t> consumed_count{0};
	std::atomic<uint64_t> consumed_xor{0};

	// Reference checksum over 1..total, computed independently of the queue.
	uint64_t expect_xor = 0;
	for (uint64_t i = 1; i <= total; ++i) expect_xor ^= i;

	std::vector<std::thread> producers;
	for (int p = 0; p < nprod; ++p) {
		producers.emplace_back([&] {
			for (;;) {
				uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
				if (id > total) break;
				q.push_back(id);                      // block-forever; waits if full
			}
		});
	}

	std::vector<std::thread> consumers;
	for (int c = 0; c < ncons; ++c) {
		consumers.emplace_back([&] {
			uint64_t v = 0;
			while (q.pop_front(v)) {
				if (v == POISON) break;               // clean stop
				consumed_count.fetch_add(1, std::memory_order_relaxed);
				consumed_xor.fetch_xor(v, std::memory_order_relaxed);
			}
		});
	}

	for (auto& t : producers) t.join();
	// Every real item is now in the queue (or already consumed). Feed one poison
	// per consumer so each gets a guaranteed stop after draining the reals.
	for (int c = 0; c < ncons; ++c) q.push_back(POISON);
	for (auto& t : consumers) t.join();

	uint64_t got = consumed_count.load();
	uint64_t gx = consumed_xor.load();
	bool ok = (got == total) && (gx == expect_xor);
	std::printf("  %-22s prod=%d cons=%d total=%llu cap=%zu -> consumed=%llu xor=%016llx  %s\n",
	            name, nprod, ncons, (unsigned long long)total, cap,
	            (unsigned long long)got, (unsigned long long)gx,
	            ok ? "OK" : "FAIL");
	return ok;
}

// Scenario 3: producers and consumers run freely; a third party calls finish()
// to tear the queue down mid-flight. Correctness here is liveness plus
// reconciliation, not full delivery: once finish() fires, in-flight pushes and
// pops return false, so what matters is that nothing hangs or crashes and that
// every item a consumer reported as received was one a producer reported as
// successfully pushed (count + XOR match between the two sides). A torn read or
// a push/pop race during the shutdown still trips ASan/TSan.
static bool run_finish_midflight(int nprod, int ncons) {
	queue::Queue<uint64_t> q(/*hard_limit*/ 1024);

	std::atomic<uint64_t> next_id{1};
	std::atomic<uint64_t> pushed_count{0}, pushed_xor{0};
	std::atomic<uint64_t> popped_count{0}, popped_xor{0};
	std::atomic<bool> stop{false};

	std::vector<std::thread> producers;
	for (int p = 0; p < nprod; ++p) {
		producers.emplace_back([&] {
			while (!stop.load(std::memory_order_acquire)) {
				uint64_t id = next_id.fetch_add(1, std::memory_order_relaxed);
				if (q.push_back(id)) {                // false once finished/full-timeout
					pushed_count.fetch_add(1, std::memory_order_relaxed);
					pushed_xor.fetch_xor(id, std::memory_order_relaxed);
				}
			}
		});
	}

	std::vector<std::thread> consumers;
	for (int c = 0; c < ncons; ++c) {
		consumers.emplace_back([&] {
			uint64_t v = 0;
			while (q.pop_front(v)) {                  // returns false when finished+empty
				popped_count.fetch_add(1, std::memory_order_relaxed);
				popped_xor.fetch_xor(v, std::memory_order_relaxed);
			}
		});
	}

	// Let the queue churn briefly, then finish() it from here. finish() wakes
	// every parked producer and consumer; the consumers' pop loop then exits as
	// soon as the queue drains.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	q.finish();
	stop.store(true, std::memory_order_release);

	for (auto& t : producers) t.join();
	for (auto& t : consumers) t.join();

	// Reconcile the two sides. Everything a consumer popped was something a
	// producer pushed, so popped is a subset of pushed: popped_count <=
	// pushed_count, and the items left abandoned in the queue at finish() are
	// exactly (pushed minus popped). The queue still answers count() after
	// finish(), so that leftover count must equal pushed_count - popped_count.
	// The XOR of the two sides is the checksum of those abandoned items; when no
	// items are left it must be zero (a non-zero XOR with a zero leftover count
	// would mean a popped id was never pushed, i.e. a torn/duplicated read).
	uint64_t left_count = pushed_count.load() - popped_count.load();
	uint64_t leftover_xor = pushed_xor.load() ^ popped_xor.load();

	bool counts_ok = (popped_count.load() <= pushed_count.load());
	bool leftover_consistent = (left_count == q.count());
	bool xor_ok = (left_count != 0) || (leftover_xor == 0);

	bool ok = counts_ok && leftover_consistent && xor_ok;
	std::printf("  %-22s prod=%d cons=%d -> pushed=%llu popped=%llu left=%llu (q.count=%llu)  %s\n",
	            "finish-midflight", nprod, ncons,
	            (unsigned long long)pushed_count.load(),
	            (unsigned long long)popped_count.load(),
	            (unsigned long long)left_count,
	            (unsigned long long)q.count(),
	            ok ? "OK" : "FAIL");
	return ok;
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::signal(SIGSEGV, crash_handler);
	std::signal(SIGBUS, crash_handler);
	std::signal(SIGABRT, crash_handler);

	// Sanitizer runs are ~10-20x slower; allow scaling the item count down so a
	// TSan pass still finishes in seconds. Default is the full count.
	uint64_t total = (argc > 1) ? (uint64_t)atoll(argv[1]) : 1000000ULL;

	std::printf("queue concurrent stress (total=%llu items)\n", (unsigned long long)total);

	bool ok = true;
	// Unbounded MPMC at two thread shapes.
	ok &= run_mpmc("unbounded 4x4", 4, 4, total, /*cap*/ 0);
	ok &= run_mpmc("unbounded 8x8", 8, 8, total, /*cap*/ 0);
	// Bounded MPMC: a small cap forces producers to park on a full queue.
	ok &= run_mpmc("bounded(64) 4x4", 4, 4, total, /*cap*/ 64);
	ok &= run_mpmc("bounded(16) 8x8", 8, 8, total, /*cap*/ 16);
	// finish() mid-flight.
	ok &= run_finish_midflight(4, 4);
	ok &= run_finish_midflight(8, 8);

	std::printf("  RESULT: %s\n", ok ? "OK - every item received exactly once" :
	            "FAIL - items lost, double-delivered, or reconciliation broke");
	return ok ? 0 : 1;
}
