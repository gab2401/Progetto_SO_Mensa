#include "common.h"
#include "config.h"
#include "ipc_utils.h"

// Variabili globali per la gestione della pulizia e dei segnali
static int shm_id = -1;
static SharedData *shm_ptr = NULL;
static int sem_id = -1;
static int msg_queue_id[NUM_STATIONS] = {-1, -1, -1, -1};

// Tracciamento dei processi figli per la terminazione controllata
static pid_t *child_pids = NULL;
static int num_children_spawned = 0;    

// Deallocazione completa di processi e risorse IPC
void cleanup_resources(void) {
    // 1. Invio SIGTERM a tutti i processi figli e attesa terminazione
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

    // 2. Sgancio e rimozione Memoria Condivisa
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        shmdt(shm_ptr);
        shm_ptr = NULL;
    }
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id = -1;
    }

    // 3. Rimozione Set di Semafori
    if (sem_id != -1) {
        semctl(sem_id, 0, IPC_RMID);
        sem_id = -1;
    }

    // 4. Rimozione Code di Messaggi
    for (int i = 0; i < NUM_STATIONS; i++) {
        if (msg_queue_id[i] != -1) {
            msgctl(msg_queue_id[i], IPC_RMID, NULL);
            msg_queue_id[i] = -1;
        }
    }
    printf("\n[CLEANUP] Tutti i processi e le risorse IPC sono stati rimossi.\n");
}

// Handler per SIGINT (Ctrl+C) e SIGTERM
void sig_handler(int signum) {
    (void)signum;
    cleanup_resources();
    _exit(EXIT_SUCCESS);
}

