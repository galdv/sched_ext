// SPDX-License-Identifier: GPL-2.0
/*
 * scx_loader - Loads BPF scheduler into kernel
 * The actual scheduling logic is in scx_scheduler.bpf.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <libgen.h>
#include <linux/limits.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define BPF_OBJ_NAME "scx_scheduler.bpf.o"

static volatile int running = 1;

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

    /* Try same directory as executable */
    strncpy(tmp, argv0, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    dir = dirname(tmp);
    snprintf(buf, buflen, "%s/%s", dir, BPF_OBJ_NAME);
    if (access(buf, R_OK) == 0)
        return buf;

    /* Try current directory */
    if (access(BPF_OBJ_NAME, R_OK) == 0)
        return BPF_OBJ_NAME;

    /* Try build/ subdirectory */
    snprintf(buf, buflen, "build/%s", BPF_OBJ_NAME);
    if (access(buf, R_OK) == 0)
        return buf;

    return NULL;
}

int main(int argc, char **argv)
{
    struct bpf_object *obj;
    struct bpf_map *map;
    struct bpf_link *link = NULL;
    char bpf_path[PATH_MAX];
    const char *bpf_obj;
    int err;

    (void)argc;

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
    usleep(100000);  /* Give kernel time to enable */
    FILE *f = fopen("/sys/kernel/sched_ext/state", "r");
    if (f) {
        char state[32] = {0};
        if (fgets(state, sizeof(state), f)) {
            state[strcspn(state, "\n")] = 0;  /* Remove newline */
            printf("sched_ext state: %s\n", state);
            if (strncmp(state, "enabled", 7) != 0) {
                fprintf(stderr, "WARNING: Scheduler attached but state is '%s'\n", state);
            }
        }
        fclose(f);
    }

    printf("==========================================\n");
    printf("  sched_ext scheduler loaded!\n");
    printf("==========================================\n");
    printf("Press Ctrl+C to unload...\n\n");

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    while (running) {
        sleep(1);
    }

    printf("\nUnloading scheduler...\n");

cleanup:
    if (link)
        bpf_link__destroy(link);
    bpf_object__close(obj);

    printf("Scheduler unloaded.\n");
    return err != 0;
}
