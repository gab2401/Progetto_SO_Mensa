#include "common.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {

    void term_handler(int signum) {
    (void)signum;
    // Serve solo per interrompere le system call bloccanti impostando errno = EINTR
    }

    (void)argc;
    (void)argv;

    // 1. Recupero chiavi e aggancio alla Shared Memory
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

    // 2. Recupero del Set di Semafori (sem_id viene dichiarato qui)
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

    // 4. Barriera di sincronizzazione iniziale: notifica e attesa del via libera
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);
    printf("[CASSIERE PID %d] Ricevuto via libera. Tento accesso postazione cassa...\n", getpid());

    // 5. Occupazione della postazione cassa
    if (sem_p(sem_id, SEM_POSTAZIONI_CASSA) == -1) {
        perror("[CASSIERE] Errore acquisizione postazione");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    printf("[CASSIERE PID %d] Postazione occupata. In servizio.\n", getpid());

    // 6. Ciclo di servizio
    OrderMessage msg;
    while (shm_ptr->simulazione_attiva) {
        ssize_t bytes = msgrcv(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (bytes == -1) {
            if (errno == EINTR || errno == EIDRM) {
                break;
            }
            continue;
        }

        // Sezione critica: aggiornamento ricavo
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->ricavo_oggi += msg.importo;
        shm_ptr->ricavo_totale += msg.importo;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Risposta mirata all'utente
        msg.mtype = msg.sender_pid;
        msg.status = 0;
        msgsnd(msg_q_cassa, &msg, sizeof(OrderMessage) - sizeof(long), 0);
    }

    // Rilascio postazione prima dell'uscita
    sem_v(sem_id, SEM_POSTAZIONI_CASSA);
    shmdt(shm_ptr);
    printf("[CASSIERE PID %d] Chiusura servizio.\n", getpid());
    return 0;
}