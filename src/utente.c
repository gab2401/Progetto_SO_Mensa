#include "common.h"
#include "ipc_utils.h"

static void sigterm_handler(int signum) {
    (void)signum;
    _exit(EXIT_SUCCESS);
}

// Simula la consumazione del pasto al tavolo tramite attesa passiva parametrata su N_NANO_SECS
static void simula_consumo_pasto(int num_piatti, long n_nano_secs) {
    if (num_piatti <= 0) return;
    long totale_nanosecs = (long)num_piatti * 2 * n_nano_secs;
    attendi_nanosecondi(totale_nanosecs);
}

// Inoltra un ordine a una stazione, cronometra l'attesa e registra le metriche in SHM
static int ordina_a_stazione(int msg_qid, int piatto_scelto, pid_t my_pid,
                             StationType staz, int giorno_attuale,
                             SharedData *shm_ptr, int sem_id) {
    OrderMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.mtype = 1;
    msg.sender_pid = my_pid;
    msg.dish_choice = piatto_scelto;
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

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->tempo_attesa_oggi[staz] += elapsed_ns;
    shm_ptr->ordini_completati_oggi[staz]++;
    shm_ptr->tempo_attesa_totale[staz] += elapsed_ns;
    shm_ptr->ordini_completati[staz]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    return msg.status; // 0 successo, -1 esaurito, -2 giorno scaduto
}

// Notifica esito finale per la giornata, garantendo coerenza numerica esatta
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

// Sblocca i compagni di gruppo nel caso un membro debba rinunciare
static void notifica_rinuncia_gruppo(SharedData *shm_ptr, int sem_id, int group_id) {
    if (group_id < 0 || group_id >= MAX_GROUPS) return;
    sem_p(sem_id, SEM_MUTEX_SHM);
    if (shm_ptr->gruppi[group_id].active_today > 0) {
        shm_ptr->gruppi[group_id].active_today--;
    }
    if (shm_ptr->gruppi[group_id].ready_count >= shm_ptr->gruppi[group_id].active_today) {
        shm_ptr->gruppi[group_id].barrier_flag = 1;
    }
    sem_v(sem_id, SEM_MUTEX_SHM);
}

