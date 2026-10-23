#include "../include/common.h"
#include "../include/ipc_utils.h"

static void sigterm_handler(int signum) {
    (void)signum;
    _exit(EXIT_SUCCESS); // Termina immediatamente il processo figlio senza passare per pulizie complesse
}

// Funzione helper per simulare il tempo di consumo pasti tramite nanosleep
static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    // Tempo proporzionale al numero di portate (es. 2 minuti virtuali a piatto)
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;

    struct timespec req, rem;
    req.tv_sec = totale_nanosecs / 1000000000L;
    req.tv_nsec = totale_nanosecs % 1000000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

// Inoltra un ordine a una stazione e attende la risposta selettiva
static int ordina_a_stazione(int msg_qid, int piatto_scelto, pid_t my_pid) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;              // Coda generica della stazione
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;

    if (msgsnd(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
        return -1;
    }

    // Attesa bloccante selettiva sul proprio PID
    if (msgrcv(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
        return -1;
    }

    return msg.status; // 0 se ottenuto, -1 se esaurito
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    // Registrazione handler per terminazione pulita
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    srand(getpid() ^ time(NULL));
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
        shmdt(shm_ptr); exit(EXIT_FAILURE); 
    }

    // 3. Recupero Code Messaggi per le 4 stazioni
    int msg_q[NUM_STATIONS];
    for (int i = 0; i < NUM_STATIONS; i++) {
        key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + i);
        msg_q[i] = msgget(msg_key, 0600);
        if (msg_q[i] == -1) { perror("[UTENTE] msgget"); shmdt(shm_ptr); exit(EXIT_FAILURE); }
    }

    // 4. Barriera di sincronizzazione iniziale
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Ciclo di vita per giorni simulati
    int ultimo_giorno_servito = 0;

    while (shm_ptr->simulazione_attiva) {
        int giorno_attuale = shm_ptr->giorno_corrente;

        // Esegue il giro mensa una sola volta al giorno
        if (giorno_attuale == ultimo_giorno_servito) {
            if (!shm_ptr->simulazione_attiva) {
                break; // Se la simulazione è finita, esce dal while
            }
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
        if (ordina_a_stazione(msg_q[STAT_PRIMI], scelta_p, my_pid) == 0) {
            ha_primo = 1;
        } else {
            // Piatto esaurito: prova l'alternativa (1 - scelta)
            if (ordina_a_stazione(msg_q[STAT_PRIMI], 1 - scelta_p, my_pid) == 0) {
                ha_primo = 1;
            }
        }
        if (ha_primo) { 
            conto_totale += 4.50; 
            num_portate++; 
        }

        // --- STAZIONE SECONDI ---
        int scelta_s = rand() % 2;
        if (ordina_a_stazione(msg_q[STAT_SECONDI], scelta_s, my_pid) == 0) {
            ha_secondo = 1;
        } else {
            if (ordina_a_stazione(msg_q[STAT_SECONDI], 1 - scelta_s, my_pid) == 0) {
                ha_secondo = 1;
            }
        }
        if (ha_secondo) { conto_totale += 5.50; num_portate++; }

        // Se entrambi i tipi di piatto sono terminati, rinuncia ed esce per oggi
        if (!ha_primo && !ha_secondo) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->utenti_non_serviti_oggi++;
            shm_ptr->utenti_non_serviti_tot++;
            sem_v(sem_id, SEM_MUTEX_SHM);
            ultimo_giorno_servito = giorno_attuale;
            continue;
        }

        // --- STAZIONE CAFFÈ (50% probabilità) ---
        if (rand() % 2 == 1) {
            if (ordina_a_stazione(msg_q[STAT_COFFEE], rand() % 4, my_pid) == 0) {
                ha_caffe = 1;
                conto_totale += 1.20;
                num_portate++;
            }
        }

        // --- STAZIONE CASSA ---
        OrderMessage cassa_msg;
        memset(&cassa_msg, 0, sizeof(cassa_msg));
        cassa_msg.mtype = 1;
        cassa_msg.sender_pid = my_pid;
        cassa_msg.importo = conto_totale;
        msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0);
        msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0);

        // --- REFEZIONE (TAVOLI) ---
        // Attesa passiva gestita dal kernel finché non c'è un posto libero
        sem_p(sem_id, SEM_TAVOLI);
        simula_consumo_pasto(num_portate, 5000000L); // Consumo pasto
        sem_v(sem_id, SEM_TAVOLI); // Libera il tavolo

        // Aggiorna statistiche utenti serviti
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->utenti_serviti_oggi++;
        shm_ptr->utenti_serviti_tot++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        ultimo_giorno_servito = giorno_attuale;
    }

    shmdt(shm_ptr);
    return 0;
}