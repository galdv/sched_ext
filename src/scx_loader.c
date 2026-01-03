// SPDX-License-Identifier: GPL-2.0
/*
 * scx_loader - Loads BPF scheduler into kernel and runs dumper thread
 * The actual scheduling logic is in scx_scheduler.bpf.c
 *
 * Dumper thread: on every context switch, writes (seq, tgid, tid) to X.txt
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>
#include <pthread.h>
#include <sched.h>
#include <linux/limits.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define BPF_OBJ_NAME "scx_scheduler.bpf.o"
#define OUTPUT_FILE "X.txt"

#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

/* Must match struct in scx_scheduler.bpf.c */
struct dumper_state {
    __u32 last_tgid;
    __u32 last_tid;
    __u32 dumper_tid;
    __u64 seq;
    __u32 pending;
    __u64 violations;
    __u64 dumper_runs;
    __u64 dispatch_pending_empty;
};

static volatile int running = 1;
static int dumper_state_map_fd = -1;
static int target_cpu = -1;  /* CPU to pin dumper thread, -1 = no pinning */

static void sigint_handler(int sig)
{
    (void)sig;
    running = 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG)
        return 0;
    return vfprintf(stderr, format, args);
}

/* Find BPF object file relative to executable */
static const char *find_bpf_obj(char *argv0, char *buf, size_t buflen)
{
    char tmp[PATH_MAX];
    char *dir;

    strncpy(tmp, argv0, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    dir = dirname(tmp);
    snprintf(buf, buflen, "%s/%s", dir, BPF_OBJ_NAME);
    if (access(buf, R_OK) == 0)
        return buf;

    if (access(BPF_OBJ_NAME, R_OK) == 0)
        return BPF_OBJ_NAME;

    snprintf(buf, buflen, "build/%s", BPF_OBJ_NAME);
    if (access(buf, R_OK) == 0)
        return buf;

    return NULL;
}

/* Dumper thread function */
static void *dumper_thread(void *arg)
{
    (void)arg;
    __u32 key = 0;
    __u64 last_seq = 0;
    struct dumper_state state;
    FILE *output = NULL;
    struct sched_param param = { .sched_priority = 0 };

    /* Get our TID */
    pid_t my_tid = gettid();
    printf("Dumper thread started (TID=%d)\n", my_tid);

    /* Pin dumper to target CPU if specified */
    if (target_cpu >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(target_cpu, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
            fprintf(stderr, "WARNING: Failed to pin dumper to CPU %d: %s\n", target_cpu, strerror(errno));
        } else {
            printf("Dumper pinned to CPU %d\n", target_cpu);
        }
    }

    /* Opt-in to SCHED_EXT so our BPF scheduler controls us */
    if (sched_setscheduler(0, SCHED_EXT, &param) == -1) {
        fprintf(stderr, "WARNING: Failed to set SCHED_EXT: %s\n", strerror(errno));
    } else {
        printf("Dumper using SCHED_EXT\n");
    }

    /* Register our TID in the BPF map */
    if (bpf_map_lookup_elem(dumper_state_map_fd, &key, &state) == 0) {
        state.dumper_tid = my_tid;
        bpf_map_update_elem(dumper_state_map_fd, &key, &state, BPF_ANY);
        printf("Dumper TID registered in BPF map\n");
    } else {
        fprintf(stderr, "Failed to read BPF map: %s\n", strerror(errno));
        return NULL;
    }

    /* Open output file for writing */
    output = fopen(OUTPUT_FILE, "w");
    if (!output) {
        fprintf(stderr, "Failed to open %s: %s\n", OUTPUT_FILE, strerror(errno));
        return NULL;
    }

    printf("Dumper running, writing to %s\n", OUTPUT_FILE);

    while (running) {
        /* Read current state from BPF map */
        if (bpf_map_lookup_elem(dumper_state_map_fd, &key, &state) != 0) {
            fprintf(stderr, "Failed to read BPF map\n");
            break;
        }

        /* Check if seq changed (new context switch happened) */
        if (state.seq != last_seq && state.seq > 0) {
            __u32 tgid = state.last_tgid;
            __u32 tid = state.last_tid;

            /* Write which thread stopped running */
            fprintf(output, "%lu %u %u\n", (unsigned long)state.seq, tgid, tid);
            fflush(output);

            /* Update our last processed seq */
            last_seq = state.seq;

            /* Clear pending flag so other tasks can run */
            state.pending = 0;
            bpf_map_update_elem(dumper_state_map_fd, &key, &state, BPF_ANY);
        }

        /* Yield CPU if no work */
        sched_yield();
    }

    if (output)
        fclose(output);

    printf("Dumper thread exiting\n");
    return NULL;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -c <cpu>\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c <cpu>   CPU to pin dumper thread (required)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  sudo %s -c 1\n", prog);
    fprintf(stderr, "  Then run: ./scx_run taskset -c 1 ./process_tree\n");
}

int main(int argc, char **argv)
{
    struct bpf_object *obj;
    struct bpf_map *map, *state_map;
    struct bpf_link *link = NULL;
    char bpf_path[PATH_MAX];
    const char *bpf_obj;
    pthread_t dumper_tid;
    int err;
    int opt;

    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
        case 'c':
            target_cpu = atoi(optarg);
            if (target_cpu < 0) {
                fprintf(stderr, "Invalid CPU: %s\n", optarg);
                return 1;
            }
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (target_cpu < 0) {
        fprintf(stderr, "Error: -c <cpu> is required\n\n");
        usage(argv[0]);
        return 1;
    }

    libbpf_set_print(libbpf_print_fn);

    /* Find BPF object file */
    bpf_obj = find_bpf_obj(argv[0], bpf_path, sizeof(bpf_path));
    if (!bpf_obj) {
        fprintf(stderr, "Cannot find %s\n", BPF_OBJ_NAME);
        return 1;
    }

    /* Open BPF object */
    obj = bpf_object__open(bpf_obj);
    if (!obj) {
        fprintf(stderr, "Failed to open BPF object %s: %s\n", bpf_obj, strerror(errno));
        return 1;
    }

    /* Load BPF object */
    err = bpf_object__load(obj);
    if (err) {
        fprintf(stderr, "Failed to load BPF object: %s\n", strerror(-err));
        goto cleanup;
    }

    /* Find the dumper_state_map */
    state_map = bpf_object__find_map_by_name(obj, "dumper_state_map");
    if (!state_map) {
        fprintf(stderr, "Failed to find dumper_state_map\n");
        err = -1;
        goto cleanup;
    }
    dumper_state_map_fd = bpf_map__fd(state_map);

    /* Find and attach the struct_ops map */
    map = bpf_object__find_map_by_name(obj, "scheduler_ops");
    if (!map) {
        fprintf(stderr, "Failed to find scheduler_ops map\n");
        err = -1;
        goto cleanup;
    }

    link = bpf_map__attach_struct_ops(map);
    if (!link) {
        fprintf(stderr, "Failed to attach struct_ops: %s\n", strerror(errno));
        err = -1;
        goto cleanup;
    }

    /* Verify scheduler is actually enabled */
    usleep(100000);
    FILE *f = fopen("/sys/kernel/sched_ext/state", "r");
    if (f) {
        char state[32] = {0};
        if (fgets(state, sizeof(state), f)) {
            state[strcspn(state, "\n")] = 0;
            printf("sched_ext state: %s\n", state);
            if (strncmp(state, "enabled", 7) != 0) {
                fprintf(stderr, "WARNING: Scheduler attached but state is '%s'\n", state);
            }
        }
        fclose(f);
    }

    printf("==========================================\n");
    printf("  sched_ext scheduler loaded!\n");
    printf("  Dumper will run on CPU %d\n", target_cpu);
    printf("==========================================\n");

    /* Start dumper thread */
    if (pthread_create(&dumper_tid, NULL, dumper_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create dumper thread: %s\n", strerror(errno));
        err = -1;
        goto cleanup;
    }

    printf("Press Ctrl+C to unload...\n\n");

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    while (running) {
        sleep(1);
    }

    printf("\nUnloading scheduler...\n");

    /* Wait for dumper thread */
    pthread_join(dumper_tid, NULL);

    /* Print verification results */
    {
        __u32 key = 0;
        struct dumper_state final_state;
        if (bpf_map_lookup_elem(dumper_state_map_fd, &key, &final_state) == 0) {
            printf("\n");
            printf("========================================\n");
            printf("       VERIFICATION RESULTS\n");
            printf("========================================\n");
            printf("  Context switches (seq):    %lu\n", (unsigned long)final_state.seq);
            printf("  Dumper runs (pending=1):   %lu\n", (unsigned long)final_state.dumper_runs);
            printf("  DUMPER_DSQ empty:          %lu\n", (unsigned long)final_state.dispatch_pending_empty);
            printf("  Violations:                %lu\n", (unsigned long)final_state.violations);
            printf("----------------------------------------\n");
            if (final_state.violations == 0) {
                printf("  PASSED: No violations detected\n");
            } else {
                printf("  FAILED: %lu violations\n", (unsigned long)final_state.violations);
            }
            printf("========================================\n");
        }
    }

cleanup:
    if (link)
        bpf_link__destroy(link);
    bpf_object__close(obj);

    printf("Scheduler unloaded.\n");
    return err != 0;
}
