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

// Calcola un tempo di erogazione casuale nell'intorno +-20% e attende in nanosecondi simulati
// Calcola un tempo di erogazione casuale nell'intorno +-20% e attende in nanosecondi simulati
static void simula_tempo_cassa(int tempo_cassa, long n_nano_secs) {
    if (tempo_cassa <= 0) return;

    double fattore = 0.8 + ((double)rand() / RAND_MAX) * 0.4;
    double tempo_sec = tempo_cassa * fattore;

    long long total_ns = (long long)((tempo_sec * n_nano_secs));
    if (total_ns < 1000LL) {
        total_ns = 1000LL; // Almeno 1 microsecondo
    }

    struct timespec req, rem;
    req.tv_sec = (time_t)(total_ns / 1000000000LL);
    req.tv_nsec = (long)(total_ns % 1000000000LL);

    // Gestione interruzione da segnali con ripresa sui nanosecondi rimanenti
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        if (!keep_running) break;
        req = rem;
    }
}
int main(int argc, char *argv[]) {
    srand((unsigned int)(time(NULL) ^ (getpid() << 16)));

    struct sigaction sa;
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    Config cfg;
    const char *conf_file = (argc > 1) ? argv[1] : "config.conf";
    if (carica_configurazione(conf_file, &cfg) != 0) {
        fprintf(stderr, "[Cassa %d] Errore caricamento configurazione\n", getpid());
        exit(EXIT_FAILURE);
    }

    int worker_id = (argc > 2) ? atoi(argv[2]) : 0;

    // 1. Recupero chiavi IPC
    key_t shm_key = ftok(FTOK_PATH, SHM_PROJ_ID);
    key_t sem_key = ftok(FTOK_PATH, SEM_PROJ_ID);
    key_t msg_cassa_key = ftok(FTOK_PATH, MSG_CASSA_PROJ_ID);

    if (shm_key == -1 || sem_key == -1 || msg_cassa_key == -1) {
        perror("[Cassa] Errore ftok");
        exit(EXIT_FAILURE);
    }

    // 2. Recupero identificatori IPC
    int shmid = shm_get_or_create(shm_key, sizeof(SharedData), 0666);
    int semid = sem_get_or_create(sem_key, NUM_SEMAPHORES, 0666);
    int msqid_cassa = msg_get_or_create(msg_cassa_key, 0666);

    if (shmid == -1 || semid == -1 || msqid_cassa == -1) {
        fprintf(stderr, "[Cassa %d] Errore aggancio risorse IPC\n", getpid());
        exit(EXIT_FAILURE);
    }

    // 3. Attach alla memoria condivisa
    SharedData *shm = (SharedData *)shm_attach(shmid);
    if (!shm) {
        perror("[Cassa] Errore shm_attach");
        exit(EXIT_FAILURE);
    }

    // Attesa sincronizzazione iniziale con il responsabile
    sem_wait_op(semid, SEM_BARRIERA_AVVIO);

    // Conquista della postazione cassa fisica
    sem_wait_op(semid, SEM_POSTAZIONI_CASSA);

    // 4. Ciclo di servizio della cassa
    while (keep_running && shm->sim_attiva) {
        OrderMessage req;

        // Ricezione bloccante per richieste cassa (mtype = 1)
        ssize_t res = msgrcv(msqid_cassa, &req, ORDER_MSG_SIZE, 1, 0);
        if (res == -1) {
            if (errno == EINTR) {
                continue;
            } else if (errno == EIDRM || errno == EINVAL) {
                break; // Risorsa deallocata dal responsabile
            } else {
                perror("[Cassa] Errore msgrcv");
                break;
            }
        }

        // Calcolo effettivo dell'importo basato sui prezzi di configurazione
        double scontrino = 0.0;
        if (req.ha_primo)   scontrino += cfg.prezzo_primo;
        if (req.ha_secondo) scontrino += cfg.prezzo_secondo;
        if (req.ha_caffe)   scontrino += cfg.prezzo_caffe;

        // Simulazione tempo di erogazione del servizio (+- 20%)
        simula_tempo_cassa(cfg.tempo_cassa, cfg.n_nano_secs);

        // Aggiornamento incasso e tracciamento operatore attivo
        sem_wait_op(semid, SEM_MUTEX_SHM);
        shm->ricavo_giornaliero += scontrino;
        shm->ricavo_totale      += scontrino;

        if (worker_id >= 0 && worker_id < 128) {
            if (!shm->operatore_ha_lavorato_oggi[worker_id]) {
                shm->operatore_ha_lavorato_oggi[worker_id] = 1;
                shm->operatori_attivi_oggi++;
                shm->operatori_attivi_totali++;
            }
        }
        sem_signal_op(semid, SEM_MUTEX_SHM);

        // Risposta all'utente pagante (mtype = PID dell'utente)
        OrderMessage resp;
        resp.mtype = req.sender_pid;
        resp.sender_pid = getpid();
        resp.ordine_accettato = true;
        resp.importo = scontrino;

        if (msgsnd(msqid_cassa, &resp, ORDER_MSG_SIZE, 0) == -1) {
            if (errno != EIDRM && errno != EINVAL) {
                perror("[Cassa] Errore msgsnd risposta scontrino");
            }
        }
    }

    // Rilascio della postazione fisica
    sem_signal_op(semid, SEM_POSTAZIONI_CASSA);

    shm_detach(shm);
    return 0;
}