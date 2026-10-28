#include "../include/common.h"
#include "../include/ipc_utils.h"
#include "../include/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>

static volatile sig_atomic_t keep_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    keep_running = 0;
}

// Simula il tempo di servizio cassa con scostamento percentuale (+-20%)
static void simula_tempo_cassa(int tempo_cassa, long n_nano_secs) {
    if (tempo_cassa <= 0) return;

    double fattore = 0.8 + ((double)rand() / RAND_MAX) * 0.4;
    double tempo_sec = tempo_cassa * fattore;

    long long total_ns = (long long)(tempo_sec * n_nano_secs);
    if (total_ns < 1000LL) {
        total_ns = 1000LL;
    }

    struct timespec req, rem;
    req.tv_sec = (time_t)(total_ns / 1000000000LL);
    req.tv_nsec = (long)(total_ns % 1000000000LL);

    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        if (!keep_running) break;
        req = rem;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. Recupero chiavi e risorse IPC condivise
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

    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[CASSIERE] semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + STAT_CASSA);
    int msg_q_cassa = msgget(msg_key, 0600);
    if (msg_q_cassa == -1) {
        perror("[CASSIERE] msgget cassa");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 2. Barriera di sincronizzazione all'avvio
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 3. Occupazione della postazione fisica di cassa
    sem_p(sem_id, SEM_POSTAZIONI_CASSA);

    int giorno_locale = 0;

    // 4. Ciclo di servizio richieste cassa
    while (keep_running && shm_ptr->simulazione_attiva) {
        // Registrazione presenza all'inizio di ciascuna nuova giornata
        if (shm_ptr->giorno_corrente != giorno_locale) {
            giorno_locale = shm_ptr->giorno_corrente;
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->operatori_attivi_oggi++;
            sem_v(sem_id, SEM_MUTEX_SHM);
        }

        // Verifica blocco cassa (communication_disorder) prima di servire
        sem_p(sem_id, SEM_CASSA_BLOCCO);
        sem_v(sem_id, SEM_CASSA_BLOCCO);

        OrderMessage req;
        ssize_t res = msgrcv(msg_q_cassa, &req, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (res == -1) {
            if (errno == EINTR) {
                continue;
            } else if (errno == EIDRM || errno == EINVAL) {
                break;
            } else {
                perror("[CASSIERE] msgrcv");
                break;
            }
        }

        // Se communication_disorder scatta durante l'elaborazione del pagamento
        sem_p(sem_id, SEM_CASSA_BLOCCO);
        sem_v(sem_id, SEM_CASSA_BLOCCO);

        // Calcolo importo con eventuale sconto ticket (30/30)
        double importo = req.importo;
        if (req.has_ticket) {
            importo = importo * 0.80; // 20% di sconto per utenti muniti di ticket
        }

        // Simulazione erogazione servizio cassa (2 sec medi simulati)
        simula_tempo_cassa(2, 5000000L);

        // Aggiornamento incasso in memoria condivisa
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->ricavo_oggi   += importo;
        shm_ptr->ricavo_totale += importo;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Risposta mirata al PID dell'utente pagante
        OrderMessage resp;
        memset(&resp, 0, sizeof(resp));
        resp.mtype = req.sender_pid;
        resp.sender_pid = getpid();
        resp.status = 0;
        resp.importo = importo;
        resp.has_ticket = req.has_ticket;

        if (msgsnd(msg_q_cassa, &resp, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
            if (errno != EIDRM && errno != EINVAL) {
                perror("[CASSIERE] msgsnd risposta scontrino");
            }
        }
    }

    // 5. Rilascio postazione fisica e distacco memoria condivisa
    sem_v(sem_id, SEM_POSTAZIONI_CASSA);
    printf("[CASSIERE PID %d] Chiusura servizio.\n", getpid());
    shmdt(shm_ptr);

    return 0;
}