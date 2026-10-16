#include "common.h"
#include "ipc_utils.h"

// Funzione helper per simulare il tempo di servizio con variazione percentuale
static void simula_servizio(int tempo_medio_sec, int variazione_percentuale, long n_nano_secs) {
    if (tempo_medio_sec <= 0) return;

    // Calcolo scostamento casuale tra -variazione e +variazione
    int delta_percentuale = (rand() % (2 * variazione_percentuale + 1)) - variazione_percentuale;
    double fattore = 1.0 + (delta_percentuale / 100.0);
    double minuti_virtuali = tempo_medio_sec * fattore;

    long totale_nanosecs = (long)(minuti_virtuali * n_nano_secs);

    struct timespec req, rem;
    req.tv_sec = totale_nanosecs / 1000000000L;
    req.tv_nsec = totale_nanosecs % 1000000000L;
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

int main(int argc, char *argv[]) {

    void term_handler(int signum) {
    (void)signum;
    // Serve solo per interrompere le system call bloccanti impostando errno = EINTR
    }

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <id_stazione>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int station_id = atoi(argv[1]);
    if (station_id < 0 || station_id >= STAT_CASSA) {
        fprintf(stderr, "[OPERATORE] ID stazione non valido: %d\n", station_id);
        exit(EXIT_FAILURE);
    }

    srand(getpid() ^ time(NULL));

    // 1. Aggancio Memoria Condivisa
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) { perror("[OPERATORE] shmget"); exit(EXIT_FAILURE); }
    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) { perror("[OPERATORE] shmat"); exit(EXIT_FAILURE); }

    // 2. Recupero Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) { perror("[OPERATORE] semget"); shmdt(shm_ptr); exit(EXIT_FAILURE); }

    // 3. Recupero Coda Messaggi della specifica stazione
    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + station_id);
    int msg_q_id = msgget(msg_key, 0600);
    if (msg_q_id == -1) { perror("[OPERATORE] msgget"); shmdt(shm_ptr); exit(EXIT_FAILURE); }

    // Mappa indice semaforo postazione
    int sem_postazione = SEM_POSTAZIONI_PRIMI + station_id;

    // 4. Barriera avvio sincrono
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Occupazione postazione fisica
    if (sem_p(sem_id, sem_postazione) == -1) {
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // Registra operatore attivo nelle statistiche
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->operatori_attivi_oggi++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    printf("[OPERATORE %d] In servizio alla stazione %d\n", getpid(), station_id);

    // 6. Ciclo di servizio richieste
    OrderMessage msg;
    while (shm_ptr->simulazione_attiva) {
        ssize_t res = msgrcv(msg_q_id, &msg, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (res == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        int piatto_disponibile = 0;
        int scelta = msg.dish_choice; // 0 o 1

        sem_p(sem_id, SEM_MUTEX_SHM);
        if (station_id == STAT_PRIMI) {
            if (shm_ptr->porzioni_primi[scelta] > 0) {
                shm_ptr->porzioni_primi[scelta]--;
                shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI]++;
                shm_ptr->piatti_distribuiti_tot[STAT_PRIMI]++;
                piatto_disponibile = 1;
            }
        } else if (station_id == STAT_SECONDI) {
            if (shm_ptr->porzioni_secondi[scelta] > 0) {
                shm_ptr->porzioni_secondi[scelta]--;
                shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI]++;
                shm_ptr->piatti_distribuiti_tot[STAT_SECONDI]++;
                piatto_disponibile = 1;
            }
        } else if (station_id == STAT_COFFEE) {
            // Illimitato da specifica
            shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE]++;
            shm_ptr->piatti_distribuiti_tot[STAT_COFFEE]++;
            piatto_disponibile = 1;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Simulazione erogazione cibo
        if (piatto_disponibile) {
            int variazione = (station_id == STAT_COFFEE) ? 80 : 50;
            simula_servizio(2, variazione, 5000000L); // 2 sec medi simulati
            msg.status = 0; // Successo
        } else {
            msg.status = -1; // Piatto esaurito
        }

        // Risposta mirata al PID dell'utente richiedente
        msg.mtype = msg.sender_pid;
        msgsnd(msg_q_id, &msg, sizeof(OrderMessage) - sizeof(long), 0);
    }

    // Rilascio postazione all'uscita
    sem_v(sem_id, sem_postazione);
    shmdt(shm_ptr);
    printf("[OPERATORE %d] Chiusura servizio.\n", getpid());
    return 0;
}