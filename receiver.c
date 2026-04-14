#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

#include "common.h"

volatile int g_running = 1;
void handle_sigterm(int signo) {
    g_running = 0;
}

void *receiver_thread(void *arg) {
    printf("Receiver thread started...\n");    
    
    int line_count = *(int*)arg;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // 分别存储两套 SHM的映射指针
    struct shm_data_slot *data_slots[MAX_LINES];
    struct shm_msg_slot *msg_slots[MAX_LINES];
    unsigned int last_data_seq[MAX_LINES] = {0};

    for (int i = 0; i < line_count; i++) {
        char name[64];

        // 映射data SHM
        snprintf(name, sizeof(name), SHM_DATA_FMT, i + 1);
        int data_fd = shm_open(name, O_RDWR, 0666);
        if (data_fd < 0) {
            perror("shm_open data");
            exit(EXIT_FAILURE);
        }
        data_slots[i] = mmap(NULL, sizeof(struct shm_data_slot), PROT_READ, MAP_SHARED, data_fd, 0); // 只读映射
        if (data_slots[i] == MAP_FAILED) {
            perror("mmap data");
            exit(EXIT_FAILURE);
        }
        close(data_fd); // 映射完成后关闭fd

        // 注册 Data Eventfd (100 + i)
        struct epoll_event ev_data = {.events = EPOLLIN, .data.u32 = 100 + i}; // 使用u32存储线路ID偏移100
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, 100 + i, &ev_data) < 0) {
            perror("epoll_ctl data");
            exit(EXIT_FAILURE);
        }

        // 映射msg SHM
        snprintf(name, sizeof(name), SHM_MSG_FMT, i + 1);
        int msg_fd = shm_open(name, O_RDWR, 0666);
        if (msg_fd < 0) {
            perror("shm_open msg");
            exit(EXIT_FAILURE);
        }
        msg_slots[i] = mmap(NULL, sizeof(struct shm_msg_slot), PROT_READ | PROT_WRITE, MAP_SHARED, msg_fd, 0); // 读写映射
        if (msg_slots[i] == MAP_FAILED) {
            perror("mmap msg");
            exit(EXIT_FAILURE);
        }
        close(msg_fd); // 映射完成后关闭fd

        // 注册 Msg Eventfd (200 + i)
        struct epoll_event ev_msg = {.events = EPOLLIN, .data.u32 = 200 + i}; // 使用u32存储线路ID偏移200
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, 200 + i, &ev_msg) < 0) {
            perror("epoll_ctl msg");
            exit(EXIT_FAILURE);
        }
    }

    printf("Single-threaded epoll monitoring %d lines...\n", line_count);

    struct epoll_event events[16];

    while (g_running) {
        uint64_t junk; // 用于读取Eventfd的值，实际不关心内容

        int n = epoll_wait(epfd, events, 16, 500); // 500ms超时
        if (n < 0) {
            perror("epoll_wait");
            break;
        } else if (n == 0) {
            // 超时，继续等待
            continue;
        }
        
        for (int i = 0; i < n; i++) {
            // 处理Data Eventfd
            uint32_t ev_id = events[i].data.u32;

            if (ev_id >= 100 && ev_id < 200) {
                int idx = ev_id - 100;

                // 读取Eventfd
                if (read(100 + idx, &junk, sizeof(junk)) < 0) {
                    perror("read data eventfd");
                    continue;
                }

                unsigned int seq = atomic_load(&data_slots[idx]->seq);
                if (seq != last_data_seq[idx]) {
                    printf("[DATA]ev_id %d Line %d (seq:%u): %s\n", ev_id, idx + 1, seq, data_slots[idx]->data);
                    last_data_seq[idx] = seq;
                }
            }
            else if (ev_id >= 200 && ev_id < 300) {
                // 处理Msg Eventfd
                int idx = ev_id - 200;

                // 读取Eventfd
                if (read(200 + idx, &junk, sizeof(junk)) < 0) {
                    perror("read msg eventfd");
                    continue;
                }

                // 判断seq是否更新，只有更新了才打印
                if (atomic_load(&msg_slots[idx]->seq) == 1) {
                    printf("[MSG!]ev_id %d Line %d: %s\n", ev_id, idx + 1, msg_slots[idx]->msg);

                    // 处理完报文后重置seq，允许下一条报文到来
                    atomic_store(&msg_slots[idx]->seq, 0);
                }
            }
        }
    }

    // --- 资源释放 ---
    for (int i = 0; i < line_count; i++) {
        // 解除 Data SHM的映射
        if (data_slots[i] != MAP_FAILED) {
            munmap(data_slots[i], sizeof(struct shm_data_slot));
        }

        if (msg_slots[i] != MAP_FAILED) {
            munmap(msg_slots[i], sizeof(struct shm_msg_slot));
        }
        
        // 关闭对应的Eventfd
        close(100 + i);
        close(200 + i);
    }

    printf("Receiver thread exiting...\n");
    close(epfd);

    return NULL;
}

int main(int argc, char **argv) {
    int line_count = (argc > 1) ? atoi(argv[1]) : 5;
    printf("Receiver started, monitoring %d lines.\n", line_count);

    signal(SIGINT, handle_sigterm);
    signal(SIGTERM, handle_sigterm);

    pthread_t receiver_thread_t;
    pthread_create(&receiver_thread_t, NULL, receiver_thread, &line_count);
    pthread_join(receiver_thread_t, NULL);

    printf("Receiver exiting...\n");
    return 0;
}