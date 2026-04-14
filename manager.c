#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>

#include "common.h"

// 全局状态
volatile sig_atomic_t g_running = 1;

void handle_sigterm(int signo) {
    g_running = 0;
}

void shm_create(int line_id) {
    char name[64];

    // 创建data SHM
    snprintf(name, sizeof(name), SHM_DATA_FMT, line_id);
    // 先尝试unlink，确保之前的资源被清理
    shm_unlink(name);
    // 创建新的SHM
    int data_fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (data_fd < 0) {
        perror("shm_open data");
        exit(EXIT_FAILURE);
    }
    ftruncate(data_fd, sizeof(struct shm_data_slot));
    // 使用data_fd结束后关闭
    close(data_fd);

    // 创建msg SHM
    snprintf(name, sizeof(name), SHM_MSG_FMT, line_id);
    // 先尝试unlink，确保之前的资源被清理
    shm_unlink(name);
    // 创建新的SHM
    int msg_fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (msg_fd < 0) {
        perror("shm_open msg");
        exit(EXIT_FAILURE);
    }
    ftruncate(msg_fd, sizeof(struct shm_msg_slot));
    // 使用msg_fd结束后关闭
    close(msg_fd);

    // 绑定 Eventfd
    int data_efd = eventfd(0, 0);
    if (data_efd < 0) {
        perror("eventfd data");
        exit(EXIT_FAILURE);
    }

    int msg_efd = eventfd(0, 0);
    if (msg_efd < 0) {
        perror("eventfd msg");
        exit(EXIT_FAILURE);
    }

    dup2(data_efd, 100 + line_id - 1); // 约定从100开始分配efds，line_id从1开始
    dup2(msg_efd, 200 + line_id - 1);  // 约定从200开始分配efds，line_id从1开始
}

void destroy_shm(int line_id) {
    char name[64];

    // 删除data SHM
    snprintf(name, sizeof(name), SHM_DATA_FMT, line_id);
    shm_unlink(name);

    // 删除msg SHM
    snprintf(name, sizeof(name), SHM_MSG_FMT, line_id);
    shm_unlink(name);
}

int main(int argc, char **argv) {
    // 动态获取线路数量，默认为5
    int line_count = (argc > 1) ? atoi(argv[1]) : 5;
    printf("Manager started with %d lines.\n", line_count);

    // 注册信号
    struct sigaction sa = {.sa_handler = handle_sigterm, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 创建用于pid的资源
    pid_t *pids = calloc(line_count + 1, sizeof(pid_t)); // +1 for receiver, line_count for senders
    if (!pids) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    // 创建efds
    int *efds = malloc(line_count * sizeof(int));
    if (!efds) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    // proc_count 进程计数器
    int proc_count = 0;

    // 创建SHM和efds
    for (int i = 0; i < line_count; i++) {
        shm_create(i + 1);
    }

    pid_t pid_receiver = fork();
    if (pid_receiver < 0) {
        perror("fork receiver");
        exit(EXIT_FAILURE);
    } else if (pid_receiver == 0) {
        char line_cnt[16];
        snprintf(line_cnt, sizeof(line_cnt), "%d", line_count);
        execl("./receiver", "receiver", line_cnt, NULL);
    }
    else {
        pids[proc_count++] = pid_receiver;
    }

    for (int i = 0; i < line_count; i++) {
        pid_t pid_sender = fork();
        if (pid_sender < 0) {
            perror("fork sender");
            exit(EXIT_FAILURE);
        } else if (pid_sender == 0) {
            char line_id[16];
            snprintf(line_id, sizeof(line_id), "%d", i);
            execl("./sender", "sender", line_id, NULL);
        }
        else {
            pids[proc_count++] = pid_sender;
        }
    }

    // 重要，关闭fd副本
    for (int i = 0; i < line_count; i++) {
        close(100 + i);
        close(200 + i);
    }

    while (g_running) {
        pause(); // 等待信号
        int status;
        pid_t wpid;
        while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < proc_count; i++) {
                if (pids[i] == wpid) {
                    printf("[manager] 子进程 %d 退出，状态 %d\n", wpid, status);
                    g_running = 0;
                }
            }
        }
    }

    printf("[manager] cleaning up resources...\n");

    // 终止所有子进程
    for (int i = 0; i < proc_count; i++) {
        if (pids[i] > 0) kill(pids[i], SIGTERM);
    }

    // 等待子进程退出，设置超时机制
    const int timeout_sec = 3;
    time_t start = time(NULL);

    while (1) {
        int alive = 0;

        for (int i = 0; i < proc_count; i++) {
            if (pids[i] <= 0)
                continue;

            pid_t ret = waitpid(pids[i], NULL, WNOHANG);
            if (ret == 0) {
                alive++;
            } else {
                pids[i] = -1; // 标记已回收
            }
        }

        if (alive == 0)
            break;

        if (time(NULL) - start >= timeout_sec) {
            printf("[manager] 超时，强制终止剩余子进程\n");
            break;
        }

        usleep(100 * 1000); // 100ms
    }

    // 强制杀死仍然存活的子进程
    for (int i = 0; i < proc_count; i++) {
        if (pids[i] > 0) {
            kill(pids[i], SIGKILL);
            waitpid(pids[i], NULL, 0);
        }
    }

    // 清理SHM和efds
    for (int i = 0; i < line_count; i++) {
        destroy_shm(i + 1);
        if (efds[i] >= 0) close(efds[i]);
    }

    free(pids);
    free(efds);

    printf("[manager] All done. Exit.\n");
    return 0;
}