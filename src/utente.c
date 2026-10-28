#include "../include/common.h"
#include "../include/ipc_utils.h"

static void sigterm_handler(int signum) {
    (void)signum;
    _exit(EXIT_SUCCESS);
}

// Funzione helper per simulare il tempo di consumo pasti tramite nanosleep
static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;

    struct timespec req, rem;
    req.tv_sec = totale_nanosecs / 1000000000L;
    req.tv_nsec = totale_nanosecs % 1000000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

// Inoltra un ordine a una stazione, cronometra l'attesa e registra le metriche in SHM
static int ordina_a_stazione(int msg_qid, int piatto_scelto, pid_t my_pid,
                             StationType staz, SharedData *shm_ptr, int sem_id) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;               // Coda generica della stazione
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (msgsnd(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
        return -1;
    }

    // Attesa bloccante selettiva sul proprio PID
    if (msgrcv(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);

    double elapsed = (double)(t_end.tv_sec - t_start.tv_sec) +
                     (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;

    // Aggiornamento statistiche in SHM
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->tempo_attesa_oggi[staz] += elapsed;
    shm_ptr->tempo_attesa_tot[staz] += elapsed;
    shm_ptr->visite_stazione_oggi[staz]++;
    shm_ptr->visite_stazione_tot[staz]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    return msg.status; // 0 se ottenuto, -1 se esaurito
}

int main(int argc, char *argv[]) {
    // 0. Determinazione del gruppo (30/30)
    int group_id = -1;
    int is_dynamic = 0;

    if (argc > 1) {
        group_id = atoi(argv[1]);
    }
    if (argc > 2) {
        is_dynamic = atoi(argv[2]); // 1 se creato da aggiungi_utenti
    }

    // Registrazione handler per terminazione pulita
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    srand(getpid() ^ (unsigned int)time(NULL));
    pid_t my_pid = getpid();

    // 1. Aggancio Memoria Condivisa
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) {
        perror("[UTENTE] shmget");
        exit(EXIT_FAILURE);
    }

    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[UTENTE] shmat");
        exit(EXIT_FAILURE);
    }

    // 2. Recupero Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[UTENTE] semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 3. Recupero Code Messaggi per le 4 stazioni
    int msg_q[NUM_STATIONS];
    for (int i = 0; i < NUM_STATIONS; i++) {
        key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + i);
        msg_q[i] = msgget(msg_key, 0600);
        if (msg_q[i] == -1) {
            perror("[UTENTE] msgget");
            shmdt(shm_ptr);
            exit(EXIT_FAILURE);
        }
    }

    // 4. Barriera di sincronizzazione iniziale
    if (!is_dynamic) {
        sem_p(sem_id, SEM_BARRIERA_START);
        sem_wait_zero(sem_id, SEM_BARRIERA_START);
    }
    // 5. Ciclo di vita per giorni simulati
    int ultimo_giorno_servito = 0;

    // Profilo utente: 80% possiede il ticket (Sezione 6)
    int has_ticket = ((rand() % 100) < 80) ? 1 : 0;

    while (shm_ptr->simulazione_attiva) {
        int giorno_attuale = shm_ptr->giorno_corrente;

        if (giorno_attuale == ultimo_giorno_servito) {
            if (!shm_ptr->simulazione_attiva) break;
            struct timespec poll_time = {0, 10000000L}; // 10ms
            nanosleep(&poll_time, NULL);
            continue;
        }

        // --- FILA LETTURA TICKET (30/30) ---
        // Gli utenti con ticket fanno una coda preliminare di validazione
        if (has_ticket) {
            sem_p(sem_id, SEM_LETTORE_TICKET);
            struct timespec ts_ticket = {0, 2000000L}; // 2ms validazione
            nanosleep(&ts_ticket, NULL);
            sem_v(sem_id, SEM_LETTORE_TICKET);
        }

        int ha_primo = 0;
        int ha_secondo = 0;
        double conto_totale = 0.0;
        int num_portate = 0;

        // --- STAZIONE PRIMI ---
        int n_primi = shm_ptr->menu.num_primi > 0 ? shm_ptr->menu.num_primi : 2;
        int scelta_p = rand() % n_primi;

        if (ordina_a_stazione(msg_q[STAT_PRIMI], scelta_p % 2, my_pid, STAT_PRIMI, shm_ptr, sem_id) == 0) {
            ha_primo = 1;
        } else {
            if (ordina_a_stazione(msg_q[STAT_PRIMI], 1 - (scelta_p % 2), my_pid, STAT_PRIMI, shm_ptr, sem_id) == 0) {
                ha_primo = 1;
            }
        }
        if (ha_primo) {
            conto_totale += 4.50;
            num_portate++;
        }

        // --- STAZIONE SECONDI ---
        int n_secondi = shm_ptr->menu.num_secondi > 0 ? shm_ptr->menu.num_secondi : 2;
        int scelta_s = rand() % n_secondi;

        if (ordina_a_stazione(msg_q[STAT_SECONDI], scelta_s % 2, my_pid, STAT_SECONDI, shm_ptr, sem_id) == 0) {
            ha_secondo = 1;
        } else {
            if (ordina_a_stazione(msg_q[STAT_SECONDI], 1 - (scelta_s % 2), my_pid, STAT_SECONDI, shm_ptr, sem_id) == 0) {
                ha_secondo = 1;
            }
        }
        if (ha_secondo) {
            conto_totale += 5.50;
            num_portate++;
        }

        // Se entrambi i piatti sono esauriti, rinuncia ed esce
        if (!ha_primo && !ha_secondo) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->utenti_non_serviti_oggi++;
            shm_ptr->utenti_non_serviti_tot++;
            ultimo_giorno_servito = shm_ptr->giorno_corrente;
            sem_v(sem_id, SEM_MUTEX_SHM);
            continue;
        }

        // --- STAZIONE CAFFÈ (50% probabilità) ---
        if (rand() % 2 == 1) {
            int n_caffe = shm_ptr->menu.num_caffe > 0 ? shm_ptr->menu.num_caffe : 4;
            int scelta_c = rand() % n_caffe;
            if (ordina_a_stazione(msg_q[STAT_COFFEE], scelta_c, my_pid, STAT_COFFEE, shm_ptr, sem_id) == 0) {
                conto_totale += 1.20;
                num_portate++;
            }
        }

        // Se la giornata si è conclusa durante la scelta, rinuncia
        if (shm_ptr->giorno_corrente != giorno_attuale) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->utenti_non_serviti_oggi++;
            shm_ptr->utenti_non_serviti_tot++;
            ultimo_giorno_servito = shm_ptr->giorno_corrente;
            sem_v(sem_id, SEM_MUTEX_SHM);
            continue;
        }

        // --- SINCRONIZZAZIONE GRUPPO PRIMA DI PAGARE (Sezione 6) ---
        // Vanno a pagare solo dopo essersi riforniti di tutti i piatti desiderati
        if (group_id >= 0 && group_id < MAX_GROUPS) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->gruppi[group_id].ready_count++;
            if (shm_ptr->gruppi[group_id].ready_count >= shm_ptr->gruppi[group_id].total_members) {
                shm_ptr->gruppi[group_id].barrier_flag = 1; // Tutto il gruppo è pronto
            }
            sem_v(sem_id, SEM_MUTEX_SHM);

            // Attesa passiva che tutti i componenti abbiano ritirato i pasti
            while (shm_ptr->simulazione_attiva) {
                sem_p(sem_id, SEM_MUTEX_SHM);
                int gruppo_pronto = shm_ptr->gruppi[group_id].barrier_flag;
                sem_v(sem_id, SEM_MUTEX_SHM);

                if (gruppo_pronto || shm_ptr->giorno_corrente != giorno_attuale) {
                    break;
                }
                struct timespec ts_sync = {0, 2000000L}; // 2ms polling
                nanosleep(&ts_sync, NULL);
            }
        }

        // Controllo aggiuntivo fine giornata dopo l'attesa del gruppo
        if (shm_ptr->giorno_corrente != giorno_attuale) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->utenti_non_serviti_oggi++;
            shm_ptr->utenti_non_serviti_tot++;
            ultimo_giorno_servito = shm_ptr->giorno_corrente;
            sem_v(sem_id, SEM_MUTEX_SHM);
            continue;
        }

        // --- STAZIONE CASSA (con misurazione attesa e flag ticket) ---
        OrderMessage cassa_msg;
        memset(&cassa_msg, 0, sizeof(cassa_msg));
        cassa_msg.mtype = 1;
        cassa_msg.sender_pid = my_pid;
        cassa_msg.importo = conto_totale;
        cassa_msg.has_ticket = has_ticket; // Segnala se applicare sconto ticket

        struct timespec t_cassa_start, t_cassa_end;
        clock_gettime(CLOCK_MONOTONIC, &t_cassa_start);

        msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0);
        msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0);

        clock_gettime(CLOCK_MONOTONIC, &t_cassa_end);

        double elapsed_cassa = (double)(t_cassa_end.tv_sec - t_cassa_start.tv_sec) +
                               (double)(t_cassa_end.tv_nsec - t_cassa_start.tv_nsec) / 1e9;

        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->tempo_attesa_oggi[STAT_CASSA] += elapsed_cassa;
        shm_ptr->tempo_attesa_tot[STAT_CASSA] += elapsed_cassa;
        shm_ptr->visite_stazione_oggi[STAT_CASSA]++;
        shm_ptr->visite_stazione_tot[STAT_CASSA]++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // --- REFEZIONE (TAVOLI) ---
        sem_p(sem_id, SEM_TAVOLI);
        simula_consumo_pasto(num_portate, 5000000L);
        sem_v(sem_id, SEM_TAVOLI);

        // Aggiorna statistiche utenti serviti in mutua esclusione
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->utenti_serviti_oggi++;
        shm_ptr->utenti_serviti_tot++;
        ultimo_giorno_servito = shm_ptr->giorno_corrente;

        // Reset del flag di gruppo se tutti hanno completato
        if (group_id >= 0 && group_id < MAX_GROUPS) {
            shm_ptr->gruppi[group_id].ready_count--;
            if (shm_ptr->gruppi[group_id].ready_count <= 0) {
                shm_ptr->gruppi[group_id].ready_count = 0;
                shm_ptr->gruppi[group_id].barrier_flag = 0;
            }
        }
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    shmdt(shm_ptr);
    return 0;
}