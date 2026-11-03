#include "common.h"
#include "ipc_utils.h"

static void sigterm_handler(int signum) {
    (void)signum;
    _exit(EXIT_SUCCESS);
}

static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;
    attendi_nanosecondi(totale_nanosecs);
}

static int ordina_a_stazione(int msg_qid, int piatto_scelto, int categoria,
                             pid_t my_pid, StationType staz, int giorno_attuale,
                             SharedData *shm_ptr, int sem_id) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;
    msg.dish_category = categoria;
    msg.giorno = giorno_attuale;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    if (msgsnd(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
        return -2;
    }

    if (msgrcv(msg_qid, &msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
        return -2;
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    long elapsed_ns = (t_end.tv_sec - t_start.tv_sec) * 1000000000L + (t_end.tv_nsec - t_start.tv_nsec);

    // Registra i tempi solo se il servizio è stato effettivamente erogato
    if (msg.status == 0) {
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->tempo_attesa_oggi[staz] += elapsed_ns;
        shm_ptr->ordini_completati_oggi[staz]++;
        shm_ptr->tempo_attesa_totale[staz] += elapsed_ns;
        shm_ptr->ordini_completati[staz]++;
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    return msg.status;
}

static void registra_esito_utente(SharedData *shm_ptr, int sem_id, int servito, int per_rinuncia) {
    sem_p(sem_id, SEM_MUTEX_SHM);
    if (servito) {
        shm_ptr->utenti_serviti_oggi++;
        shm_ptr->utenti_serviti_tot++;
    } else {
        shm_ptr->utenti_non_serviti_oggi++;
        shm_ptr->utenti_non_serviti_tot++;
        if (per_rinuncia) {
            shm_ptr->utenti_non_serviti_rinuncia_oggi++;
            shm_ptr->utenti_non_serviti_rinuncia_tot++;
        } else {
            shm_ptr->utenti_non_serviti_tempo_oggi++;
            shm_ptr->utenti_non_serviti_tempo_tot++;
        }
    }
    shm_ptr->utenti_finito_giornata++;
    sem_v(sem_id, SEM_MUTEX_SHM);
}

static void notifica_rinuncia_gruppo(SharedData *shm_ptr, int sem_id, int group_id) {
    if (group_id < 0 || group_id >= MAX_GROUPS) return;
    sem_p(sem_id, SEM_MUTEX_SHM);
    if (shm_ptr->gruppi[group_id].active_today > 0) {
        shm_ptr->gruppi[group_id].active_today--;
    }
    int pronti = shm_ptr->gruppi[group_id].ready_count;
    int attivi = shm_ptr->gruppi[group_id].active_today;
    sem_v(sem_id, SEM_MUTEX_SHM);

    if (pronti >= attivi && pronti > 0) {
        sem_add(sem_id, SEM_GRUPPI_BASE + group_id, pronti);
    }
}

int main(int argc, char *argv[]) {
    int group_id = -1;
    int is_dynamic = 0;

    if (argc > 1) group_id = atoi(argv[1]);
    if (argc > 2) is_dynamic = atoi(argv[2]);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    srand((unsigned int)(getpid() ^ time(NULL)));
    pid_t my_pid = getpid();

    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) { perror("[UTENTE] shmget"); exit(EXIT_FAILURE); }
    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) { perror("[UTENTE] shmat"); exit(EXIT_FAILURE); }

    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) { perror("[UTENTE] semget"); shmdt(shm_ptr); exit(EXIT_FAILURE); }

    int msg_q[NUM_STATIONS];
    for (int i = 0; i < NUM_STATIONS; i++) {
        key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + i);
        msg_q[i] = msgget(msg_key, 0600);
        if (msg_q[i] == -1) { perror("[UTENTE] msgget"); shmdt(shm_ptr); exit(EXIT_FAILURE); }
    }

    if (!is_dynamic) {
        sem_p(sem_id, SEM_BARRIERA_START);
        sem_wait_zero(sem_id, SEM_BARRIERA_START);
    }

    int has_ticket = ((rand() % 100) < 80) ? 1 : 0;

    while (shm_ptr->simulazione_attiva) {
        if (sem_p(sem_id, SEM_INIZIO_GIORNATA) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        if (!shm_ptr->simulazione_attiva) break;

        sem_p(sem_id, SEM_MUTEX_SHM);
        int giorno_attuale = shm_ptr->giorno_corrente;
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (has_ticket) {
            if (sem_p(sem_id, SEM_LETTORE_TICKET) != -1) {
                attendi_nanosecondi(shm_ptr->conf.n_nano_secs / 2);
                sem_v(sem_id, SEM_LETTORE_TICKET);
            }
        }

        int ha_primo = 0;
        int ha_secondo = 0;
        int ha_coffee = 0;
        int num_portate = 0;

        int n_primi = shm_ptr->menu.num_primi > 0 ? shm_ptr->menu.num_primi : 1;
        int scelta_p = rand() % n_primi;
        int esito_p = -1;

        for (int t = 0; t < n_primi; t++) {
            int piatto = (scelta_p + t) % n_primi;
            esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], piatto, 0, my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
            if (esito_p == 0) { ha_primo = 1; num_portate++; break; }
            else if (esito_p == -2) break;
        }

        if (esito_p == -2) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0, 0);
            continue;
        }

        int n_secondi = shm_ptr->menu.num_secondi > 0 ? shm_ptr->menu.num_secondi : 1;
        int scelta_s = rand() % n_secondi;
        int esito_s = -1;

        for (int t = 0; t < n_secondi; t++) {
            int piatto = (scelta_s + t) % n_secondi;
            esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], piatto, 1, my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
            if (esito_s == 0) { ha_secondo = 1; num_portate++; break; }
            else if (esito_s == -2) break;
        }

        if (esito_s == -2) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0, 0);
            continue;
        }

        // Se entrambi risultano momentaneamente esauriti, attende il refill ed effettua un secondo tentativo
        if (!ha_primo && !ha_secondo) {
            attendi_nanosecondi(5 * shm_ptr->conf.n_nano_secs); // Attesa di 5 minuti simulati
            
            for (int t = 0; t < n_primi; t++) {
                int piatto = (scelta_p + t) % n_primi;
                esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], piatto, 0, my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
                if (esito_p == 0) { ha_primo = 1; num_portate++; break; }
            }
            for (int t = 0; t < n_secondi; t++) {
                int piatto = (scelta_s + t) % n_secondi;
                esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], piatto, 1, my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
                if (esito_s == 0) { ha_secondo = 1; num_portate++; break; }
            }

            if (!ha_primo && !ha_secondo) {
                notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
                registra_esito_utente(shm_ptr, sem_id, 0, 1);
                continue;
            }
        }

        // 3. Stazione Caffè e Dolce (solo se il giorno non è già scaduto)
        sem_p(sem_id, SEM_MUTEX_SHM);
        int in_tempo_caffe = (shm_ptr->giorno_corrente == giorno_attuale);
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (in_tempo_caffe && (rand() % 2 == 1)) {
            int n_caffe = shm_ptr->menu.num_caffe > 0 ? shm_ptr->menu.num_caffe : 1;
            int scelta_c = rand() % n_caffe;
            if (ordina_a_stazione(msg_q[STAT_COFFEE], scelta_c, 2, my_pid, STAT_COFFEE, giorno_attuale, shm_ptr, sem_id) == 0) {
                ha_coffee = 1;
                num_portate++;
            }
        }

        // 4. Barriera dei Gruppi passiva su semaforo
        if (group_id >= 0 && group_id < MAX_GROUPS) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->gruppi[group_id].ready_count++;
            int pronti = shm_ptr->gruppi[group_id].ready_count;
            int attivi = shm_ptr->gruppi[group_id].active_today;
            sem_v(sem_id, SEM_MUTEX_SHM);

            if (pronti < attivi) {
                sem_p(sem_id, SEM_GRUPPI_BASE + group_id);
            } else if (attivi > 1) {
                sem_add(sem_id, SEM_GRUPPI_BASE + group_id, attivi - 1);
            }
        }

        // 5. Pagamento alla Cassa (chi ha i piatti nel vassoio paga sempre prima di uscire)
        OrderMessage cassa_msg;
        memset(&cassa_msg, 0, sizeof(cassa_msg));
        cassa_msg.mtype = 1;
        cassa_msg.sender_pid = my_pid;
        cassa_msg.giorno = giorno_attuale;
        cassa_msg.ha_primo = ha_primo;
        cassa_msg.ha_secondo = ha_secondo;
        cassa_msg.ha_coffee = ha_coffee;
        cassa_msg.has_ticket = has_ticket;

        struct timespec t_cassa_start, t_cassa_end;
        clock_gettime(CLOCK_MONOTONIC, &t_cassa_start);

        if (msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0) == -1 ||
            msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
            registra_esito_utente(shm_ptr, sem_id, 0, 0);
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t_cassa_end);
        long elapsed_cassa_ns = (t_cassa_end.tv_sec - t_cassa_start.tv_sec) * 1000000000L +
                                (t_cassa_end.tv_nsec - t_cassa_start.tv_nsec);

        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->tempo_attesa_oggi[STAT_CASSA] += elapsed_cassa_ns;
        shm_ptr->ordini_completati_oggi[STAT_CASSA]++;
        shm_ptr->tempo_attesa_totale[STAT_CASSA] += elapsed_cassa_ns;
        shm_ptr->ordini_completati[STAT_CASSA]++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // 6. Tavolo e Refezione
        if (sem_p(sem_id, SEM_TAVOLI) != -1) {
            simula_consumo_pasto(num_portate, shm_ptr->conf.n_nano_secs);
            sem_v(sem_id, SEM_TAVOLI);
        }

        // L'utente ha ritirato i piatti, pagato in cassa e consumato al tavolo -> SERVITO!
        registra_esito_utente(shm_ptr, sem_id, 1, 0);
    }

    shmdt(shm_ptr);
    return 0;
}