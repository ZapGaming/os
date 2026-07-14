#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <stdint.h>

/* Minimal test-and-set busy-wait spinlock -- the one new cross-cutting
 * concurrency primitive this SMP scheduling pass adds (the prior
 * bring-up-only pass had none: a repo-wide grep before that pass found
 * zero spinlock/mutex/atomic primitives anywhere in this kernel).
 *
 * Deliberately as simple as this gets: no ticket ordering, no fairness,
 * no recursion support. That's enough for this kernel's actual
 * concurrency shape right now -- at most two cores (kernel/apic.c only
 * ever wakes one AP), with light, short-lived contention.
 *
 * IRQs ARE disabled across the hold, on the core doing the acquiring --
 * see lock_acquire_irqsave()/lock_release_irqrestore() below, the only
 * pair kernel/scheduler.c actually uses. An earlier version of this
 * header assumed every acquirer would already be running inside an
 * interrupt gate (IF == 0 for the duration, by the CPU's own gate
 * semantics -- kernel/idt.c's 0x8E gates), which is true for
 * schedule() itself but NOT for task_create*()/task_set_cpu_affinity()
 * -- both called from ordinary kernel_main() flow with interrupts
 * already enabled. Without disabling IRQs there, this exact sequence
 * self-deadlocks a core: acquire the lock -> this core's own timer IRQ
 * fires mid-critical-section -> its handler calls schedule() -> tries
 * to acquire the SAME lock this same core already holds -> spins
 * forever, since the interrupted holder can never run again to release
 * it until the handler returns, which it never does. That's not a rare
 * boot-time-only risk either: task_create_user_isolated() (used by
 * every `run`/ELF launch, for the rest of this kernel's lifetime) goes
 * through the exact same path with interrupts already on. */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

/* Disables interrupts on this core, returning the EFLAGS this core had
 * beforehand (so a caller nested inside an already-IF==0 context, e.g.
 * schedule() running inside an interrupt gate, gets 0 back and
 * lock_release_irqrestore() correctly leaves interrupts off afterward
 * instead of prematurely re-enabling them before the gate's own iret).
 * Then busy-waits for the lock exactly like the old lock_acquire() did.
 * `xchg` is implicitly atomic on x86 (no explicit `lock` prefix needed,
 * unlike e.g. `add`/`cmpxchg`) -- the textbook test-and-set spinlock
 * built on it. The `pause` in the retry loop is the SDM's documented
 * hint for spin-wait bodies (reduces power/bus contention while
 * spinning; it is not needed for correctness). */
static inline uint32_t lock_acquire_irqsave(spinlock_t *lock) {
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(eflags));
    for (;;) {
        uint32_t was_locked = 1;
        __asm__ volatile ("xchg %0, %1"
                           : "+r"(was_locked), "+m"(lock->locked)
                           :
                           : "memory");
        if (was_locked == 0) {
            return eflags; /* lock->locked was 0 before this xchg -- we now own it */
        }
        while (lock->locked) {
            __asm__ volatile ("pause");
        }
    }
}

/* Plain store of 0, with a compiler barrier so no earlier memory access
 * inside the critical section can be reordered past the release by the
 * compiler. No `lock` prefix or fence instruction is needed on x86 for
 * the release side of a spinlock: the x86 memory model (TSO) never
 * reorders a store after an earlier store, so a plain `mov` here is
 * already ordered correctly with respect to the other core's next
 * `lock_acquire_irqsave()` spin-read of the same address. Then restores
 * interrupts to exactly whatever state `saved_eflags` (the value
 * lock_acquire_irqsave() returned) says they were in beforehand --
 * re-enabling them only if they were genuinely on before the acquire,
 * never unconditionally, since unconditionally `sti`ing inside a
 * still-active interrupt gate would be wrong. */
static inline void lock_release_irqrestore(spinlock_t *lock, uint32_t saved_eflags) {
    __asm__ volatile ("" ::: "memory");
    lock->locked = 0;
    if (saved_eflags & (1u << 9)) { /* EFLAGS.IF */
        __asm__ volatile ("sti");
    }
}

#endif