// Helper per fork ed execve
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
// Scrittura statistiche su file CSV
static void esporta_statistiche_csv(const char *filepath, const SharedData *shm,
                                   int giorni_svolti, const char *causa) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        perror("[RESPONSABILE] Errore apertura file CSV per statistiche");
        return;
    }

    fprintf(fp, "Metrica,Valore\n");
    fprintf(fp, "Causa_Terminazione,\"%s\"\n", causa);
    fprintf(fp, "Giorni_Simulati,%d\n", giorni_svolti);
    fprintf(fp, "Utenti_Serviti_Totali,%d\n", shm->utenti_serviti_tot);
    fprintf(fp, "Utenti_Non_Serviti_Totali,%d\n", shm->utenti_non_serviti_tot);
    fprintf(fp, "Media_Utenti_Serviti_Giorno,%.2f\n", (double)shm->utenti_serviti_tot / giorni_svolti);
    fprintf(fp, "Media_Utenti_Non_Serviti_Giorno,%.2f\n", (double)shm->utenti_non_serviti_tot / giorni_svolti);
    fprintf(fp, "Ricavo_Totale_EUR,%.2f\n", shm->ricavo_totale);
    fprintf(fp, "Media_Ricavo_Giorno_EUR,%.2f\n", shm->ricavo_totale / giorni_svolti);
    fprintf(fp, "Pause_Totali,%d\n", shm->pause_totali);
    fprintf(fp, "Media_Pause_Giorno,%.2f\n", (double)shm->pause_totali / giorni_svolti);
    fprintf(fp, "Piatti_Primi_Distribuiti,%d\n", shm->piatti_distribuiti_tot[STAT_PRIMI]);
    fprintf(fp, "Piatti_Secondi_Distribuiti,%d\n", shm->piatti_distribuiti_tot[STAT_SECONDI]);
    fprintf(fp, "Piatti_Coffee_Distribuiti,%d\n", shm->piatti_distribuiti_tot[STAT_COFFEE]);

    // Tempi di attesa complessivi
    for (int s = 0; s < NUM_STATIONS; s++) {
        double media = (shm->visite_stazione_tot[s] > 0) ?
            (shm->tempo_attesa_tot[s] / shm->visite_stazione_tot[s]) : 0.0;
        fprintf(fp, "Tempo_Medio_Attesa_Staz_%d,%.4f\n", s, media);
    }

    fclose(fp);
    printf("[RESPONSABILE] Statistiche esportate con successo in '%s'\n", filepath);
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

    // Registrazione gestore segnali
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Setup risorse IPC
    shm_id = init_shared_memory(&conf, &shm_ptr);
    if (shm_id == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    sem_id = init_semaphores(&conf);
    if (sem_id == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    if (init_message_queues(msg_queue_id) == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    // Caricamento del file di menu nella struct SHM
    if (carica_menu("config/menu.txt", &shm_ptr->menu) == -1) {
        fprintf(stderr, "[RESPONSABILE] Attenzione: impossibile leggere config/menu.txt\n");
    } else {
        printf("[RESPONSABILE] Menu caricato: %d primi, %d secondi, %d dolci, %d caffe'.\n",
               shm_ptr->menu.num_primi, shm_ptr->menu.num_secondi,
               shm_ptr->menu.num_dolci, shm_ptr->menu.num_caffe);
    }

    // Inizializzazione tempi cumulativi complessivi in SHM
    for (int s = 0; s < NUM_STATIONS; s++) {
        shm_ptr->tempo_attesa_tot[s] = 0.0;
        shm_ptr->visite_stazione_tot[s] = 0;
    }

    // Allocazione array dei PID
    int totale_figli = conf.nof_workers + conf.nof_users;
    child_pids = malloc(sizeof(pid_t) * totale_figli);
    if (child_pids == NULL) {
        perror("Errore malloc child_pids");
        cleanup_resources();
        exit(EXIT_FAILURE);
    }

    // -------------------------------------------------------------
    // AVVIO PROCESSI FIGLI (FORK + EXECVE)
    // -------------------------------------------------------------

    // 1. Avvio Cassiere
    char *cassiere_args[] = {"cassiere", NULL};
    pid_t pid_cassa = avvia_processo("./bin/cassiere", cassiere_args);
    if (pid_cassa == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
    child_pids[num_children_spawned++] = pid_cassa;

    // 2. Avvio Operatori (almeno 1 per stazione)
    int operatori_da_creare = conf.nof_workers - 1;
    for (int i = 0; i < operatori_da_creare; i++) {
        int staz_id;
        if (i == 0) staz_id = STAT_PRIMI;
        else if (i == 1) staz_id = STAT_SECONDI;
        else if (i == 2) staz_id = STAT_COFFEE;
        else staz_id = (i % 3);

        char staz_arg[16];
        snprintf(staz_arg, sizeof(staz_arg), "%d", staz_id);
        char *operatore_args[] = {"operatore", staz_arg, NULL};

        pid_t pid_op = avvia_processo("./bin/operatore", operatore_args);
        if (pid_op == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
        child_pids[num_children_spawned++] = pid_op;
    }


    // 3. Avvio Utenti suddivisi in gruppi (versione 30/30)
    srand((unsigned int)time(NULL));
    int utenti_rimasti = conf.nof_users;
    int cur_group_id = 0;
    shm_ptr->utenti_totali_generati = conf.nof_users;

    while (utenti_rimasti > 0 && cur_group_id < MAX_GROUPS) {
        int max_taglia = (conf.max_users_per_group > 0) ? conf.max_users_per_group : 1;
        int group_size = (rand() % max_taglia) + 1;
        if (group_size > utenti_rimasti) {
            group_size = utenti_rimasti;
        }

        shm_ptr->gruppi[cur_group_id].total_members = group_size;
        shm_ptr->gruppi[cur_group_id].ready_count = 0;
        shm_ptr->gruppi[cur_group_id].barrier_flag = 0;

        char group_arg[16];
        snprintf(group_arg, sizeof(group_arg), "%d", cur_group_id);

        for (int k = 0; k < group_size; k++) {
            char *utente_args[] = {"utente", group_arg, NULL};
            pid_t pid_ut = avvia_processo("./bin/utente", utente_args);
            if (pid_ut == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
            child_pids[num_children_spawned++] = pid_ut;
        }

        utenti_rimasti -= group_size;
        cur_group_id++;
    }
    shm_ptr->num_gruppi = cur_group_id;
    printf("[RESPONSABILE] Creati %d utenti suddivisi in %d gruppi.\n", conf.nof_users, cur_group_id);

    printf("[RESPONSABILE] Creati %d processi figli. Attesa sincronizzazione alla barriera...\n",
           num_children_spawned);

    // Attesa sincrona che tutti i figli abbiano completato il setup
    sem_wait_zero(sem_id, SEM_BARRIERA_START);
    printf("[RESPONSABILE] Tutti i processi pronti. AVVIO SIMULAZIONE!\n");

    // -------------------------------------------------------------
    // CICLO SIMULAZIONE (GIORNI E MINUTI)
    // -------------------------------------------------------------
    const int MINUTI_PER_GIORNO = 60;
    int giorni_effettivi_svolti = 0;
    const char *causa_terminazione = "TIMEOUT (Raggiunta durata massima SIM_DURATION)";
    const char *staz_nomi[NUM_STATIONS] = {"Primi", "Secondi", "Dolce-Caffe'", "Cassa"};

    for (int g = 1; g <= conf.sim_duration && shm_ptr->simulazione_attiva; g++) {
        giorni_effettivi_svolti = g;

        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g;
        shm_ptr->utenti_serviti_oggi = 0;
        shm_ptr->utenti_non_serviti_oggi = 0;
        shm_ptr->ricavo_oggi = 0.0;
        shm_ptr->pause_effettuate_oggi = 0;
        shm_ptr->operatori_attivi_oggi = 0;

        for (int s = 0; s < NUM_STATIONS; s++) {
            shm_ptr->piatti_distribuiti_oggi[s] = 0;
            shm_ptr->tempo_attesa_oggi[s] = 0.0;
            shm_ptr->visite_stazione_oggi[s] = 0;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);

        printf("\n=========================================\n");
        printf("         INIZIO GIORNATA %d              \n", g);
        printf("=========================================\n");

        // Calcolo primo intervallo refill: AVG_REFILL_TIME ± 20%
        int jitter = (conf.avg_refill_time * 20) / 100;
        if (jitter < 1) jitter = 1;
        int delta_refill = (conf.avg_refill_time - jitter) + (rand() % (2 * jitter + 1));
        int prossimo_refill = delta_refill;

        for (int m = 1; m <= MINUTI_PER_GIORNO && shm_ptr->simulazione_attiva; m++) {
            attendi_nanosecondi(conf.n_nano_secs);

            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->minuto_corrente = m;

            // Refill variabile con intervallo dinamico
            if (m >= prossimo_refill) {
                for (int p = 0; p < 2; p++) {
                    if (shm_ptr->porzioni_primi[p] + conf.avg_refill_primi <= conf.max_porzioni_primi)
                        shm_ptr->porzioni_primi[p] += conf.avg_refill_primi;
                    if (shm_ptr->porzioni_secondi[p] + conf.avg_refill_secondi <= conf.max_porzioni_secondi)
                        shm_ptr->porzioni_secondi[p] += conf.avg_refill_secondi;
                }
                printf("[REFILL] Minuto %d: Ricarica porzioni effettuata.\n", m);

                // Pianifica il prossimo refill
                delta_refill = (conf.avg_refill_time - jitter) + (rand() % (2 * jitter + 1));
                prossimo_refill = m + delta_refill;
            }
            sem_v(sem_id, SEM_MUTEX_SHM);
        }

        // Breve attesa per consentire ai processi ancora in transito di registrarsi
        struct timespec drain_time = {0, 50000000L}; // 50ms
        nanosleep(&drain_time, NULL);

        // --- REPORT FINE GIORNATA (Sezione 5.1 del PDF) ---
        sem_p(sem_id, SEM_MUTEX_SHM);
        printf("\n--- Report Fine Giorno %d ---\n", g);
        printf("Utenti serviti oggi:        %d\n", shm_ptr->utenti_serviti_oggi);
        printf("Utenti non serviti oggi:    %d\n", shm_ptr->utenti_non_serviti_oggi);
        printf("Incasso giornaliero:        %.2f EUR\n", shm_ptr->ricavo_oggi);
        printf("Operatori attivi oggi:      %d\n", shm_ptr->operatori_attivi_oggi);
        printf("Pause effettuate oggi:      %d\n", shm_ptr->pause_effettuate_oggi);
        printf("Piatti distribuiti oggi:    Primi: %d | Secondi: %d | Dolce-Caffe': %d\n",
               shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI],
               shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI],
               shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE]);
        printf("Scorte residue:             Primi: [%d, %d] | Secondi: [%d, %d]\n",
               shm_ptr->porzioni_primi[0], shm_ptr->porzioni_primi[1],
               shm_ptr->porzioni_secondi[0], shm_ptr->porzioni_secondi[1]);

        // Tempi medi di attesa della giornata
        printf("Tempi medi di attesa oggi:\n");
        double somma_tempi_oggi = 0.0;
        int somma_visite_oggi = 0;
        for (int s = 0; s < NUM_STATIONS; s++) {
            double media_staz = (shm_ptr->visite_stazione_oggi[s] > 0) ?
                (shm_ptr->tempo_attesa_oggi[s] / shm_ptr->visite_stazione_oggi[s]) : 0.0;
            printf("  - %-14s: %.4f s (%d richieste)\n", staz_nomi[s], media_staz, shm_ptr->visite_stazione_oggi[s]);
            somma_tempi_oggi += shm_ptr->tempo_attesa_oggi[s];
            somma_visite_oggi += shm_ptr->visite_stazione_oggi[s];
        }
        double media_complessiva_oggi = (somma_visite_oggi > 0) ?
            (somma_tempi_oggi / somma_visite_oggi) : 0.0;
        printf("  -> Media complessiva oggi: %.4f s\n", media_complessiva_oggi);

        // Verifica condizione di terminazione OVERLOAD (Sezione 5.6 del PDF)
        if (conf.overload_threshold > 0 &&
            shm_ptr->utenti_non_serviti_oggi > conf.overload_threshold) {
            causa_terminazione = "OVERLOAD (Utenti non serviti a fine giornata > soglia)";
            shm_ptr->simulazione_attiva = 0;
            sem_v(sem_id, SEM_MUTEX_SHM);
            printf("\n[ALERT] Superata la soglia di OVERLOAD (%d > %d). Arresto simulazione.\n",
                   shm_ptr->utenti_non_serviti_oggi, conf.overload_threshold);
            break;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    // Segnalazione fine simulazione a tutti i processi
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->simulazione_attiva = 0;
    sem_v(sem_id, SEM_MUTEX_SHM);

    if (giorni_effettivi_svolti == 0) giorni_effettivi_svolti = 1;

    // --- REPORT FINALE COMPLESSIVO (Sezione 5.1 e 5.6 del PDF) ---
    printf("\n=======================================================\n");
    printf("                  FINE SIMULAZIONE                     \n");
    printf("=======================================================\n");
    printf("Causa di terminazione:      %s\n", causa_terminazione);
    printf("Giorni simulati effettivi:  %d su %d\n", giorni_effettivi_svolti, conf.sim_duration);
    printf("Totale utenti serviti:      %d (Media: %.1f al giorno)\n",
           shm_ptr->utenti_serviti_tot,
           (double)shm_ptr->utenti_serviti_tot / giorni_effettivi_svolti);
    printf("Totale utenti non serviti:  %d (Media: %.1f al giorno)\n",
           shm_ptr->utenti_non_serviti_tot,
           (double)shm_ptr->utenti_non_serviti_tot / giorni_effettivi_svolti);
    printf("Incasso totale complessivo: %.2f EUR (Media: %.2f EUR al giorno)\n",
           shm_ptr->ricavo_totale,
           shm_ptr->ricavo_totale / giorni_effettivi_svolti);
    printf("Totale pause effettuate:    %d (Media: %.1f al giorno)\n",
           shm_ptr->pause_totali,
           (double)shm_ptr->pause_totali / giorni_effettivi_svolti);
    printf("Piatti totali distribuiti:  Primi: %d (media %.1f) | Secondi: %d (media %.1f) | Dolce-Caffe': %d (media %.1f)\n",
           shm_ptr->piatti_distribuiti_tot[STAT_PRIMI],
           (double)shm_ptr->piatti_distribuiti_tot[STAT_PRIMI] / giorni_effettivi_svolti,
           shm_ptr->piatti_distribuiti_tot[STAT_SECONDI],
           (double)shm_ptr->piatti_distribuiti_tot[STAT_SECONDI] / giorni_effettivi_svolti,
           shm_ptr->piatti_distribuiti_tot[STAT_COFFEE],
           (double)shm_ptr->piatti_distribuiti_tot[STAT_COFFEE] / giorni_effettivi_svolti);

    printf("\nTempi medi di attesa complessivi (intera simulazione):\n");
    double somma_tempi_tot = 0.0;
    int somma_visite_tot = 0;
    for (int s = 0; s < NUM_STATIONS; s++) {
        double media_tot_staz = (shm_ptr->visite_stazione_tot[s] > 0) ?
            (shm_ptr->tempo_attesa_tot[s] / shm_ptr->visite_stazione_tot[s]) : 0.0;
        printf("  - %-14s: %.4f s (%d richieste)\n", staz_nomi[s], media_tot_staz, shm_ptr->visite_stazione_tot[s]);
        somma_tempi_tot += shm_ptr->tempo_attesa_tot[s];
        somma_visite_tot += shm_ptr->visite_stazione_tot[s];
    }
    double media_totale_globale = (somma_visite_tot > 0) ?
        (somma_tempi_tot / somma_visite_tot) : 0.0;
    printf("  -> Media complessiva simulazione: %.4f s\n", media_totale_globale);
    printf("=======================================================\n");

    // Esportazione su file CSV
    esporta_statistiche_csv("statistiche.csv", shm_ptr, giorni_effettivi_svolti, causa_terminazione);
    // Pulizia e attesa di tutti i processi figli
    cleanup_resources();
    return 0;
}