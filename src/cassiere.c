#include "common.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {
    setup_term_handler();
    (void)argc;
    (void)argv;

    srand(getpid() ^ time(NULL));

    // 1. Aggancio alla Shared Memory
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if (shm_key == -1) {
        perror("[CASSIERE] Errore ftok SHM");
        exit(EXIT_FAILURE);
    }
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) {
        perror("[CASSIERE] Errore shmget");
        exit(EXIT_FAILURE);
    }
    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[CASSIERE] Errore shmat");
        exit(EXIT_FAILURE);
    }

    // 2. Recupero del Set di Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[CASSIERE] Errore semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 3. Recupero della coda messaggi per la cassa
    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + STAT_CASSA);
    int msg_q_cassa = msgget(msg_key, 0600);
    if (msg_q_cassa == -1) {
        perror("[CASSIERE] Errore msgget cassa");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 4. Barriera di sincronizzazione iniziale
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Occupazione della postazione cassa
    if (sem_p(sem_id, SEM_POSTAZIONI_CASSA) == -1) {
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->operatori_in_servizio[STAT_CASSA]++;
    shm_ptr->operatori_attivi_oggi++;
    shm_ptr->operatori_attivi_tot++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    printf("[CASSIERE PID %d] Postazione occupata. In servizio.\n", getpid());

    // 6. Ciclo di servizio
    OrderMessage msg;
    int giorno_locale = 1;

    while (shm_ptr->simulazione_attiva) {
        ssize_t bytes = msgrcv(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (bytes == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        sem_p(sem_id, SEM_MUTEX_SHM);
        int g_curr = shm_ptr->giorno_corrente;
        if (g_curr != giorno_locale) {
            giorno_locale = g_curr;
            shm_ptr->operatori_attivi_oggi++;
            shm_ptr->operatori_attivi_tot++;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Se l'ordine appartiene a una giornata già conclusa, l'utente deve abbandonare
        if (msg.giorno != g_curr) {
            msg.mtype = msg.sender_pid;
            msg.status = -2;
            msgsnd(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }

        // Simulazione tempo di servizio cassa (AVG_SRVC_CASSA ± 20%)
        simula_servizio(shm_ptr->conf.avg_srvc[STAT_CASSA], 20, shm_ptr->conf.n_nano_secs);

        // Ri-verifica se la giornata è scaduta durante il servizio
        sem_p(sem_id, SEM_MUTEX_SHM);
        if (msg.giorno != shm_ptr->giorno_corrente) {
            sem_v(sem_id, SEM_MUTEX_SHM);
            msg.mtype = msg.sender_pid;
            msg.status = -2;
            msgsnd(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }
        shm_ptr->ricavo_oggi += msg.importo;
        shm_ptr->ricavo_totale += msg.importo;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Risposta mirata all'utente
        msg.mtype = msg.sender_pid;
        msg.status = 0;
        msgsnd(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 0);
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    if (shm_ptr->operatori_in_servizio[STAT_CASSA] > 0) {
        shm_ptr->operatori_in_servizio[STAT_CASSA]--;
    }
    sem_v(sem_id, SEM_MUTEX_SHM);

    sem_v(sem_id, SEM_POSTAZIONI_CASSA);
    shmdt(shm_ptr);
    printf("[CASSIERE PID %d] Chiusura servizio.\n", getpid());
    return 0;
}