#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <mach/mach_time.h>

uint64_t fib_blret(uint64_t n);
uint64_t fib_brbr(uint64_t n, void* sideStackTop);
uint64_t fib_ic(uint64_t n, void* sideStackTop);
uint64_t fib_mir(uint64_t n, void* sideStackTop);

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
    volatile uint64_t r2 = fib_ic(n, top);
    if (r2 != r0) { printf("IC MISMATCH\n"); return 1; }
    volatile uint64_t r3 = fib_mir(n, top);
    if (r3 != r0) { printf("MIR MISMATCH\n"); return 1; }
    if (r0 != r1) { printf("MISMATCH %llu vs %llu\n", r0, r1); return 1; }

    for (int rep = 0; rep < 3; rep++) {
        uint64_t t0 = mach_absolute_time();
        uint64_t a = fib_blret(n);
        uint64_t t1 = mach_absolute_time();
        uint64_t b = fib_brbr(n, top);
        uint64_t t2 = mach_absolute_time();
        uint64_t c = fib_ic(n, top);
        uint64_t t3 = mach_absolute_time();
        uint64_t dd = fib_mir(n, top);
        uint64_t t4 = mach_absolute_time();
        (void)dd;
        double nsA = (double)(t1 - t0) * tb.numer / tb.denom;
        double nsB = (double)(t2 - t1) * tb.numer / tb.denom;
        double nsC = (double)(t3 - t2) * tb.numer / tb.denom;
        uint64_t nc = calls_for(a);
        double nsD = (double)(t4 - t3) * tb.numer / tb.denom;
        printf("rep%d  bl/ret %.2f | br/br %.2f | +ic+rmw %.2f | +mirror %.2f (mirror adds %.2f ns/call)\n",
               rep, nsA/nc, nsB/nc, nsC/nc, nsD/nc, (nsD-nsC)/nc);
    }
    return 0;
}
