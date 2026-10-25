#include "../src/echo-plan.h"
#include <assert.h>
#include <stdlib.h>
int main(void) {
    EchoSpan spans[] = {{30, 40}, {10, 20}, {20, 25}, {12, 15}, {50, 60}};
    qsort(spans, 5, sizeof(*spans), echo_span_order);
    size_t count = echo_merge_spans(spans, 5);
    assert(count == 3);
    struct {
        int64_t begin, end, cut;
    } cases[] = {{0, 8, 8}, {0, 12, 10}, {15, 19, 15}, {25, 31, 30}, {40, 50, 50}, {60, 70, 70}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        assert(echo_cut_at(spans, count, cases[i].begin, cases[i].end) == cases[i].cut);
    EchoArrival arrivals[] = {{20, 1, 0}, {10, 2, 1}, {20, 0, 0}};
    qsort(arrivals, 3, sizeof(*arrivals), echo_arrival_order);
    assert(arrivals[0].id == 2 && arrivals[1].id == 0 && arrivals[2].id == 1);
}
