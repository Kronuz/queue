/*
 * work_queue: a runnable, self-checking example for the queue API shape Xapiand
 * uses for worker hand-off queues.
 *
 * It doubles as a REGRESSION TEST: every step uses CHECK(...), and main()
 * returns non-zero on failure, so CMake registers it with ctest. The scenario is
 * deliberately Xapiand-shaped: producers submit bounded work to a pool of
 * consumers, urgent work can enter at the front, workers consume from both ends,
 * empty pops can time out, and end()/finish() shut the queue down cleanly.
 */

#include "queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static std::atomic<int> failures{0};

#define CHECK(x) do { \
		if (!(x)) { \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
			failures.fetch_add(1, std::memory_order_relaxed); \
		} \
	} while (0)

struct WorkItem {
	uint64_t id;
	uint64_t cost;
	bool urgent;
};

static uint64_t cost_for(uint64_t id) {
	return id * 37 + 11;
}

static bool is_urgent(uint64_t id) {
	return id % 17 == 0;
}

static void check_empty_pop_times_out() {
	queue::Queue<WorkItem> q(/*hard_limit*/ 4);
	WorkItem item{};

	auto t0 = std::chrono::steady_clock::now();
	bool popped = q.pop_front(item, 0.02);
	auto waited = std::chrono::steady_clock::now() - t0;

	CHECK(!popped);
	CHECK(waited >= 5ms);
	CHECK(q.empty());
	std::puts("work_queue OK: timed pop returns false on an empty queue");
}

static void check_double_ended_priority_order() {
	queue::Queue<WorkItem> q(/*hard_limit*/ 4);

	CHECK(q.push_back(WorkItem{1, cost_for(1), false}, 0.0));
	CHECK(q.push_back(WorkItem{2, cost_for(2), false}, 0.0));
	CHECK(q.push_front(WorkItem{17, cost_for(17), true}, 0.0));

	WorkItem item{};
	CHECK(q.pop_front(item, 0.0) && item.id == 17 && item.urgent);
	CHECK(q.pop_back(item, 0.0) && item.id == 2 && !item.urgent);
	CHECK(q.pop_front(item, 0.0) && item.id == 1 && !item.urgent);
	CHECK(q.empty());
	std::puts("work_queue OK: push_front gives urgent work priority, pop_back steals the tail");
}

static void check_bounded_capacity_blocks() {
	queue::Queue<int> q(/*hard_limit*/ 2);
	CHECK(q.push_back(1));
	CHECK(q.push_back(2));

	std::atomic<bool> started{false};
	std::atomic<bool> returned{false};
	std::atomic<bool> pushed{false};

	std::thread producer([&] {
		started.store(true, std::memory_order_release);
		pushed.store(q.push_back(3, 1.0), std::memory_order_release);
		returned.store(true, std::memory_order_release);
	});

	for (int i = 0; i < 100 && !started.load(std::memory_order_acquire); ++i) {
		std::this_thread::sleep_for(1ms);
	}
	CHECK(started.load(std::memory_order_acquire));

	std::this_thread::sleep_for(30ms);
	CHECK(!returned.load(std::memory_order_acquire));

	int value = 0;
	CHECK(q.pop_front(value, 0.0) && value == 1);

	producer.join();
	CHECK(returned.load(std::memory_order_acquire));
	CHECK(pushed.load(std::memory_order_acquire));

	CHECK(q.pop_front(value, 0.0) && value == 2);
	CHECK(q.pop_front(value, 0.0) && value == 3);
	CHECK(q.empty());
	std::puts("work_queue OK: bounded push blocks while full and resumes when room appears");
}

