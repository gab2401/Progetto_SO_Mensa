#include "common.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {
    setup_term_handler();
    (void)argc;
    (void)argv;

    srand((unsigned int)(getpid() ^ time(NULL)));

    // 1. Connessione alla Memoria Condivisa
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

    // Registra il PID del cassiere in SHM
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->pid_cassiere = getpid();
    sem_v(sem_id, SEM_MUTEX_SHM);

    // 3. Connessione alla Coda Messaggi della Cassa
    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + STAT_CASSA);
    int msg_q_cassa = msgget(msg_key, 0600);
    if (msg_q_cassa == -1) { 
        perror("[CASSIERE] msgget cassa"); 
        shmdt(shm_ptr); 
        exit(EXIT_FAILURE); 
    }

    // 4. Barriera d'avvio sincrono (Wait-for-Zero)
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Occupazione della postazione cassa
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
        // Controllo disservizio cassa con notifica a video se bloccata
        struct sembuf s_test;
        s_test.sem_num = SEM_CASSA_BLOCCO;
        s_test.sem_op = -1;
        s_test.sem_flg = IPC_NOWAIT;

        if (semop(sem_id, &s_test, 1) == -1) {
            if (errno == EAGAIN) {
                printf("[CASSIERE PID %d] ATTENZIONE: Disservizio di rete (POS bloccato), attesa ripristino...\n", getpid());
                // Attesa bloccante del ripristino
                if (sem_p(sem_id, SEM_CASSA_BLOCCO) == -1) {
                    if (errno == EINTR || errno == EIDRM) break;
                }
                sem_v(sem_id, SEM_CASSA_BLOCCO);
                printf("[CASSIERE PID %d] Rete ripristinata. Ripresa servizio pagamenti.\n", getpid());
            } else if (errno == EINTR || errno == EIDRM) {
                break;
            }
        } else {
            // Rilascio immediato del token preso per il test
            s_test.sem_op = 1;
            s_test.sem_flg = 0;
            semop(sem_id, &s_test, 1);
        }

        // Ricezione richiesta utente (mtype = 1)
        ssize_t res = msgrcv(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (res == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        // Verifica validità temporale dell'ordine
        sem_p(sem_id, SEM_MUTEX_SHM);
        int g_curr = shm_ptr->giorno_corrente;
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (req.giorno != g_curr) {
            req.status = -2;
            req.mtype = req.sender_pid;
            msgsnd(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }

        // Ricontrollo blocco cassa prima di procedere al pagamento
        if (sem_p(sem_id, SEM_CASSA_BLOCCO) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }
        sem_v(sem_id, SEM_CASSA_BLOCCO);

        // Calcolo dell'importo
        double importo = 0.0;
        if (req.ha_primo)   importo += shm_ptr->conf.price_primi;
        if (req.ha_secondo) importo += shm_ptr->conf.price_secondi;
        if (req.ha_coffee)  importo += shm_ptr->conf.price_coffee;

        if (req.has_ticket) {
            double sconto = shm_ptr->conf.ticket_discount;
            if (sconto <= 0.0 || sconto > 1.0) sconto = 0.20;
            importo = importo * (1.0 - sconto);
        }

        // Simulazione servizio (AVG_SRVC_CASSA con ±20%)
        simula_servizio(shm_ptr->conf.avg_srvc[STAT_CASSA], 20, shm_ptr->conf.n_nano_secs);

        // Aggiornamento immediato in SHM dell'incasso
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->ricavo_oggi += importo;
        shm_ptr->ricavo_totale += importo;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Risposta mirata all'utente (mtype = sender_pid)
        OrderMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.mtype = req.sender_pid;
        resp.sender_pid = getpid();
        resp.status = 0;
        resp.importo_calcolato = importo;
        resp.has_ticket = req.has_ticket;

        msgsnd(msg_q_cassa, &resp, sizeof(OrderMessage) - sizeof(long), 0);
    }

    // Chiusura e rilascio postazione
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