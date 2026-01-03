#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <getopt.h>

#define Y_FILE "Y.txt"
#define MAX_RECORDS 100000

static int enable_display = 0;  /* Disabled by default */

#define NUM_LAYERS 3
#define NUM_CHILDREN 2
#define NUM_THREADS 3

/* Record entry for shared memory buffer */
typedef struct {
    pid_t pid;
    pid_t tid;
} record_t;

#define CLEAR       "\033[2J"
#define HOME        "\033[H"
#define RED         "\033[1;31m"
#define GREEN       "\033[1;32m"
#define YELLOW      "\033[1;33m"
#define CYAN        "\033[1;36m"
#define RESET       "\033[0m"
#define BOLD        "\033[1m"

typedef struct {
    int process_id;
    int thread_id;
} thread_arg_t;

typedef struct {
    int active_process;
    int active_thread;
    int running;
    pthread_mutex_t mutex;         /* Single lock for all operations */
    unsigned long record_idx;      /* Next record index */
    record_t records[MAX_RECORDS]; /* Buffer of records */
} shared_data_t;

shared_data_t *shared;
volatile sig_atomic_t keep_running = 1;

void handle_signal(int sig) {
    (void)sig;
    keep_running = 0;
    if (shared) shared->running = 0;
}

void pt(int p, int t, int ap, int at) {
    if (p == ap && t == at)
        printf(RED "T%d" RESET, t);
    else
        printf("T%d", t);
}

void end_line(void) {
    printf("\033[85G" CYAN "║\n" RESET);
}

void draw_tree(int ap, int at) {
    printf(CLEAR HOME);

    printf(CYAN "╔═══════════════════════════════════════════════════════════════════════════════════╗\n" RESET);
    printf(CYAN "║" RESET "                            " BOLD "PROCESS TREE" RESET);
    end_line();
    printf(CYAN "╠═══════════════════════════════════════════════════════════════════════════════════╣\n" RESET);
    printf(CYAN "║" RESET);
    end_line();

    // Layer 0 - P:0
    printf(CYAN "║" RESET "  " YELLOW "Layer 0:" RESET "                              " BOLD "[P:0]" RESET);
    end_line();
    printf(CYAN "║" RESET "                                        ");
    pt(0,0,ap,at); printf(" "); pt(0,1,ap,at); printf(" "); pt(0,2,ap,at);
    end_line();
    printf(CYAN "║" RESET "                                         |");
    end_line();
    printf(CYAN "║" RESET "                        ┌────────────────┴────────────────┐");
    end_line();
    printf(CYAN "║" RESET "                        |                                 |");
    end_line();

    // Layer 1 - P:1 and P:2
    printf(CYAN "║" RESET "  " YELLOW "Layer 1:" RESET "              " BOLD "[P:1]" RESET "                           " BOLD "[P:2]" RESET);
    end_line();
    printf(CYAN "║" RESET "                      ");
    pt(1,0,ap,at); printf(" "); pt(1,1,ap,at); printf(" "); pt(1,2,ap,at);
    printf("                         ");
    pt(2,0,ap,at); printf(" "); pt(2,1,ap,at); printf(" "); pt(2,2,ap,at);
    end_line();
    printf(CYAN "║" RESET "                         |                                 |");
    end_line();
    printf(CYAN "║" RESET "              ┌──────────┴──────────┐           ┌──────────┴──────────┐");
    end_line();
    printf(CYAN "║" RESET "              |                     |           |                     |");
    end_line();

    // Layer 2 - P:3, P:4, P:5, P:6
    printf(CYAN "║" RESET "  " YELLOW "Layer 2:" RESET "    " BOLD "[P:3]" RESET "               " BOLD "[P:4]" RESET "       " BOLD "[P:5]" RESET "               " BOLD "[P:6]" RESET);
    end_line();
    printf(CYAN "║" RESET "            ");
    pt(3,0,ap,at); printf(" "); pt(3,1,ap,at); printf(" "); pt(3,2,ap,at);
    printf("             ");
    pt(4,0,ap,at); printf(" "); pt(4,1,ap,at); printf(" "); pt(4,2,ap,at);
    printf("     ");
    pt(5,0,ap,at); printf(" "); pt(5,1,ap,at); printf(" "); pt(5,2,ap,at);
    printf("             ");
    pt(6,0,ap,at); printf(" "); pt(6,1,ap,at); printf(" "); pt(6,2,ap,at);
    end_line();

    printf(CYAN "║" RESET);
    end_line();
    printf(CYAN "╠═══════════════════════════════════════════════════════════════════════════════════╣\n" RESET);
    printf(CYAN "║" RESET "  " GREEN "Active:" RESET " " RED "P:%d T:%d" RESET "  │  7 processes × 3 threads = 21 threads", ap, at);
    end_line();
    printf(CYAN "║" RESET "  Press Ctrl+C to stop");
    end_line();
    printf(CYAN "╚═══════════════════════════════════════════════════════════════════════════════════╝\n" RESET);

    fflush(stdout);
}