static void check_worker_pool_handoff() {
	constexpr size_t producer_count = 4;
	constexpr size_t consumer_count = 4;
	constexpr uint64_t items_per_producer = 128;
	constexpr uint64_t total_items = producer_count * items_per_producer;

	queue::Queue<WorkItem> q(/*hard_limit*/ 13);
	std::vector<std::atomic<unsigned>> deliveries(total_items + 1);
	for (auto& seen : deliveries) {
		seen.store(0, std::memory_order_relaxed);
	}

	uint64_t expected_sum = 0;
	uint64_t expected_urgent = 0;
	for (uint64_t id = 1; id <= total_items; ++id) {
		expected_sum += cost_for(id);
		if (is_urgent(id)) {
			++expected_urgent;
		}
	}

	std::atomic<uint64_t> produced_count{0};
	std::atomic<uint64_t> produced_sum{0};
	std::atomic<uint64_t> urgent_pushes{0};
	std::atomic<uint64_t> consumed_count{0};
	std::atomic<uint64_t> consumed_sum{0};
	std::atomic<uint64_t> consumed_urgent{0};
	std::atomic<uint64_t> invalid_items{0};
	std::atomic<uint64_t> back_pops{0};
	std::atomic<uint64_t> consumers_done{0};

	std::vector<std::thread> consumers;
	consumers.reserve(consumer_count);
	for (size_t consumer = 0; consumer < consumer_count; ++consumer) {
		consumers.emplace_back([&, consumer] {
			const bool steal_from_back = (consumer % 2) == 1;
			WorkItem item{};
			while (steal_from_back ? q.pop_back(item) : q.pop_front(item)) {
				if (steal_from_back) {
					back_pops.fetch_add(1, std::memory_order_relaxed);
				}
				if (item.id == 0 || item.id > total_items) {
					invalid_items.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				unsigned previous = deliveries[item.id].fetch_add(1, std::memory_order_relaxed);
				if (previous != 0) {
					CHECK(previous == 0);
				}
				consumed_count.fetch_add(1, std::memory_order_relaxed);
				consumed_sum.fetch_add(item.cost, std::memory_order_relaxed);
				if (item.urgent) {
					consumed_urgent.fetch_add(1, std::memory_order_relaxed);
				}
			}
			consumers_done.fetch_add(1, std::memory_order_relaxed);
		});
	}

	std::vector<std::thread> producers;
	producers.reserve(producer_count);
	for (size_t producer = 0; producer < producer_count; ++producer) {
		producers.emplace_back([&, producer] {
			const uint64_t first = producer * items_per_producer + 1;
			const uint64_t last = first + items_per_producer;
			for (uint64_t id = first; id < last; ++id) {
				WorkItem item{id, cost_for(id), is_urgent(id)};
				bool pushed = item.urgent ? q.push_front(item) : q.push_back(item);
				if (!pushed) {
					CHECK(pushed);
					continue;
				}
				produced_count.fetch_add(1, std::memory_order_relaxed);
				produced_sum.fetch_add(item.cost, std::memory_order_relaxed);
				if (item.urgent) {
					urgent_pushes.fetch_add(1, std::memory_order_relaxed);
				}
			}
		});
	}

	for (auto& producer : producers) {
		producer.join();
	}

	CHECK(produced_count.load(std::memory_order_relaxed) == total_items);
	CHECK(produced_sum.load(std::memory_order_relaxed) == expected_sum);
	CHECK(urgent_pushes.load(std::memory_order_relaxed) == expected_urgent);

	q.end();

	for (auto& consumer : consumers) {
		consumer.join();
	}

	CHECK(consumers_done.load(std::memory_order_relaxed) == consumer_count);
	CHECK(q.empty());
	CHECK(q.count() == 0);
	CHECK(consumed_count.load(std::memory_order_relaxed) == total_items);
	CHECK(consumed_sum.load(std::memory_order_relaxed) == expected_sum);
	CHECK(consumed_urgent.load(std::memory_order_relaxed) == expected_urgent);
	CHECK(invalid_items.load(std::memory_order_relaxed) == 0);

	uint64_t missing = 0;
	uint64_t duplicates = 0;
	for (uint64_t id = 1; id <= total_items; ++id) {
		unsigned seen = deliveries[id].load(std::memory_order_relaxed);
		if (seen == 0) {
			++missing;
		} else if (seen > 1) {
			duplicates += seen - 1;
		}
	}
	CHECK(missing == 0);
	CHECK(duplicates == 0);

	CHECK(!q.push_back(WorkItem{total_items + 1, cost_for(total_items + 1), false}, 0.0));
	q.finish();
	WorkItem item{};
	CHECK(!q.pop_front(item, 0.0));

	std::printf(
		"work_queue OK: %llu items delivered exactly once through %zu producers/%zu consumers "
		"(urgent=%llu, tail-steals=%llu)\n",
		(unsigned long long)total_items,
		producer_count,
		consumer_count,
		(unsigned long long)consumed_urgent.load(std::memory_order_relaxed),
		(unsigned long long)back_pops.load(std::memory_order_relaxed));
}

int main() {
	check_empty_pop_times_out();
	check_double_ended_priority_order();
	check_bounded_capacity_blocks();
	check_worker_pool_handoff();

	if (failures.load(std::memory_order_relaxed) == 0) {
		std::puts("work_queue: all checks passed (timeouts + bounds + both ends + end/finish)");
	}
	return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