int main(int argc, char *argv[]) {
    int group_id = -1;
    int is_dynamic = 0;

    if (argc > 1) {
        group_id = atoi(argv[1]);
    }
    if (argc > 2) {
        is_dynamic = atoi(argv[2]);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);

    srand(getpid() ^ (unsigned int)time(NULL));
    pid_t my_pid = getpid();

    // 1. Connessione a SHM
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if (shm_key == -1) {
        perror("[UTENTE] ftok SHM");
        exit(EXIT_FAILURE);
    }
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

    // 2. Connessione al set di semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[UTENTE] semget");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // 3. Connessione alle code di messaggi
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

    // 4. Barriera iniziale: solo per processi creati al boot (non dinamici)
    if (!is_dynamic) {
        sem_p(sem_id, SEM_BARRIERA_START);
        sem_wait_zero(sem_id, SEM_BARRIERA_START);
    }

    // Profilo utente: 80% possiede ticket buono pasto
    int has_ticket = ((rand() % 100) < 80) ? 1 : 0;

    // 5. Ciclo principale: sincronizzazione passiva mediante semaforo
    while (shm_ptr->simulazione_attiva) {
        if (sem_p(sem_id, SEM_INIZIO_GIORNATA) == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        if (!shm_ptr->simulazione_attiva) break;

        sem_p(sem_id, SEM_MUTEX_SHM);
        int giorno_attuale = shm_ptr->giorno_corrente;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Fila preliminare per la validazione del ticket
        if (has_ticket) {
            if (sem_p(sem_id, SEM_LETTORE_TICKET) != -1) {
                attendi_nanosecondi(shm_ptr->conf.n_nano_secs / 2);
                sem_v(sem_id, SEM_LETTORE_TICKET);
            }
        }

        int ha_primo = 0;
        int ha_secondo = 0;
        double conto_totale = 0.0;
        int num_portate = 0;

        // A. Stazione Primi
        int n_primi = shm_ptr->menu.num_primi > 0 ? shm_ptr->menu.num_primi : 2;
        int scelta_p = rand() % n_primi;

        int esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], scelta_p % MAX_DISHES_PER_TYPE,
                                         my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
        if (esito_p == -2) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        } else if (esito_p == 0) {
            ha_primo = 1;
        } else {
            // Prova la seconda scelta se il piatto era esaurito
            esito_p = ordina_a_stazione(msg_q[STAT_PRIMI], (1 - (scelta_p % 2)),
                                         my_pid, STAT_PRIMI, giorno_attuale, shm_ptr, sem_id);
            if (esito_p == -2) {
                notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
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

        // B. Stazione Secondi
        int n_secondi = shm_ptr->menu.num_secondi > 0 ? shm_ptr->menu.num_secondi : 2;
        int scelta_s = rand() % n_secondi;

        int esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], scelta_s % MAX_DISHES_PER_TYPE,
                                         my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
        if (esito_s == -2) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        } else if (esito_s == 0) {
            ha_secondo = 1;
        } else {
            esito_s = ordina_a_stazione(msg_q[STAT_SECONDI], (1 - (scelta_s % 2)),
                                         my_pid, STAT_SECONDI, giorno_attuale, shm_ptr, sem_id);
            if (esito_s == -2) {
                notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
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

        // Se entrambi i piatti risultano esauriti, rinuncia al servizio
        if (!ha_primo && !ha_secondo) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // C. Stazione Caffè (50% probabilità)
        if (rand() % 2 == 1) {
            int n_caffe = shm_ptr->menu.num_caffe > 0 ? shm_ptr->menu.num_caffe : 4;
            int scelta_c = rand() % n_caffe;
            int esito_c = ordina_a_stazione(msg_q[STAT_COFFEE], scelta_c % MAX_DISHES_PER_TYPE,
                                             my_pid, STAT_COFFEE, giorno_attuale, shm_ptr, sem_id);
            if (esito_c == -2) {
                notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
                registra_esito_utente(shm_ptr, sem_id, 0);
                continue;
            } else if (esito_c == 0) {
                conto_totale += shm_ptr->conf.price_coffee;
                num_portate++;
            }
        }

        // Verifica validità prima della fase cassa
        sem_p(sem_id, SEM_MUTEX_SHM);
        int ancora_in_tempo = (shm_ptr->giorno_corrente == giorno_attuale);
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (!ancora_in_tempo) {
            notifica_rinuncia_gruppo(shm_ptr, sem_id, group_id);
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // D. Sincronizzazione Gruppo: attesa che tutti i compagni abbiano ritirato i pasti
        if (group_id >= 0 && group_id < MAX_GROUPS) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->gruppi[group_id].ready_count++;
            if (shm_ptr->gruppi[group_id].ready_count >= shm_ptr->gruppi[group_id].active_today) {
                shm_ptr->gruppi[group_id].barrier_flag = 1;
            }
            sem_v(sem_id, SEM_MUTEX_SHM);

            while (shm_ptr->simulazione_attiva) {
                sem_p(sem_id, SEM_MUTEX_SHM);
                int gruppo_pronto = shm_ptr->gruppi[group_id].barrier_flag;
                int scaduto = (shm_ptr->giorno_corrente != giorno_attuale);
                sem_v(sem_id, SEM_MUTEX_SHM);

                if (gruppo_pronto || scaduto) break;
                attendi_nanosecondi(shm_ptr->conf.n_nano_secs / 2);
            }
        }

        sem_p(sem_id, SEM_MUTEX_SHM);
        ancora_in_tempo = (shm_ptr->giorno_corrente == giorno_attuale);
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (!ancora_in_tempo) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // E. Pagamento alla Cassa
        OrderMessage cassa_msg;
        memset(&cassa_msg, 0, sizeof(cassa_msg));
        cassa_msg.mtype = 1;
        cassa_msg.sender_pid = my_pid;
        cassa_msg.importo = conto_totale;
        cassa_msg.giorno = giorno_attuale;
        cassa_msg.has_ticket = has_ticket;

        struct timespec t_cassa_start, t_cassa_end;
        clock_gettime(CLOCK_MONOTONIC, &t_cassa_start);

        if (msgsnd(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), 0) == -1) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }
        if (msgrcv(msg_q[STAT_CASSA], &cassa_msg, sizeof(OrderMessage) - sizeof(long), my_pid, 0) == -1) {
            registra_esito_utente(shm_ptr, sem_id, 0);
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

        if (cassa_msg.status == -2) {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // F. Consumazione al Tavolo
        if (sem_p(sem_id, SEM_TAVOLI) != -1) {
            simula_consumo_pasto(num_portate, shm_ptr->conf.n_nano_secs);
            sem_v(sem_id, SEM_TAVOLI);
        } else {
            registra_esito_utente(shm_ptr, sem_id, 0);
            continue;
        }

        // G. Verifica finale dell'esito
        sem_p(sem_id, SEM_MUTEX_SHM);
        int completato_in_giorno = (shm_ptr->giorno_corrente == giorno_attuale);
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (completato_in_giorno) {
            registra_esito_utente(shm_ptr, sem_id, 1);
        } else {
            registra_esito_utente(shm_ptr, sem_id, 0);
        }
    }

    shmdt(shm_ptr);
    return 0;
}