void *display_thread_func(void *arg) {
    (void)arg;
    while (shared->running && keep_running) {
        pthread_mutex_lock(&shared->mutex);
        int proc = shared->active_process;
        int thread = shared->active_thread;
        pthread_mutex_unlock(&shared->mutex);
        draw_tree(proc, thread);
        usleep(100000);
    }
    return NULL;
}

void worker_loop(int process_id, int thread_id) {
    pid_t my_pid = getpid();
    pid_t my_tid = syscall(SYS_gettid);

    while (shared->running && keep_running) {
        /* Single critical section: update display + record */
        pthread_mutex_lock(&shared->mutex);

        shared->active_process = process_id;
        shared->active_thread = thread_id;

        /* Record to shared memory buffer while holding lock */
        unsigned long idx = shared->record_idx;
        if (idx < MAX_RECORDS) {
            shared->records[idx].pid = my_pid;
            shared->records[idx].tid = my_tid;
            shared->record_idx++;
        }

        pthread_mutex_unlock(&shared->mutex);

        /* Sleep to allow natural context switch - not spinning */
        usleep(10000);  /* 10ms - gives time for context switch */
    }
}

void *worker_thread_func(void *arg) {
    thread_arg_t *targ = (thread_arg_t *)arg;
    worker_loop(targ->process_id, targ->thread_id);
    return NULL;
}

void run_threads(int process_id) {
    pthread_t threads[NUM_THREADS - 1];
    thread_arg_t args[NUM_THREADS - 1];
    srand(getpid());

    /* Create T1 and T2 as separate threads */
    for (int i = 0; i < NUM_THREADS - 1; i++) {
        args[i].process_id = process_id;
        args[i].thread_id = i + 1;
        pthread_create(&threads[i], NULL, worker_thread_func, &args[i]);
    }

    /* Main process thread runs as T0 */
    worker_loop(process_id, 0);

    for (int i = 0; i < NUM_THREADS - 1; i++) {
        pthread_join(threads[i], NULL);
    }
}

void create_process_tree(int current_layer, int process_id) {
    if (current_layer >= NUM_LAYERS - 1) {
        run_threads(process_id);
        return;
    }
    int child_ids[NUM_CHILDREN] = {2 * process_id + 1, 2 * process_id + 2};

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork failed");
            exit(1);
        } else if (pid == 0) {
            create_process_tree(current_layer + 1, child_ids[i]);
            exit(0);
        }
    }
    run_threads(process_id);
    for (int i = 0; i < NUM_CHILDREN; i++) {
        wait(NULL);
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--display]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --display    Enable visual tree display\n");
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"display", no_argument, NULL, 'd'},
        {"help",    no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            enable_display = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    shared = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shared->mutex, &attr);
    shared->active_process = 0;
    shared->active_thread = 0;
    shared->running = 1;
    shared->record_idx = 0;

    pthread_t display_thread;
    if (enable_display) {
        pthread_create(&display_thread, NULL, display_thread_func, NULL);
    }

    printf("Process tree running (display=%s). Press Ctrl+C to stop.\n",
           enable_display ? "on" : "off");

    create_process_tree(0, 0);

    shared->running = 0;
    if (enable_display) {
        pthread_join(display_thread, NULL);
        printf(CLEAR HOME);
    }
    printf("Process tree terminated.\n");

    /* Dump records to Y.txt */
    unsigned long num_records = shared->record_idx;
    if (num_records > MAX_RECORDS)
        num_records = MAX_RECORDS;

    printf("Dumping %lu records to %s...\n", num_records, Y_FILE);
    FILE *yf = fopen(Y_FILE, "w");
    if (yf) {
        for (unsigned long i = 0; i < num_records; i++) {
            fprintf(yf, "%lu %d %d\n", i + 1,
                    shared->records[i].pid,
                    shared->records[i].tid);
        }
        fclose(yf);
        printf("Done.\n");
    } else {
        perror("Failed to open Y.txt");
    }

    /* Cleanup */
    pthread_mutex_destroy(&shared->mutex);
    munmap(shared, sizeof(shared_data_t));

    return 0;
}
