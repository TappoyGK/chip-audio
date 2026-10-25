// Private, allocation-free interval queries used during echo setup.
#ifndef CHIP_ECHO_PLAN_H
#define CHIP_ECHO_PLAN_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    int64_t at, end;
} EchoSpan;
typedef struct {
    int64_t at;
    size_t id;
    int source;
} EchoArrival;
static int echo_span_order(const void *a, const void *b) {
    const EchoSpan *x = a, *y = b;
    return x->at < y->at ? -1 : x->at > y->at ? 1 : 0;
}
static int echo_arrival_order(const void *a, const void *b) {
    const EchoArrival *x = a, *y = b;
    if (x->at != y->at)
        return x->at < y->at ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}
// Input sorted by start; merge touching intervals for a monotonic end index.
static size_t echo_merge_spans(EchoSpan *spans, size_t count) {
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (used && spans[i].at <= spans[used - 1].end) {
            if (spans[i].end > spans[used - 1].end)
                spans[used - 1].end = spans[i].end;
        } else
            spans[used++] = spans[i];
    }
    return used;
}
static size_t echo_first_overlap(const EchoSpan *spans, size_t count, int64_t at) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (spans[mid].end <= at)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
// First occupied point at/after begin, clipped to the requested interval.
static int64_t echo_cut_at(const EchoSpan *spans, size_t count, int64_t begin, int64_t end) {
    size_t i = echo_first_overlap(spans, count, begin);
    if (i == count || spans[i].at >= end)
        return end;
    return spans[i].at > begin ? spans[i].at : begin;
}
#endif
