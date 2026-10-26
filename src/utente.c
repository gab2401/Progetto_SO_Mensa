#include "common.h"
#include "ipc_utils.h"

static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;
    attendi_nanosecondi(totale_nanosecs);
}

static int ordina_a_stazione(int msg_qid, int piatto_scelto, pid_t my_pid,
                             int station_id, int giorno_attuale,
                             SharedData *shm_ptr, int sem_id) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;
    msg.giorno = giorno_attuale;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (msgsnd(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
        return -2;
    }

    if (msgrcv(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
        return -2;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ns = (end.tv_sec - start.tv_sec) * 1000000000L + (end.tv_nsec - start.tv_nsec);

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->tempo_attesa_oggi[station_id] += elapsed_ns;
    shm_ptr->ordini_completati_oggi[station_id]++;
    shm_ptr->tempo_attesa_totale[station_id] += elapsed_ns;
    shm_ptr->ordini_completati[station_id]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    return msg.status;
}

static void registra_esito_utente(SharedData *shm_ptr, int sem_id, int servito) {
    sem_p(sem_id, SEM_MUTEX_SHM);
    if (servito) {
        shm_ptr->utenti_serviti_oggi++;
        shm_ptr->utenti_serviti_tot++;
    } else {
        shm_ptr->utenti_non_serviti_oggi++;
        shm_ptr->utenti_non_serviti_tot++;
    }
    shm_ptr->utenti_finito_giornata++;
    sem_v(sem_id, SEM_MUTEX_SHM);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    setup_term_handler();

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

    // 5. Ciclo principale sincronizzato sul semaforo di inizio giornata (ZERO POLLING)
    while (shm_ptr->simulazione_attiva) {
        if (sem_p(sem_id, SEM_INIZIO_GIORNATA) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        if (!shm_ptr->simulazione_attiva) break;

        sem_p(sem_id, SEM_MUTEX_SHM);
        int giorno_attuale = shm_ptr->giorno_corrente;
        sem_v(sem_id, SEM_MUTEX_SHM);

        int ha_primo = 0;
        int ha_secondo = 0;
        double conto_totale = 0.0;
        int num_portate = 0;

        // --- STAZIONE PRIMI ---
        int scelta_p = rand() % 2;
        int esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], scelta_p, my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
        if (esito_p == -2) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        } else if (esito_p == 0) {
            ha_primo = 1;
        } else {
            esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], 1 - scelta_p, my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
            if (esito_p == -2) {
                registra_esito_utente(shm_ptr, sem_id, 0);
                continue;
            } else if (esito_p == 0) {
                ha_primo = 1;
            }
        }
        if (ha_primo) {
            conto_totale += shm_ptr->conf.price_primi;
            num_portate++;
        }

        // --- STAZIONE SECONDI ---
        int scelta_s = rand() % 2;
        int esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], scelta_s, my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
        if (esito_s == -2) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        } else if (esito_s == 0) {
            ha_secondo = 1;
        } else {
            esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], 1 - scelta_s, my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
            if (esito_s == -2) {
                registra_esito_utente(shm_ptr, sem_id, 0);
                continue;
            } else if (esito_s == 0) {
                ha_secondo = 1;
            }
        }
        if (ha_secondo) {
            conto_totale += shm_ptr->conf.price_secondi;
            num_portate++;
        }

        // Rinuncia se entrambi i piatti sono esauriti
        if (!ha_primo && !ha_secondo) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // --- STAZIONE CAFFÈ (50% di probabilità) ---
        if (rand() % 2 == 1) {
            int esito_c = ordina_a_stazione(msg_q[STAT_COFFEE], rand() % 4, my_pid, STAT_COFFEE, giorno_attuale, shm_ptr, sem_id);
            if (esito_c == -2) {
                registra_esito_utente(shm_ptr, sem_id, 0);
                continue;
            } else if (esito_c == 0) {
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
        cassa_msg.giorno = giorno_attuale;

        struct timespec start_c, end_c;
        clock_gettime(CLOCK_MONOTONIC, &start_c);

        if (msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }
        if (msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &end_c);
        long elapsed_cassa = (end_c.tv_sec - start_c.tv_sec) * 1000000000L + (end_c.tv_nsec - start_c.tv_nsec);

        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->tempo_attesa_oggi[STAT_CASSA] += elapsed_cassa;
        shm_ptr->ordini_completati_oggi[STAT_CASSA]++;
        shm_ptr->tempo_attesa_totale[STAT_CASSA] += elapsed_cassa;
        shm_ptr->ordini_completati[STAT_CASSA]++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (cassa_msg.status == -2) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // --- REFEZIONE (TAVOLI) ---
        if (sem_p(sem_id, SEM_TAVOLI) != -1) {
            simula_consumo_pasto(num_portate, shm_ptr->conf.n_nano_secs);
            sem_v(sem_id, SEM_TAVOLI);
        } else {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // Registra il pasto completato
        registra_esito_utente(shm_ptr, sem_id, 1);
    }

    shmdt(shm_ptr);
    return 0;
}