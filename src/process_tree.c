#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <string.h>
#include <signal.h>

#define NUM_LAYERS 3
#define NUM_CHILDREN 2
#define NUM_THREADS 3

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
    pthread_mutex_t mutex;
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
    while (shared->running && keep_running) {
        pthread_mutex_lock(&shared->mutex);
        shared->active_process = process_id;
        shared->active_thread = thread_id;
        pthread_mutex_unlock(&shared->mutex);
        usleep(200000 + (rand() % 300000));
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

int main(void) {
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

    pthread_t display_thread;
    pthread_create(&display_thread, NULL, display_thread_func, NULL);

    create_process_tree(0, 0);

    shared->running = 0;
    pthread_join(display_thread, NULL);

    printf(CLEAR HOME);
    printf("Process tree terminated.\n");

    pthread_mutex_destroy(&shared->mutex);
    munmap(shared, sizeof(shared_data_t));

    return 0;
}
