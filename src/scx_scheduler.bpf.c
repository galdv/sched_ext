// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal sched_ext scheduler - dispatches all tasks to shared DSQ
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

/* Our shared DSQ ID */
#define SHARED_DSQ 0

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
 * Dispatch to the shared DSQ
 */
SEC("struct_ops/enqueue")
void BPF_PROG(enqueue, struct task_struct *p, u64 enq_flags)
{
    scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
}

/*
 * dispatch - dispatch tasks to a CPU
 * Consume from shared DSQ
 */
SEC("struct_ops/dispatch")
void BPF_PROG(dispatch, s32 cpu, struct task_struct *prev)
{
    scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

/*
 * init - scheduler initialization
 * Create the shared dispatch queue
 * Marked sleepable (.s) because scx_bpf_create_dsq is sleepable
 */
SEC("struct_ops.s/init")
s32 BPF_PROG(init)
{
    return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

/*
 * sched_exit - scheduler exit/cleanup
 */
SEC("struct_ops/exit")
void BPF_PROG(sched_exit, struct scx_exit_info *ei)
{
    /* Prevent optimization by using a volatile operation */
    volatile int dummy = 0;
    (void)dummy;
}

/*
 * SCX_OPS_SWITCH_PARTIAL (8) = only opted-in tasks use this scheduler
 * Without it (flags=0), ALL tasks use this scheduler
 */
#define SCX_OPS_SWITCH_PARTIAL 8ULL

/* Define the scheduler ops structure */
SEC(".struct_ops")
struct sched_ext_ops scheduler_ops = {
    .select_cpu     = (void *)select_cpu,
    .enqueue        = (void *)enqueue,
    .dispatch       = (void *)dispatch,
    .init           = (void *)init,
    .exit           = (void *)sched_exit,
    .flags          = SCX_OPS_SWITCH_PARTIAL,  /* Only opted-in tasks */
    .name           = "scheduler",
};
