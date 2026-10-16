#include "ipc_utils.h"

int init_shared_memory(const Config *conf, SharedData **shm_ptr_out) {
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if (shm_key == -1) {
        perror("Errore ftok per SHM");
        return -1;
    }

    int shm_id = shmget(shm_key, sizeof(SharedData), IPC_CREAT | IPC_EXCL | 0600);
    if (shm_id == -1) {
        perror("Errore shmget");
        return -1;
    }

    SharedData *ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (ptr == (void *)-1) {
        perror("Errore shmat");
        shmctl(shm_id, IPC_RMID, NULL);
        return -1;
    }

    // Inizializzazione dati
    memset(ptr, 0, sizeof(SharedData));
    ptr->simulazione_attiva = 1;
    ptr->giorno_corrente = 1;
    ptr->minuto_corrente = 0;
    ptr->porzioni_primi[0] = conf->avg_refill_primi;
    ptr->porzioni_primi[1] = conf->avg_refill_primi;
    ptr->porzioni_secondi[0] = conf->avg_refill_secondi;
    ptr->porzioni_secondi[1] = conf->avg_refill_secondi;

    *shm_ptr_out = ptr;
    return shm_id;
}

int init_semaphores(const Config *conf) {
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    if (sem_key == -1) {
        perror("Errore ftok per semafori");
        return -1;
    }

    int sem_id = semget(sem_key, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);
    if (sem_id == -1) {
        perror("Errore semget");
        return -1;
    }

    union semun arg;

    // 1. Mutex per la memoria condivisa (valore = 1)
    arg.val = 1;
    if (semctl(sem_id, SEM_MUTEX_SHM, SETVAL, arg) == -1) return -1;

    // 2. Barriera di sincronizzazione iniziale (conta tutti i processi figli)
    arg.val = conf->nof_workers + conf->nof_users;
    if (semctl(sem_id, SEM_BARRIERA_START, SETVAL, arg) == -1) return -1;

    // 3. Postazioni per le stazioni
    arg.val = conf->nof_wk_seats[STAT_PRIMI];
    if (semctl(sem_id, SEM_POSTAZIONI_PRIMI, SETVAL, arg) == -1) return -1;

    arg.val = conf->nof_wk_seats[STAT_SECONDI];
    if (semctl(sem_id, SEM_POSTAZIONI_SECONDI, SETVAL, arg) == -1) return -1;

    arg.val = conf->nof_wk_seats[STAT_COFFEE];
    if (semctl(sem_id, SEM_POSTAZIONI_COFFEE, SETVAL, arg) == -1) return -1;

    arg.val = conf->nof_wk_seats[STAT_CASSA];
    if (semctl(sem_id, SEM_POSTAZIONI_CASSA, SETVAL, arg) == -1) return -1;

    // 4. Tavoli mensa
    arg.val = conf->nof_table_seats;
    if (semctl(sem_id, SEM_TAVOLI, SETVAL, arg) == -1) return -1;

    return sem_id;
}

int init_message_queues(int msg_queue_ids[NUM_STATIONS]) {
    for (int i = 0; i < NUM_STATIONS; i++) {
        key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + i);
        if (msg_key == -1) {
            perror("Errore ftok per coda messaggi");
            return -1;
        }

        msg_queue_ids[i] = msgget(msg_key, IPC_CREAT | IPC_EXCL | 0600);
        if (msg_queue_ids[i] == -1) {
            if (errno == EEXIST) {
                int old_id = msgget(msg_key, 0600);
                if (old_id != -1) {
                    msgctl(old_id, IPC_RMID, NULL);
                }
                msg_queue_ids[i] = msgget(msg_key, IPC_CREAT | IPC_EXCL | 0600);
            }
            if (msg_queue_ids[i] == -1) {
                perror("Errore creazione coda messaggi");
                return -1;
            }
        }
    }
    return 0;
}

// Handler per risvegliare le chiamate bloccanti su interruzione (EINTR)
static void term_handler(int signum) {
    (void)signum;
}

void setup_term_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = term_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

void attendi_nanosecondi(long nanosecs) {
    if (nanosecs <= 0) return;
    struct timespec req, rem;
    req.tv_sec = nanosecs / 1000000000L;
    req.tv_nsec = nanosecs % 1000000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

// Operazione P (Wait): decrementa e blocca se <= 0
int sem_p(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = 0;
    return semop(sem_id, &op, 1);
}

// Operazione V (Signal): incrementa e sblocca
int sem_v(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = 0;
    return semop(sem_id, &op, 1);
}

// Attende passivamente che il semaforo raggiunga il valore zero
int sem_wait_zero(int sem_id, int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 0; // Wait-for-zero
    op.sem_flg = 0;
    return semop(sem_id, &op, 1);
}