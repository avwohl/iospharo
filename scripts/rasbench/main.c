#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <mach/mach_time.h>

uint64_t fib_blret(uint64_t n);
uint64_t fib_brbr(uint64_t n, void* sideStackTop);

// calls(n): number of activations of fib(n) = 2*fib(n)-1 with fib(0)=fib(1)=1
static uint64_t calls_for(uint64_t r) { return 2*r - 1; }

int main(int argc, char** argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], 0, 10) : 30;
    static uint8_t side[1 << 20] __attribute__((aligned(64)));
    void* top = side + sizeof(side);

    mach_timebase_info_data_t tb; mach_timebase_info(&tb);

    // warmup
    volatile uint64_t r0 = fib_blret(n);
    volatile uint64_t r1 = fib_brbr(n, top);
    if (r0 != r1) { printf("MISMATCH %llu vs %llu\n", r0, r1); return 1; }

    for (int rep = 0; rep < 3; rep++) {
        uint64_t t0 = mach_absolute_time();
        uint64_t a = fib_blret(n);
        uint64_t t1 = mach_absolute_time();
        uint64_t b = fib_brbr(n, top);
        uint64_t t2 = mach_absolute_time();
        double nsA = (double)(t1 - t0) * tb.numer / tb.denom;
        double nsB = (double)(t2 - t1) * tb.numer / tb.denom;
        uint64_t nc = calls_for(a);
        printf("rep%d  bl/ret: %7.2f ms (%.2f ns/call)   br/br: %7.2f ms (%.2f ns/call)   ratio %.2fx\n",
               rep, nsA/1e6, nsA/nc, nsB/1e6, nsB/nc, nsB/nsA);
    }
    return 0;
}
