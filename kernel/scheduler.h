/*
 * SAM OS — kernel/scheduler.h
 * ============================
 * Sprint 8: Three-way cooperative scheduler.
 *
 * DESIGN: Hardware-honest, one heavy workload at a time.
 *
 *   Three domains, three weights:
 *     AI      (0x10)  — inference priority, heaviest compute
 *     GAME    (0x20)  — 16ms deadline, real-time
 *     GENERAL (0x30)  — lightweight apps, background tasks
 *
 *   Cooperative round-robin: each domain gets one "quantum" per
 *   scheduling cycle. A quantum is one callable work unit. After
 *   running its work unit, a domain yields back to the scheduler.
 *   No preemption at this sprint — the PIT IRQ ticks but the
 *   scheduler is driven from the main loop, not from the IRQ.
 *
 *   The rule: domains yield voluntarily. If a domain is not
 *   runnable (no work queued), it is skipped silently.
 *
 * ISOLATION PROOF:
 *   - AI work operates only within AI_DOMAIN_BASE..+64MiB
 *   - GAME work operates only within GAME_DOMAIN_BASE..+64MiB
 *   - GENERAL work operates only within GENERAL_DOMAIN_BASE..+64MiB
 *   - The scheduler itself lives in kernel BSS — outside all domains
 *   - domain_alloc() ensures no base/size overlap at allocation time
 *
 * FUTURE (Sprint 9+):
 *   - Preemptive: IRQ0 handler sets a flag; main loop yields on flag
 *   - Priority inheritance: GAME can preempt GENERAL but not AI
 *   - Page table per domain: MMU-enforced isolation
 *
 * No libc. No external dependencies.
 */

#ifndef SAM_SCHEDULER_H
#define SAM_SCHEDULER_H

#include <stdint.h>

/* ── Domain IDs (must match main.c) ──────────────────────────────────────── */
#define SCHED_DOMAIN_AI      0x10
#define SCHED_DOMAIN_GAME    0x20
#define SCHED_DOMAIN_GENERAL 0x30

/* ── Work function type: a single schedulable unit of work ───────────────── */
typedef void (*sched_work_fn)(void *ctx);

/* ── Per-domain scheduler slot ───────────────────────────────────────────── */
typedef struct {
    uint8_t       id;           /* domain id (0x10/0x20/0x30) */
    uint8_t       runnable;     /* 1 if work is queued */
    sched_work_fn work;         /* work function for this quantum */
    void         *ctx;          /* opaque context passed to work fn */
    uint32_t      quanta_run;   /* total quanta executed for this domain */
    char          name[16];     /* domain name for serial output */
} sched_slot_t;

/* ── Scheduler state ─────────────────────────────────────────────────────── */
#define SCHED_MAX_DOMAINS 3

typedef struct {
    sched_slot_t slots[SCHED_MAX_DOMAINS];
    int          count;
    int          current;       /* index of last-run slot */
    uint32_t     cycles;        /* total scheduling cycles */
} sam_scheduler_t;

static sam_scheduler_t sam_sched;

