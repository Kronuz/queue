#!/usr/bin/env python3
"""Break queue.h in many ways and require the tests to notice.

Each mutant edits queue.h in a scratch copy of the repository, builds the test programs from
that copy and runs them. A mutant is killed when a test program fails, exits on its watchdog
or times out. The script exits non-zero if any mutant
survives, stops building, or no longer matches its source text, so a test that stops pinning a
wake-up shows up here.

The mutants are the ways a state change can leave a parked thread asleep that the tests in
test/liveness.cc, test/concurrent.cc and test/test.cc pin: a missing or misplaced notify, a wait
that is not counted, a wake that is not handed on, and a count that is not given back.

usage: tools/mutants.py [--cxx c++] [--jobs N] [--filter SUBSTRING] [--all]
  --all  run every test program on a mutant instead of stopping at the first one that fails
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = "\t"

# (id, old text, new text, what it breaks[, number of matches])
MUTANTS = [
    ("wake-all-no-lock", "{ std::lock_guard<std::mutex> lk(_state->_mutex); }", "",
     "end() and finish() broadcast without the empty critical section: a waiter between its predicate and its wait misses it"),
    ("wake-all-no-consumers",
     "{ std::lock_guard<std::mutex> lk(_state->_mutex); }\n" + 3 * T + "_pop_cond.notify_all();",
     "{ std::lock_guard<std::mutex> lk(_state->_mutex); }",
     "end() and finish() do not wake the consumers"),
    ("wake-all-no-producers",
     "_pop_cond.notify_all();\n" + 3 * T + "_state->_push_cond.notify_all();\n" + 2 * T + "}\n\n" + 1 * T + "public:",
     "_pop_cond.notify_all();\n" + 2 * T + "}\n\n" + 1 * T + "public:",
     "end() and finish() do not wake the producers"),
    ("untimed-consumer-uncounted", "_pop_cond.wait(lk);",
     "--_pop_waiters; _pop_cond.wait(lk); ++_pop_waiters;",
     "an untimed consumer is not counted while it waits, so a push skips the notify (M2)"),
    ("untimed-producer-uncounted", "_state->_push_cond.wait(lk);",
     "--_state->_push_waiters; _state->_push_cond.wait(lk); ++_state->_push_waiters;",
     "an untimed producer is not counted while it waits, so a pop skips the notify (M2p)"),
    ("push-back-no-wake", "bool pushed = _push_back_impl(std::forward<E>(element), timeout, lk, force);",
     "bool pushed = _push_back_impl(std::forward<E>(element), timeout, lk, force); if (pushed) { return true; }",
     "push_back does not wake a consumer"),
    ("push-front-no-wake", "bool pushed = _push_front_impl(std::forward<E>(element), timeout, lk, force);",
     "bool pushed = _push_front_impl(std::forward<E>(element), timeout, lk, force); if (pushed) { return true; }",
     "push_front does not wake a consumer"),
    ("queueset-push-no-wake", "Queue_t::_pop_cond.notify_one();", "(void)0;",
     "QueueSet::push does not wake a consumer"),
    ("shared-pop-condvar", "std::condition_variable _pop_cond;", "static inline std::condition_variable _pop_cond;",
     "every Queue's consumers wait on one condition variable: a push on A can wake only B's consumer (CA1)"),
    ("pop-with-timeout-no-wake",
     "auto wake = popped ? _state->_room_freed(1) : QueueState::_wake::none;",
     "auto wake = popped && timeout < 0 ? _state->_room_freed(1) : QueueState::_wake::none;",
     "a pop with a deadline or without waiting does not wake a producer", 2),
    ("pop-gated-by-threshold", "_cnt.load(std::memory_order_relaxed) >= _hard_limit",
     "_cnt.load(std::memory_order_relaxed) >= _threshold",
     "a pop wakes a producer only below the threshold, not below the hard limit (CA1)"),
    ("clear-wakes-one", "auto wake = _state->_room_freed(_clear_impl(lk));",
     "auto wake = _state->_room_freed(_clear_impl(lk) ? 1 : 0);",
     "clear() frees every slot but wakes one producer (CA1)"),
    ("clear-one-slot-no-wake", "auto wake = _state->_room_freed(_clear_impl(lk));",
     "auto freed = _clear_impl(lk); auto wake = freed > 1 ? _state->_room_freed(freed) : QueueState::_wake::none;",
     "clear() of a single slot wakes nobody"),
    ("queueset-clear-wakes-one", "Queue_t::_state->_room_freed(Queue_t::_clear_impl(lk))",
     "Queue_t::_state->_room_freed(Queue_t::_clear_impl(lk) ? 1 : 0)",
     "QueueSet::clear() frees every slot but wakes one producer"),
    ("queueset-erase-no-wake", "auto wake = Queue_t::_state->_room_freed(1);",
     "auto wake = QueueState::_wake::none;", "QueueSet::erase frees a slot and wakes nobody (CA1)"),
    ("queueset-pop-no-wake", "auto wake = popped ? Queue_t::_state->_room_freed(1) : QueueState::_wake::none;",
     "auto wake = QueueState::_wake::none;", "QueueSet::pop frees a slot and wakes nobody"),
    ("destroy-running-no-wake",
     "_finished = true;\n" + 4 * T + "_pop_cond.notify_all();\n" + 4 * T + "_state->_push_cond.notify_all();",
     "bool was_finished = _finished; _finished = true; _pop_cond.notify_all();\n" + 4 * T +
     "if (was_finished) { _state->_push_cond.notify_all(); }",
     "the destructor of a Queue that is not finished does not wake the producers on its state"),
    ("destroy-finished-no-wake",
     "_finished = true;\n" + 4 * T + "_pop_cond.notify_all();\n" + 4 * T + "_state->_push_cond.notify_all();",
     "bool was_finished = _finished; _finished = true;\n" + 4 * T +
     "if (!was_finished) { _pop_cond.notify_all(); _state->_push_cond.notify_all(); }",
     "the destructor of a finished Queue does not wake the producers on its state (CA1)"),
    ("move-assign-count-leak", "old->_cnt_sub(size);", "(void)size;",
     "move assignment keeps the target's items in its state's count"),
    ("move-assign-no-wake", "old->_wake_pushers(wake);", "(void)wake;",
     "move assignment frees the target's slots and wakes no producer of its old state"),
    ("move-assign-self", "if (this == &q) {", "if (false) {",
     "self move-assignment drops the Queue's items"),
    ("hand-on-producer", "q->_state->_wake_pushers(q->_state->_room_freed(1));", "(void)0;",
     "a woken producer whose insert throws does not wake the next one"),
    ("hand-on-consumer", "q->_pop_cond.notify_one();", "(void)0;",
     "a woken consumer whose take throws does not wake the next one"),
    ("soft-limit-refusal-no-hand-on", "_state->_wake_pushers(_state->_room_freed(1));", "(void)0;",
     "a producer woken for a slot that refuses at the soft limit does not wake the next one"),
]

# program name -> sources, flags, then the runs: (label, args, environment)
PROGRAMS = [
    ("race", ["test/race.cc"], [], [("queue_race", [], {})]),
    ("liveness", ["test/liveness.cc"], [], [
        ("queue_liveness", [], {"LIVENESS_DEADLINE_MS": "1000", "LIVENESS_WATCHDOG_MS": "8000"}),
        ("queue_liveness_stress", ["stress", "150"], {"LIVENESS_DEADLINE_MS": "1000", "LIVENESS_WATCHDOG_MS": "8000"}),
    ]),
    ("test", ["test/test.cc"], [], [("queue", [], {})]),
    ("concurrent", ["test/concurrent.cc"], [], [("queue_concurrent", ["200000"], {})]),
    ("differential", ["test/differential.cc"], [], [("queue_differential", [], {})]),
]
TIMEOUT = 100


def run(cmd, env=None, timeout=None):
    """Returns (exit status, output); a timeout is status 124."""
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           env=dict(os.environ, **(env or {})), timeout=timeout)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        return 124, str(e.stdout or "")


def build(cxx, work, name, sources, flags):
    exe = os.path.join(work, "bin-" + name)
    cmd = [cxx, "-std=c++20", "-O1", "-I" + work, "-I" + os.path.join(work, "test"), "-o", exe] + flags
    cmd += [os.path.join(work, s) for s in sources] + ["-pthread"]
    status, out = run(cmd)
    return exe, status, out


def trial(cxx, work, everything):
    """Build and run the programs in `work`. Returns (labels of the runs that failed, build error)."""
    failed = []
    for name, sources, flags, runs in PROGRAMS:
        exe, status, out = build(cxx, work, name, sources, flags)
        if status != 0:
            return None, out
        for label, args, env in runs:
            status, out = run([exe] + args, env, TIMEOUT)
            if status != 0:
                failed.append(label)
        if failed and not everything:
            break
    return failed, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--filter", default="")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="queue-mutants-") as tmp:
        def fresh(name):
            work = os.path.join(tmp, name)
            shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".git", "build*", "*.o"))
            return work

        failed, err = trial(args.cxx, fresh("clean"), True)
        if failed is None or failed:
            print("the unmodified sources do not pass: " + ("build failed\n" + err if failed is None else " ".join(failed)))
            return 2
        print("clean: all tests pass", flush=True)

        def one(m):
            mid, old, new, what = m[:4]
            matches = m[4] if len(m) > 4 else 1
            work = fresh(mid)
            path = os.path.join(work, "queue.h")
            text = open(path).read()
            if text.count(old) != matches:
                return mid, "STALE", "%d matches of %r, expected %d" % (text.count(old), old, matches)
            open(path, "w").write(text.replace(old, new))
            failed, err = trial(args.cxx, work, args.all)
            shutil.rmtree(work)
            if failed is None:
                lines = [l for l in err.splitlines() if "error" in l]
                return mid, "BROKEN", lines[0] if lines else err[-200:]
            if failed:
                return mid, "killed", "by %s (%s)" % (", ".join(failed), what)
            return mid, "SURVIVES", what

        todo = [m for m in MUTANTS if args.filter in m[0]]
        bad = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for mid, status, detail in pool.map(one, todo):
                print("%-30s %-8s %s" % (mid, status, detail), flush=True)
                if status != "killed":
                    bad.append(mid)
        print("%d mutants, %d killed" % (len(todo), len(todo) - len(bad)))
        if bad:
            print("not killed: " + " ".join(bad))
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
