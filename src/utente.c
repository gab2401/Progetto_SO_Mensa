#include "common.h"
#include "ipc_utils.h"

// Handler per garantire l'uscita immediata su ricezione di SIGTERM
static void sigterm_handler(int signum) {
    (void)signum;
    _exit(EXIT_SUCCESS);
}

// Funzione helper per simulare il consumo pasti tramite nanosleep
static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;
    attendi_nanosecondi(totale_nanosecs);
}

// Inoltra un ordine a una stazione generica, misura il tempo di attesa e aggiorna la SHM
static int ordina_a_stazione(int msg_qid, int piatto_scelto, pid_t my_pid, 
                             int station_id, SharedData *shm_ptr, int sem_id) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1; // Canale generico della stazione
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (msgsnd(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
        return -1;
    }

    if (msgrcv(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);

    // Aggiornamento atomico delle metriche di attesa
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->tempo_attesa_totale[station_id] += elapsed_ns;
    shm_ptr->ordini_completati[station_id]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    return msg.status;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Registrazione gestore di terminazione
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    srand(getpid() ^ time(NULL));
    pid_t my_pid = getpid();

    // 1. Connessione alla Memoria Condivisa
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if (shm_key == -1) {
        perror("[UTENTE] Errore ftok SHM");
        exit(EXIT_FAILURE);
    }
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) {
        perror("[UTENTE] Errore shmget");
        exit(EXIT_FAILURE);
    }
    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[UTENTE] Errore shmat");
        exit(EXIT_FAILURE);
    }

    // 2. Connessione ai Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[UTENTE] Errore semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 3. Connessione alle Code di Messaggi
    int msg_q[NUM_STATIONS];
    for (int i = 0; i < NUM_STATIONS; i++) {
        key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + i);
        msg_q[i] = msgget(msg_key, 0600);
        if (msg_q[i] == -1) {
            perror("[UTENTE] Errore msgget");
            shmdt(shm_ptr);
            exit(EXIT_FAILURE);
        }
    }

    // 4. Barriera di sincronizzazione iniziale
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Ciclo principale
    int ultimo_giorno_servito = 0;

    while (shm_ptr->simulazione_attiva) {
        int giorno_attuale = shm_ptr->giorno_corrente;

        if (giorno_attuale == ultimo_giorno_servito) {
            if (!shm_ptr->simulazione_attiva) break;
            struct timespec poll_time = {0, 10000000L};
            nanosleep(&poll_time, NULL);
            continue;
        }

        int ha_primo = 0;
        int ha_secondo = 0;
        int ha_caffe = 0;
        double conto_totale = 0.0;
        int num_portate = 0;

        // --- STAZIONE PRIMI ---
        int scelta_p = rand() % 2;
        if (ordina_a_stazione(msg_q[STAT_PRIMI], scelta_p, my_pid, STAT_PRIMI, shm_ptr, sem_id) == 0) {
            ha_primo = 1;
        } else {
            if (ordina_a_stazione(msg_q[STAT_PRIMI], 1 - scelta_p, my_pid, STAT_PRIMI, shm_ptr, sem_id) == 0) {
                ha_primo = 1;
            }
        }
        if (ha_primo) {
            conto_totale += shm_ptr->conf.price_primi;
            num_portate++;
        }

        // --- STAZIONE SECONDI ---
        int scelta_s = rand() % 2;
        if (ordina_a_stazione(msg_q[STAT_SECONDI], scelta_s, my_pid, STAT_SECONDI, shm_ptr, sem_id) == 0) {
            ha_secondo = 1;
        } else {
            if (ordina_a_stazione(msg_q[STAT_SECONDI], 1 - scelta_s, my_pid, STAT_SECONDI, shm_ptr, sem_id) == 0) {
                ha_secondo = 1;
            }
        }
        if (ha_secondo) {
            conto_totale += shm_ptr->conf.price_secondi;
            num_portate++;
        }

        // Rinuncia se entrambi i piatti sono esauriti
        if (!ha_primo && !ha_secondo) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->utenti_non_serviti_oggi++;
            shm_ptr->utenti_non_serviti_tot++;
            sem_v(sem_id, SEM_MUTEX_SHM);
            ultimo_giorno_servito = giorno_attuale;
            continue;
        }

        // --- STAZIONE CAFFÈ (50% di probabilità) ---
        if (rand() % 2 == 1) {
            if (ordina_a_stazione(msg_q[STAT_COFFEE], rand() % 4, my_pid, STAT_COFFEE, shm_ptr, sem_id) == 0) {
                ha_caffe = 1;
                conto_totale += shm_ptr->conf.price_coffee;
                num_portate++;
            }
        }

        // --- STAZIONE CASSA ---
        OrderMessage cassa_msg;
        memset(&cassa_msg, 0, sizeof(cassa_msg));
        cassa_msg.mtype = 1;
        cassa_msg.sender_pid = my_pid;
        cassa_msg.importo = conto_totale;

        struct timespec start_c, end_c;
        clock_gettime(CLOCK_MONOTONIC, &start_c);

        if (msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0) != -1) {
            if (msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) != -1) {
                clock_gettime(CLOCK_MONOTONIC, &end_c);
                long elapsed_cassa = (end_c.tv_sec - start_c.tv_sec) * 1000000000L + (end_c.tv_nsec - start_c.tv_nsec);

                sem_p(sem_id, SEM_MUTEX_SHM);
                shm_ptr->tempo_attesa_totale[STAT_CASSA] += elapsed_cassa;
                shm_ptr->ordini_completati[STAT_CASSA]++;
                sem_v(sem_id, SEM_MUTEX_SHM);
            }
        }

        // --- REFEZIONE (TAVOLI) ---
        if (sem_p(sem_id, SEM_TAVOLI) != -1) {
            simula_consumo_pasto(num_portate, shm_ptr->conf.n_nano_secs);
            sem_v(sem_id, SEM_TAVOLI);
        }

        // Registrazione completamento servizio protetta da controllo di coerenza
        sem_p(sem_id, SEM_MUTEX_SHM);
        if (shm_ptr->giorno_corrente == giorno_attuale) {
            shm_ptr->utenti_serviti_oggi++;
        }
        shm_ptr->utenti_serviti_tot++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        ultimo_giorno_servito = giorno_attuale;
    }

    shmdt(shm_ptr);
    return 0;
}