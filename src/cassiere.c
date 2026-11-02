#include "common.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {
    setup_term_handler();
    (void)argc;
    (void)argv;

    srand(getpid() ^ (unsigned int)time(NULL));

    // 1. Aggancio alla Memoria Condivisa
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) {
        perror("[CASSIERE] shmget");
        exit(EXIT_FAILURE);
    }
    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[CASSIERE] shmat");
        exit(EXIT_FAILURE);
    }

    // 2. Connessione ai Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[CASSIERE] semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // Registrazione PID per tracciamento
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->pid_cassiere = getpid();
    sem_v(sem_id, SEM_MUTEX_SHM);

    // 3. Connessione Coda Messaggi Cassa
    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + STAT_CASSA);
    int msg_q_cassa = msgget(msg_key, 0600);
    if (msg_q_cassa == -1) {
        perror("[CASSIERE] msgget cassa");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 4. Barriera iniziale
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Occupazione postazione fisica
    if (sem_p(sem_id, SEM_POSTAZIONI_CASSA) == -1) {
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->operatori_in_servizio[STAT_CASSA]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    printf("[CASSIERE PID %d] Postazione occupata. In servizio.\n", getpid());

    OrderMessage req;
    while (shm_ptr->simulazione_attiva) {
        // Verifica disponibilità cassa (se bloccata da communication_disorder attende)
        if (sem_p(sem_id, SEM_CASSA_BLOCCO) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }
        sem_v(sem_id, SEM_CASSA_BLOCCO);

        ssize_t res = msgrcv(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (res == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        sem_p(sem_id, SEM_MUTEX_SHM);
        int g_curr = shm_ptr->giorno_corrente;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Controllo validità giorno
        if (req.giorno != g_curr) {
            req.mtype = req.sender_pid;
            req.status = -2;
            msgsnd(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }

        // Verifica blocco cassa anche prima di elaborare l'ordine
        if (sem_p(sem_id, SEM_CASSA_BLOCCO) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }
        sem_v(sem_id, SEM_CASSA_BLOCCO);

        // Calcolo importo con eventuale sconto ticket da configurazione
        double importo = req.importo;
        if (req.has_ticket) {
            double sconto = shm_ptr->conf.ticket_discount;
            if (sconto <= 0.0 || sconto > 1.0) sconto = 0.20;
            importo = importo * (1.0 - sconto);
        }

        // Servizio cassa basato sui parametri di configurazione
        simula_servizio(shm_ptr->conf.avg_srvc[STAT_CASSA], 20, shm_ptr->conf.n_nano_secs);

        sem_p(sem_id, SEM_MUTEX_SHM);
        if (req.giorno != shm_ptr->giorno_corrente) {
            sem_v(sem_id, SEM_MUTEX_SHM);
            req.mtype = req.sender_pid;
            req.status = -2;
            msgsnd(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }
        shm_ptr->ricavo_oggi += importo;
        shm_ptr->ricavo_totale += importo;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Risposta all'utente
        OrderMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.mtype = req.sender_pid;
        resp.sender_pid = getpid();
        resp.status = 0;
        resp.importo = importo;
        resp.has_ticket = req.has_ticket;

        msgsnd(msg_q_cassa, &resp, sizeof(OrderMessage) - sizeof(long), 0);
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