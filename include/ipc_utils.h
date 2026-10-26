#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include "common.h"

int init_shared_memory(const Config *conf, SharedData **shm_ptr_out);
int init_semaphores(const Config *conf);
int init_message_queues(int msg_queue_ids[NUM_STATIONS]);

void setup_term_handler(void);
void attendi_nanosecondi(long nanosecs);
void simula_servizio(int tempo_medio_sec, int variazione_percentuale, long n_nano_secs);

int sem_p(int sem_id, int sem_num);
int sem_v(int sem_id, int sem_num);
int sem_add(int sem_id, int sem_num, int val);
int sem_wait_zero(int sem_id, int sem_num);

#endif