#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "common.h"

// Inizializzazione risorse nel processo Master
int init_shared_memory(const Config *conf, SharedData **shm_ptr_out);
int init_semaphores(const Config *conf);
int init_message_queues(int msg_queue_ids[NUM_STATIONS]);

int sem_wait_zero(int sem_id, int sem_num);

// Operazioni semaforiche atomiche (P e V)
int sem_p(int sem_id, int sem_num);
int sem_v(int sem_id, int sem_num);

#endif