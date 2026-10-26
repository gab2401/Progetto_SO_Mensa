#include "common.h"
#include "config.h"
#include "ipc_utils.h"

static int shm_id = -1;
static SharedData *shm_ptr = NULL;
static int sem_id = -1;
static int msg_queue_id[NUM_STATIONS] = {-1, -1, -1, -1};

static pid_t *child_pids = NULL;
static int num_children_spawned = 0;

void cleanup_resources(void) {
    for (int i = 0; i < num_children_spawned; i++) {
        if (child_pids != NULL && child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
            waitpid(child_pids[i], NULL, 0);
        }
    }
    if (child_pids != NULL) {
        free(child_pids);
        child_pids = NULL;
    }

    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        shmdt(shm_ptr);
        shm_ptr = NULL;
    }
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id = -1;
    }

    if (sem_id != -1) {
        semctl(sem_id, 0, IPC_RMID);
        sem_id = -1;
    }

    for (int i = 0; i < NUM_STATIONS; i++) {
        if (msg_queue_id[i] != -1) {
            msgctl(msg_queue_id[i], IPC_RMID, NULL);
            msg_queue_id[i] = -1;
        }
    }
    printf("\n[CLEANUP] Tutti i processi e le risorse IPC sono stati rimossi.\n");
}

void sig_handler(int signum) {
    (void)signum;
    cleanup_resources();
    _exit(EXIT_SUCCESS);
}

pid_t avvia_processo(const char *bin_path, char *const args[]) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("Errore fork");
        return -1;
    }
    if (pid == 0) {
        execve(bin_path, args, NULL);
        perror("Errore execve");
        _exit(EXIT_FAILURE);
    }
    return pid;
}

