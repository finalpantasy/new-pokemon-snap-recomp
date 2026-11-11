// Minimal host runtime for the lifted-subset link check: every indirect or unresolved call logs and aborts.
// Written for the Snap prototype; the Cpu/GuestFn types and the rt_* prototypes come from cpu.h (TotK/LAS, MIT).
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"

// Called by the generated stubs_abort.c for a callee that is not part of the lifted subset (an import through the PLT,
// or a guest function the subset does not contain).
void rt_unresolved(Cpu *c, uint64_t addr, const char *what) {
    fprintf(stderr, "UNRESOLVED call to %#llx (%s), lr=%#llx\n", (unsigned long long)addr, what,
            (unsigned long long)c->x[30]);
    fflush(stderr);
    abort();
}

void rt_dispatch(Cpu *c) { rt_unresolved(c, c->pc, "indirect branch/call (rt_dispatch)"); }
void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    fprintf(stderr, "SVC %#x at %#llx\n", imm, (unsigned long long)pc);
    (void)c;
    abort();
}
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    fprintf(stderr, "BRK %#x at %#llx\n", imm, (unsigned long long)pc);
    (void)c;
    abort();
}
void rt_udf(Cpu *c, uint64_t pc) {
    fprintf(stderr, "UDF trap at %#llx\n", (unsigned long long)pc);
    (void)c;
    abort();
}
uint64_t rt_ticks(void) { return 0; }
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }
