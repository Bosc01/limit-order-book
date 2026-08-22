#pragma once
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include "alloc_count.hpp"
#include "clock.hpp"
#include "workload.hpp"

namespace bench {

// Compiler barrier: forces `v` to exist in a register/memory at this point,
// so the optimizer cannot delete the book operations as dead code. Costs
// zero instructions — it only constrains scheduling.
template <class T>
inline void do_not_optimize(T v) {
    asm volatile("" : "+r"(v) : : "memory");
}

// The harness is a template, not an interface with virtual calls: virtual
// dispatch would add an indirect call to every measured sample and, worse,
// act as an optimization barrier around the very code we are measuring.
// Each engine gets compiled into its own fully-inlined copy of this loop.
template <class Book>
inline lob::Qty dispatch(Book& book, const Op& op) {
    // Each arm folds the result into a value the barrier consumes, so the
    // optimizer can never discard the book operation as dead code.
    switch (op.kind) {
        case OpKind::Limit:
            return book.submit_limit(op.id, op.side, op.price, op.qty, op.tif).filled;
        case OpKind::Market:
            return book.submit_market(op.id, op.side, op.qty);
        case OpKind::Cancel:
            return book.cancel(op.id) ? 1 : 0;
        case OpKind::Modify: {
            const auto m = book.modify(op.id, op.price, op.qty);
            return m.filled + (m.ok ? 1 : 0);
        }
    }
    return 0;
}

struct RunResult {
    std::vector<std::uint64_t> latency_ticks; // per measured op, in raw ticks
    double                     wall_seconds = 0;
};

// Warmup ops run against the same book but are not recorded: they populate
// the book to a realistic depth and warm caches, branch predictors, and the
// page tables backing our sample buffer.
template <class Book>
RunResult run_ops(Book& book, std::span<const Op> ops, std::size_t warmup) {
    RunResult r;
    // resize (not reserve): touching every element now pre-faults the pages,
    // so the first writes during measurement don't eat a page-fault each.
    r.latency_ticks.resize(ops.size() - warmup, 0);

    for (std::size_t i = 0; i < warmup; ++i)
        do_not_optimize(dispatch(book, ops[i]));

    // Count allocations over the MEASURED region only: the sample buffer
    // above and the warmup's book-building must not pollute the "zero
    // allocations on the hot path" evidence.
    alloc_count_reset();

    const auto w0 = std::chrono::steady_clock::now();
    for (std::size_t i = warmup; i < ops.size(); ++i) {
        const std::uint64_t t0 = rdticks();
        const lob::Qty      res = dispatch(book, ops[i]);
        const std::uint64_t t1 = rdticks();
        do_not_optimize(res);
        r.latency_ticks[i - warmup] = t1 - t0;
    }
    const auto w1 = std::chrono::steady_clock::now();

    r.wall_seconds = std::chrono::duration<double>(w1 - w0).count();
    return r;
}

} // namespace bench
