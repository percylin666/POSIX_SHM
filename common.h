#ifndef __COMMON_H
#define __COMMON_H

#include <stdatomic.h>
#include <stdint.h>

#define MAX_LINES       64

#define SHM_DATA_SIZE   1024
#define SHM_MSG_SIZE    1024

#define SHM_DATA_FMT   "/shm_data_line_%d"
#define SHM_MSG_FMT    "/shm_msg_line_%d"

struct shm_data_slot {
    atomic_uint seq;
    char data[SHM_DATA_SIZE];
};

struct shm_msg_slot {
    atomic_uint seq;
    char msg[SHM_MSG_SIZE];
};

#endif /* __COMMON_H */