#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>

#include "common.h"

volatile int g_running = 1;
void handle_sigterm(int signo) {
    g_running = 0;
}

// 数据线程：高频，允许覆盖
void *data_thread(void *arg) {
    int id = *(int*)arg;
    int efd = 100 + id;
    char name[64];
    snprintf(name, sizeof(name), SHM_DATA_FMT, id + 1);
    int fd = shm_open(name, O_RDWR, 0666);
    struct shm_data_slot *slot = mmap(NULL, sizeof(*slot), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    uint32_t cnt = 0;
    while(g_running) {
        sprintf(slot->data, "Data Line %d: %u", id, cnt++);
        atomic_store(&slot->seq, cnt); // 仅做标志
        uint64_t one = 1;
        write(efd, &one, 8);
        usleep(100 * 1000); // 100ms
    }

    printf("Data thread %d exiting...\n", id);
    munmap(slot, sizeof(*slot));
    close(fd);
    return NULL;
}

// 消息线程：低频，重要消息不允许覆盖
void *msg_thread(void *arg) {
    int id = *(int*)arg;
    int efd = 200 + id;
    char name[64];
    snprintf(name, sizeof(name), SHM_MSG_FMT, id + 1);
    int fd = shm_open(name, O_RDWR, 0666);
    struct shm_msg_slot *slot = mmap(NULL, sizeof(*slot), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

    uint32_t cnt = 0;
    while(g_running) {
        // 关键同步点，只有当seq为0时才写入新消息，确保重要消息不被覆盖
        while (atomic_load(&slot->seq) != 0 && g_running) {
            usleep(50 * 1000); // 等待上一个消息被处理
        }
        if (!g_running) break;

        sprintf(slot->msg, "MSG Line %d: %u", id, cnt++);
        atomic_store(&slot->seq, 1); // 1表示有新消息
        uint64_t one = 1;
        write(efd, &one, 8);
        usleep(500 * 1000); // 500ms
    }

    printf("Msg thread %d exiting...\n", id);
    munmap(slot, sizeof(*slot));
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <line_count>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int line_id = atoi(argv[1]);
    if (line_id > MAX_LINES) {
        fprintf(stderr, "Line count must be between 0 and %d\n", MAX_LINES);
        return EXIT_FAILURE;
    }

    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigterm);

    pthread_t data_thread_t;
    pthread_t msg_thread_t;

    pthread_create(&data_thread_t, NULL, data_thread, &line_id);
    pthread_create(&msg_thread_t, NULL, msg_thread, &line_id);

    pthread_join(data_thread_t, NULL);
    pthread_join(msg_thread_t, NULL);

    printf("Sender exiting...\n");
    return EXIT_SUCCESS;
}