/*
 * scx_run - Launch a program with SCHED_EXT scheduling policy
 * Usage: scx_run <program> [args...]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <string.h>

#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

/* Check if sched_ext scheduler is enabled */
static int is_scx_enabled(void) {
    FILE *f = fopen("/sys/kernel/sched_ext/state", "r");
    if (!f)
        return 0;

    char state[32] = {0};
    if (fgets(state, sizeof(state), f)) {
        fclose(f);
        return strncmp(state, "enabled", 7) == 0;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    struct sched_param param = { .sched_priority = 0 };

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program> [args...]\n", argv[0]);
        fprintf(stderr, "Launch a program with SCHED_EXT scheduling policy\n");
        return 1;
    }

    /* Check if scheduler is enabled */
    if (!is_scx_enabled()) {
        fprintf(stderr, "ERROR: No sched_ext scheduler is enabled!\n");
        fprintf(stderr, "Run 'sudo ./scx_minimal' first to load the scheduler.\n");
        return 1;
    }

    /* Set SCHED_EXT for this process - child will inherit */
    if (sched_setscheduler(0, SCHED_EXT, &param) == -1) {
        fprintf(stderr, "Failed to set SCHED_EXT: %s\n", strerror(errno));
        return 1;
    }

    /* Exec the target program */
    execvp(argv[1], &argv[1]);

    /* If exec fails */
    fprintf(stderr, "Failed to exec %s: %s\n", argv[1], strerror(errno));
    return 1;
}