// Sblocca con status = -2 eventuali richieste ancora in coda a fine giornata
static void respingi_coda_fine_giornata(int g_concluso) {
    OrderMessage msg;
    for (int s = 0; s < NUM_STATIONS; s++) {
        while (msgrcv(msg_queue_id[s], &msg, sizeof(OrderMessage) - sizeof(long), 1, IPC_NOWAIT) != -1) {
            if (msg.giorno <= g_concluso) {
                msg.mtype = msg.sender_pid;
                msg.status = -2;
                msgsnd(msg_queue_id[s], &msg, sizeof(OrderMessage) - sizeof(long), 0);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_configurazione>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    Config conf;
    if (parse_config(argv[1], &conf) == -1) {
        exit(EXIT_FAILURE);
    }

    printf("Configurazione caricata con successo:\n");
    printf("- Durata simulazione: %d giorni\n", conf.sim_duration);
    printf("- Lavoratori: %d, Utenti: %d\n", conf.nof_workers, conf.nof_users);
    printf("- Prezzo primi: %.2f EUR, secondi: %.2f EUR, caffe': %.2f EUR\n",
           conf.price_primi, conf.price_secondi, conf.price_coffee);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Setup risorse IPC (una sola chiamata per ciascuna risorsa!)
    shm_id = init_shared_memory(&conf, &shm_ptr);
    if (shm_id == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    sem_id = init_semaphores(&conf);
    if (sem_id == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    if (init_message_queues(msg_queue_id) == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    if (parse_menu("config/menu.txt", shm_ptr) == -1) {
        fprintf(stderr, "Avviso: impossibile caricare config/menu.txt\n");
    }

    int totale_figli = conf.nof_workers + conf.nof_users;
    child_pids = malloc(sizeof(pid_t) * totale_figli);
    if (child_pids == NULL) {
        perror("Errore malloc child_pids");
        cleanup_resources();
        exit(EXIT_FAILURE);
    }

    // 1. Avvio Cassiere
    char *cassiere_args[] = {"cassiere", NULL};
    pid_t pid_cassa = avvia_processo("./bin/cassiere", cassiere_args);
    if (pid_cassa == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
    child_pids[num_children_spawned++] = pid_cassa;

    // 2. Avvio Operatori
    int operatori_da_creare = conf.nof_workers - 1;
    int op_assegnati[3] = {0, 0, 0};

    for (int i = 0; i < operatori_da_creare; i++) {
        int staz_id = 0;
        if (i < 3) {
            staz_id = i;
        } else {
            double max_ratio = -1.0;
            for (int s = 0; s < 3; s++) {
                int div = op_assegnati[s] == 0 ? 1 : op_assegnati[s];
                double ratio = (double)conf.avg_srvc[s] / (double)div;
                if (ratio > max_ratio) {
                    max_ratio = ratio;
                    staz_id = s;
                }
            }
        }
        op_assegnati[staz_id]++;

        char staz_arg[16];
        snprintf(staz_arg, sizeof(staz_arg), "%d", staz_id);
        char *operatore_args[] = {"operatore", staz_arg, NULL};

        pid_t pid_op = avvia_processo("./bin/operatore", operatore_args);
        if (pid_op == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
        child_pids[num_children_spawned++] = pid_op;
    }

    // 3. Avvio Utenti
    char *utente_args[] = {"utente", NULL};
    for (int i = 0; i < conf.nof_users; i++) {
        pid_t pid_ut = avvia_processo("./bin/utente", utente_args);
        if (pid_ut == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
        child_pids[num_children_spawned++] = pid_ut;
    }

    printf("[RESPONSABILE] Creati %d processi figli. Attesa sincronizzazione alla barriera...\n",
           num_children_spawned);

    sem_wait_zero(sem_id, SEM_BARRIERA_START);
    printf("[RESPONSABILE] Tutti i processi pronti. AVVIO SIMULAZIONE!\n");

    const int MINUTI_PER_GIORNO = 60;
    int giorni_effettivi = 0;
    const char *nomi_stazioni[] = {"Primi", "Secondi", "Coffee", "Cassa"};

    for (int g = 1; g <= conf.sim_duration && shm_ptr->simulazione_attiva; g++) {
        giorni_effettivi = g;

        // Preparazione nuovo giorno
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g;
        shm_ptr->minuto_corrente = 0;
        shm_ptr->utenti_serviti_oggi = 0;
        shm_ptr->utenti_non_serviti_oggi = 0;
        shm_ptr->utenti_finito_giornata = 0;
        shm_ptr->ricavo_oggi = 0.0;
        shm_ptr->pause_effettuate_oggi = 0;
        for (int s = 0; s < NUM_STATIONS; s++) {
            shm_ptr->piatti_distribuiti_oggi[s] = 0;
            shm_ptr->tempo_attesa_oggi[s] = 0;
            shm_ptr->ordini_completati_oggi[s] = 0;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);

        printf("\n=========================================\n");
        printf("         INIZIO GIORNATA %d              \n", g);
        printf("=========================================\n");

        // Sblocco passivo di tutti gli utenti per la giornata corrente
        sem_add(sem_id, SEM_INIZIO_GIORNATA, conf.nof_users);

        for (int m = 1; m <= MINUTI_PER_GIORNO && shm_ptr->simulazione_attiva; m++) {
            attendi_nanosecondi(conf.n_nano_secs);

            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->minuto_corrente = m;

            if (m % 10 == 0) {
                for (int p = 0; p < 2; p++) {
                    if (shm_ptr->porzioni_primi[p] + conf.avg_refill_primi <= conf.max_porzioni_primi)
                        shm_ptr->porzioni_primi[p] += conf.avg_refill_primi;
                    else
                        shm_ptr->porzioni_primi[p] = conf.max_porzioni_primi;

                    if (shm_ptr->porzioni_secondi[p] + conf.avg_refill_secondi <= conf.max_porzioni_secondi)
                        shm_ptr->porzioni_secondi[p] += conf.avg_refill_secondi;
                    else
                        shm_ptr->porzioni_secondi[p] = conf.max_porzioni_secondi;
                }
                printf("[REFILL] Minuto %d: Ricarica porzioni effettuata.\n", m);
            }
            sem_v(sem_id, SEM_MUTEX_SHM);
        }

        // Fine giornata: incrementa giorno_corrente così gli operatori/cassiere respingono i ritardatari
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g + 1;
        int avanzati_oggi = shm_ptr->porzioni_primi[0] + shm_ptr->porzioni_primi[1] +
                            shm_ptr->porzioni_secondi[0] + shm_ptr->porzioni_secondi[1];
        shm_ptr->piatti_avanzati_oggi = avanzati_oggi;
        shm_ptr->piatti_avanzati_tot += avanzati_oggi;
        sem_v(sem_id, SEM_MUTEX_SHM);

        // Sblocca eventuali utenti ancora fermi nelle code a fine giornata
        respingi_coda_fine_giornata(g);
        attendi_nanosecondi(2 * conf.n_nano_secs);
        respingi_coda_fine_giornata(g);

        // Stampa Report Giornaliero (SOLA LETTURA delle statistiche aggiornate dai figli)
        sem_p(sem_id, SEM_MUTEX_SHM);
        int non_serviti_oggi = shm_ptr->utenti_non_serviti_oggi;
        printf("\n--- Report Fine Giorno %d ---\n", g);
        printf("Utenti serviti oggi: %d\n", shm_ptr->utenti_serviti_oggi);
        printf("Utenti non serviti oggi: %d\n", non_serviti_oggi);
        printf("Piatti distribuiti oggi: Primi=%d, Secondi=%d, Coffee=%d (Tot=%d)\n",
               shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI],
               shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI],
               shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE],
               shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI] +
               shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI] +
               shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE]);
        printf("Piatti avanzati a fine giornata: %d (Primi: [%d, %d], Secondi: [%d, %d])\n",
               shm_ptr->piatti_avanzati_oggi,
               shm_ptr->porzioni_primi[0], shm_ptr->porzioni_primi[1],
               shm_ptr->porzioni_secondi[0], shm_ptr->porzioni_secondi[1]);
        printf("Operatori attivi oggi: %d | Pause effettuate oggi: %d\n",
               shm_ptr->operatori_attivi_oggi, shm_ptr->pause_effettuate_oggi);
        printf("Incasso giornaliero: %.2f EUR\n", shm_ptr->ricavo_oggi);

        for (int s = 0; s < NUM_STATIONS; s++) {
            if (shm_ptr->ordini_completati_oggi[s] > 0) {
                double att_ms = (double)shm_ptr->tempo_attesa_oggi[s] /
                                (shm_ptr->ordini_completati_oggi[s] * 1000000.0);
                printf("  Attesa media oggi %s: %.2f ms\n", nomi_stazioni[s], att_ms);
            }
        }

        if (non_serviti_oggi > conf.overload_threshold) {
            printf("\n[ATTENZIONE] Rilevato OVERLOAD: %d utenti non serviti superano la soglia (%d)!\n",
                   non_serviti_oggi, conf.overload_threshold);
            shm_ptr->motivo_terminazione = 1;
            shm_ptr->simulazione_attiva = 0;
            sem_v(sem_id, SEM_MUTEX_SHM);
            break;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    // Fine simulazione e Report Finale Completo
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->simulazione_attiva = 0;

    if (giorni_effettivi <= 0) giorni_effettivi = 1;

    printf("\n=========================================\n");
    printf("         FINE SIMULAZIONE                \n");
    if (shm_ptr->motivo_terminazione == 1) {
        printf("Causa terminazione: OVERLOAD (soglia superata)\n");
    } else {
        printf("Causa terminazione: TIMEOUT (raggiunti %d giorni)\n", conf.sim_duration);
    }
    printf("Giorni simulati: %d\n", giorni_effettivi);
    printf("Utenti serviti: Totale = %d | Media/giorno = %.2f\n",
           shm_ptr->utenti_serviti_tot, (double)shm_ptr->utenti_serviti_tot / giorni_effettivi);
    printf("Utenti non serviti: Totale = %d | Media/giorno = %.2f\n",
           shm_ptr->utenti_non_serviti_tot, (double)shm_ptr->utenti_non_serviti_tot / giorni_effettivi);

    int tot_piatti_dist = shm_ptr->piatti_distribuiti_tot[STAT_PRIMI] +
                          shm_ptr->piatti_distribuiti_tot[STAT_SECONDI] +
                          shm_ptr->piatti_distribuiti_tot[STAT_COFFEE];
    printf("Piatti distribuiti: Totale = %d (Primi=%d, Secondi=%d, Coffee=%d) | Media/giorno = %.2f\n",
           tot_piatti_dist,
           shm_ptr->piatti_distribuiti_tot[STAT_PRIMI],
           shm_ptr->piatti_distribuiti_tot[STAT_SECONDI],
           shm_ptr->piatti_distribuiti_tot[STAT_COFFEE],
           (double)tot_piatti_dist / giorni_effettivi);
    printf("Piatti avanzati: Totale = %d | Media/giorno = %.2f\n",
           shm_ptr->piatti_avanzati_tot, (double)shm_ptr->piatti_avanzati_tot / giorni_effettivi);
    printf("Pause effettuate: Totale = %d | Media/giorno = %.2f\n",
           shm_ptr->pause_totali, (double)shm_ptr->pause_totali / giorni_effettivi);
    printf("Incasso complessivo: %.2f EUR | Media/giorno = %.2f EUR\n",
           shm_ptr->ricavo_totale, shm_ptr->ricavo_totale / giorni_effettivi);

    printf("\n--- Tempi Medi di Attesa Complessivi ---\n");
    for (int s = 0; s < NUM_STATIONS; s++) {
        if (shm_ptr->ordini_completati[s] > 0) {
            double attesa_ms = (double)shm_ptr->tempo_attesa_totale[s] /
                               (shm_ptr->ordini_completati[s] * 1000000.0);
            printf("Stazione %s: %.2f ms (su %d ordini)\n",
                   nomi_stazioni[s], attesa_ms, shm_ptr->ordini_completati[s]);
        } else {
            printf("Stazione %s: N/D (0 ordini)\n", nomi_stazioni[s]);
        }
    }
    printf("=========================================\n");
    sem_v(sem_id, SEM_MUTEX_SHM);

    cleanup_resources();
    return 0;
}