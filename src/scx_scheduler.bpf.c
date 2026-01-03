// SPDX-License-Identifier: GPL-2.0
/*
 * sched_ext scheduler with dumper thread synchronization
 *
 * On every context switch:
 * 1. Save the switched-out task's info
 * 2. Set pending=1 so only dumper can run
 * 3. Dumper reads maps, sets pending=0
 * 4. Other tasks can run
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

/* Define SCX constants */
#define SCX_SLICE_DFL       (20ULL * 1000000)  /* 20ms default slice */

/* Enqueue flags */
#define SCX_ENQ_WAKEUP      (1LLU << 0)
#define SCX_ENQ_HEAD        (1LLU << 1)

/* DSQ IDs - separate queues for dumper and other tasks */
#define SHARED_DSQ 0    /* For regular tasks */
#define DUMPER_DSQ 1    /* For dumper thread only */

/*
 * Shared state between BPF and userspace dumper
 */
struct dumper_state {
    __u32 last_tgid;    /* Process ID of last switched-out task */
    __u32 last_tid;     /* Thread ID of last switched-out task */
    __u32 dumper_tid;   /* TID of dumper thread (set by userspace) */
    __u64 seq;          /* Context switch sequence number */
    __u32 pending;      /* 1 = dumper must run, 0 = others can run */
    __u64 violations;   /* TEST: count times non-dumper ran while pending=1 */
    __u64 dumper_runs;  /* TEST: count times dumper ran when pending=1 */
    __u64 dispatch_pending_empty; /* DEBUG: dispatch called with pending=1 but DUMPER_DSQ empty */
};

/* BPF map to share state with userspace */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct dumper_state);
} dumper_state_map SEC(".maps");

/* kfunc declarations */
extern void scx_bpf_dsq_insert(struct task_struct *p, u64 dsq_id, u64 slice, u64 enq_flags) __ksym;
extern bool scx_bpf_dsq_move_to_local(u64 dsq_id) __ksym;
extern s32 scx_bpf_create_dsq(u64 dsq_id, s32 node) __ksym;
extern s32 scx_bpf_task_cpu(const struct task_struct *p) __ksym;
extern void scx_bpf_kick_cpu(s32 cpu, u64 flags) __ksym;

/*
 * select_cpu - select a CPU for a waking task
 * Just return the previously used CPU
 */
SEC("struct_ops/select_cpu")
s32 BPF_PROG(select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
    return prev_cpu;
}

/*
 * enqueue - enqueue a task to be scheduled
 * Dumper goes to DUMPER_DSQ, others go to SHARED_DSQ
 */
SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags)
{
    __u32 key = 0;
    struct dumper_state *state;
    __u32 tid = p->pid;  /* In kernel, pid is actually TID */

    state = bpf_map_lookup_elem(&dumper_state_map, &key);
    if (!state) {
        scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
        return;
    }

    if (state->dumper_tid != 0 && tid == state->dumper_tid) {
        /* Dumper goes to its own DSQ */
        scx_bpf_dsq_insert(p, DUMPER_DSQ, SCX_SLICE_DFL, enq_flags);
    } else {
        /* Regular tasks go to shared DSQ */
        scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
    }
}

/*
 * running - called when a task starts running on CPU
 * Used to detect violations: non-dumper running while pending=1
 */
SEC("struct_ops/running")
void BPF_PROG(running, struct task_struct *p)
{
    __u32 key = 0;
    struct dumper_state *state;
    __u32 tid = p->pid;

    state = bpf_map_lookup_elem(&dumper_state_map, &key);
    if (!state)
        return;

    /* Only check after dumper has registered */
    if (state->dumper_tid == 0)
        return;

    if (state->pending == 1) {
        if (tid == state->dumper_tid) {
            /* Good: dumper is running while pending=1 */
            state->dumper_runs++;
        } else {
            /* BAD: non-dumper running while pending=1 - VIOLATION! */
            state->violations++;
        }
    }
}

/*
 * stopping - called when a task is being switched out
 * Update state for dumper synchronization
 */
SEC("struct_ops/stopping")
void BPF_PROG(stopping, struct task_struct *p, bool runnable)
{
    __u32 key = 0;
    struct dumper_state *state;
    __u32 tid = p->pid;   /* In kernel, pid is actually TID */
    __u32 tgid = p->tgid; /* Process ID */
    s32 cpu;

    state = bpf_map_lookup_elem(&dumper_state_map, &key);
    if (!state)
        return;

    /* Skip if no dumper registered yet */
    if (state->dumper_tid == 0)
        return;

    /* Skip if this is the dumper thread - don't track dumper's own switches */
    if (tid == state->dumper_tid)
        return;

    /* Update state: save task info, increment seq, set pending */
    state->last_tgid = tgid;
    state->last_tid = tid;
    state->seq++;
    state->pending = 1;

    /* Kick CPU to wake dumper */
    cpu = scx_bpf_task_cpu(p);
    scx_bpf_kick_cpu(cpu, 0);
}

/*
 * dispatch - dispatch tasks to a CPU
 * If pending=1, only dumper can run. Otherwise, dispatch normally.
 */
SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct *prev)
{
    __u32 key = 0;
    struct dumper_state *state;

    state = bpf_map_lookup_elem(&dumper_state_map, &key);
    if (!state) {
        /* Fallback if map lookup fails */
        scx_bpf_dsq_move_to_local(SHARED_DSQ);
        return;
    }

    if (state->pending == 1) {
        /* Only dumper can run - consume only from DUMPER_DSQ */
        if (!scx_bpf_dsq_move_to_local(DUMPER_DSQ)) {
            /* DUMPER_DSQ is empty while pending=1 - this causes violations */
            state->dispatch_pending_empty++;
        }
    } else {
        /* Normal operation - try dumper first, then shared */
        if (!scx_bpf_dsq_move_to_local(DUMPER_DSQ))
            scx_bpf_dsq_move_to_local(SHARED_DSQ);
    }
}

/*
 * init - scheduler initialization
 * Create both dispatch queues
 */
SEC("struct_ops.s/init")
s32 BPF_PROG(init)
{
    s32 err;

    /* Create shared DSQ for regular tasks */
    err = scx_bpf_create_dsq(SHARED_DSQ, -1);
    if (err)
        return err;

    /* Create dumper DSQ for dumper thread */
    err = scx_bpf_create_dsq(DUMPER_DSQ, -1);
    if (err)
        return err;

    return 0;
}

/*
 * sched_exit - scheduler exit/cleanup
 */
SEC("struct_ops/exit")
void BPF_PROG(sched_exit, struct scx_exit_info *ei)
{
    volatile int dummy = 0;
    (void)dummy;
}

/*
 * SCX_OPS_SWITCH_PARTIAL (8) = only opted-in tasks use this scheduler
 */
#define SCX_OPS_SWITCH_PARTIAL 8ULL

/* Define the scheduler ops structure */
SEC(".struct_ops")
struct sched_ext_ops scheduler_ops = {
    .select_cpu     = (void *)select_cpu,
    .enqueue        = (void *)enqueue,
    .dispatch       = (void *)dispatch,
    .running        = (void *)running,
    .stopping       = (void *)stopping,
    .init           = (void *)init,
    .exit           = (void *)sched_exit,
    .flags          = SCX_OPS_SWITCH_PARTIAL,
    .name           = "scheduler",
};