/* ── sam_sched_init ─────────────────────────────────────────────────────── */
static inline void sam_sched_init(void)
{
    sam_sched.count   = 0;
    sam_sched.current = 0;
    sam_sched.cycles  = 0;

    /* Slot 0: AI domain */
    sam_sched.slots[0].id         = SCHED_DOMAIN_AI;
    sam_sched.slots[0].runnable   = 0;
    sam_sched.slots[0].work       = 0;
    sam_sched.slots[0].ctx        = 0;
    sam_sched.slots[0].quanta_run = 0;
    sam_sched.slots[0].name[0]    = 'A'; sam_sched.slots[0].name[1] = 'I';
    sam_sched.slots[0].name[2]    = '\0';

    /* Slot 1: GAME domain */
    sam_sched.slots[1].id         = SCHED_DOMAIN_GAME;
    sam_sched.slots[1].runnable   = 0;
    sam_sched.slots[1].work       = 0;
    sam_sched.slots[1].ctx        = 0;
    sam_sched.slots[1].quanta_run = 0;
    sam_sched.slots[1].name[0]    = 'G'; sam_sched.slots[1].name[1] = 'A';
    sam_sched.slots[1].name[2]    = 'M'; sam_sched.slots[1].name[3] = 'E';
    sam_sched.slots[1].name[4]    = '\0';

    /* Slot 2: GENERAL domain */
    sam_sched.slots[2].id         = SCHED_DOMAIN_GENERAL;
    sam_sched.slots[2].runnable   = 0;
    sam_sched.slots[2].work       = 0;
    sam_sched.slots[2].ctx        = 0;
    sam_sched.slots[2].quanta_run = 0;
    sam_sched.slots[2].name[0]    = 'G'; sam_sched.slots[2].name[1] = 'E';
    sam_sched.slots[2].name[2]    = 'N'; sam_sched.slots[2].name[3] = '\0';

    sam_sched.count = SCHED_MAX_DOMAINS;
}

/* ── sam_sched_submit — post one unit of work to a domain slot ───────────── */
/* domain_id: SCHED_DOMAIN_AI / _GAME / _GENERAL
 * work:      function to call for this quantum
 * ctx:       opaque pointer passed to work (may be NULL)
 * Returns 0 on success, -1 if domain_id not found.
 */
static inline int sam_sched_submit(uint8_t domain_id,
                                   sched_work_fn work, void *ctx)
{
    for (int i = 0; i < sam_sched.count; i++) {
        if (sam_sched.slots[i].id == domain_id) {
            sam_sched.slots[i].work     = work;
            sam_sched.slots[i].ctx      = ctx;
            sam_sched.slots[i].runnable = 1;
            return 0;
        }
    }
    return -1;   /* unknown domain */
}

/* ── sam_sched_tick — run one full round-robin cycle ─────────────────────── */
/* Visits slots in order: AI → GAME → GENERAL.
 * Each runnable slot gets exactly one quantum (one work() call).
 * After the call the slot is marked not-runnable until next submit.
 * Returns number of quanta executed this cycle (0..3).
 */
static inline int sam_sched_tick(void)
{
    int ran = 0;
    for (int i = 0; i < sam_sched.count; i++) {
        sched_slot_t *s = &sam_sched.slots[i];
        if (!s->runnable) continue;
        s->runnable = 0;
        if (s->work) {
            s->work(s->ctx);   /* run one quantum */
        }
        s->quanta_run++;
        ran++;
    }
    sam_sched.cycles++;
    return ran;
}

/* ── sam_sched_report — print scheduler state to serial ──────────────────── */
/* Caller must supply a serial_puts and serial_putdec function pointer,
 * or simply inline the output. We use extern declarations to avoid
 * pulling main.c's static functions into this header.
 *
 * To keep scheduler.h self-contained and free of dependencies, the
 * report function accepts a puts-style callback.
 */
typedef void (*sched_puts_fn)(const char *s);
typedef void (*sched_putdec_fn)(uint64_t v);

static inline void sam_sched_report(sched_puts_fn puts_fn,
                                    sched_putdec_fn putdec_fn)
{
    puts_fn("     Scheduler cycles : "); putdec_fn(sam_sched.cycles); puts_fn("\n");
    for (int i = 0; i < sam_sched.count; i++) {
        sched_slot_t *s = &sam_sched.slots[i];
        puts_fn("     [domain 0x");
        /* Print id as hex nibble */
        const char *hex = "0123456789ABCDEF";
        char id_hi = hex[(s->id >> 4) & 0xF];
        char id_lo = hex[s->id & 0xF];
        char id_str[3] = {id_hi, id_lo, '\0'};
        puts_fn(id_str);
        puts_fn("] ");
        puts_fn(s->name);
        puts_fn(" : ");
        putdec_fn(s->quanta_run);
        puts_fn(" quanta\n");
    }
}

#endif /* SAM_SCHEDULER_H */
