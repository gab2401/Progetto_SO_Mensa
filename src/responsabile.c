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
    // 1. Invio SIGTERM a tutti i processi figli iniziali e attesa terminazione
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

    // 2. Invio SIGTERM a tutti gli utenti dinamici iniettati con aggiungi_utenti
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        for (int i = 0; i < shm_ptr->num_utenti_dinamici; i++) {
            if (shm_ptr->utenti_dinamici_pids[i] > 0) {
                kill(shm_ptr->utenti_dinamici_pids[i], SIGTERM);
                waitpid(shm_ptr->utenti_dinamici_pids[i], NULL, 0);
            }
        }
    }

    // 3. Sgancio e rimozione Memoria Condivisa
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        shmdt(shm_ptr);
        shm_ptr = NULL;
    }
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id = -1;
    }

    // 4. Rimozione Set di Semafori
    if (sem_id != -1) {
        semctl(sem_id, 0, IPC_RMID);
        sem_id = -1;
    }

    // 5. Rimozione Code di Messaggi
    for (int i = 0; i < NUM_STATIONS; i++) {
        if (msg_queue_id[i] != -1) {
            msgctl(msg_queue_id[i], IPC_RMID, NULL);
            msg_queue_id[i] = -1;
        }
    }
    printf("\n[CLEANUP] Tutti i processi e le risorse IPC sono stati rimossi con successo.\n");
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

// Svuota con status = -2 eventuali ordini ancora accodati alle stazioni allo scadere del giorno
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

// Scrittura statistiche su file CSV
static void esporta_statistiche_csv(const char *filepath, const SharedData *shm,
                                    int giorni_svolti, const char *causa) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        perror("[RESPONSABILE] Errore apertura file CSV per statistiche");
        return;
    }

    fprintf(fp, "Metrica,Valore_Totale,Media_Giornaliera\n");
    fprintf(fp, "Causa_Terminazione,\"%s\",N/D\n", causa);
    fprintf(fp, "Giorni_Simulati,%d,%d\n", giorni_svolti, giorni_svolti);
    fprintf(fp, "Utenti_Serviti,%d,%.2f\n", shm->utenti_serviti_tot, (double)shm->utenti_serviti_tot / giorni_svolti);
    fprintf(fp, "Utenti_Non_Serviti,%d,%.2f\n", shm->utenti_non_serviti_tot, (double)shm->utenti_non_serviti_tot / giorni_svolti);
    fprintf(fp, "Ricavo_Totale_EUR,%.2f,%.2f\n", shm->ricavo_totale, shm->ricavo_totale / giorni_svolti);
    fprintf(fp, "Pause_Totali,%d,%.2f\n", shm->pause_totali, (double)shm->pause_totali / giorni_svolti);
    
    int tot_piatti = shm->piatti_distribuiti_tot[STAT_PRIMI] +
                     shm->piatti_distribuiti_tot[STAT_SECONDI] +
                     shm->piatti_distribuiti_tot[STAT_COFFEE];
    fprintf(fp, "Piatti_Distribuiti_Totali,%d,%.2f\n", tot_piatti, (double)tot_piatti / giorni_svolti);
    fprintf(fp, "Piatti_Primi_Distribuiti,%d,%.2f\n", shm->piatti_distribuiti_tot[STAT_PRIMI], (double)shm->piatti_distribuiti_tot[STAT_PRIMI] / giorni_svolti);
    fprintf(fp, "Piatti_Secondi_Distribuiti,%d,%.2f\n", shm->piatti_distribuiti_tot[STAT_SECONDI], (double)shm->piatti_distribuiti_tot[STAT_SECONDI] / giorni_svolti);
    fprintf(fp, "Piatti_Coffee_Distribuiti,%d,%.2f\n", shm->piatti_distribuiti_tot[STAT_COFFEE], (double)shm->piatti_distribuiti_tot[STAT_COFFEE] / giorni_svolti);
    fprintf(fp, "Piatti_Avanzati_Totali,%d,%.2f\n", shm->piatti_avanzati_tot, (double)shm->piatti_avanzati_tot / giorni_svolti);

    const char *staz_nomi[NUM_STATIONS] = {"Primi", "Secondi", "Coffee", "Cassa"};
    for (int s = 0; s < NUM_STATIONS; s++) {
        double media_ms = (shm->ordini_completati[s] > 0) ?
            ((double)shm->tempo_attesa_totale[s] / (shm->ordini_completati[s] * 1000000.0)) : 0.0;
        fprintf(fp, "Attesa_Media_%s_ms,%.2f,%.2f\n", staz_nomi[s], media_ms, media_ms);
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
    printf("- Lavoratori: %d, Utenti iniziali: %d\n", conf.nof_workers, conf.nof_users);
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

    // Caricamento del menu da file
    if (carica_menu("config/menu.txt", &shm_ptr->menu) == -1) {
        fprintf(stderr, "[RESPONSABILE] Attenzione: impossibile leggere config/menu.txt\n");
    } else {
        printf("[RESPONSABILE] Menu caricato: %d primi, %d secondi, %d dolci, %d caffe'.\n",
               shm_ptr->menu.num_primi, shm_ptr->menu.num_secondi,
               shm_ptr->menu.num_dolci, shm_ptr->menu.num_caffe);
    }

    // Inizializzazione tempi cumulativi in SHM
    for (int s = 0; s < NUM_STATIONS; s++) {
        shm_ptr->tempo_attesa_totale[s] = 0;
        shm_ptr->ordini_completati[s] = 0;
    }

    // Allocazione array dei PID figli iniziali
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

    // 2. Avvio Operatori (garantendo almeno 1 operatore per stazione primi, secondi, caffe)
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

    while (utenti_rimasti > 0 && cur_group_id < MAX_GROUPS) {
        int max_taglia = (conf.max_users_per_group > 0) ? conf.max_users_per_group : 1;
        int group_size = (rand() % max_taglia) + 1;
        if (group_size > utenti_rimasti) {
            group_size = utenti_rimasti;
        }

        shm_ptr->gruppi[cur_group_id].total_members = group_size;
        shm_ptr->gruppi[cur_group_id].active_today = group_size;
        shm_ptr->gruppi[cur_group_id].ready_count = 0;
        shm_ptr->gruppi[cur_group_id].barrier_flag = 0;

        char group_arg[16];
        snprintf(group_arg, sizeof(group_arg), "%d", cur_group_id);

        for (int k = 0; k < group_size; k++) {
            char *utente_args[] = {"utente", group_arg, "0", NULL};
            pid_t pid_ut = avvia_processo("./bin/utente", utente_args);
            if (pid_ut == -1) { cleanup_resources(); exit(EXIT_FAILURE); }
            child_pids[num_children_spawned++] = pid_ut;
        }

        utenti_rimasti -= group_size;
        cur_group_id++;
    }
    shm_ptr->num_gruppi = cur_group_id;
    printf("[RESPONSABILE] Creati %d utenti suddivisi in %d gruppi.\n", conf.nof_users, cur_group_id);
    printf("[RESPONSABILE] Creati %d processi figli totali. Attesa sincronizzazione alla barriera...\n",
           num_children_spawned);

    // Attesa sincrona che tutti i figli abbiano completato il setup (wait-for-zero)
    sem_wait_zero(sem_id, SEM_BARRIERA_START);
    printf("[RESPONSABILE] Tutti i processi pronti. AVVIO SIMULAZIONE!\n");

    // -------------------------------------------------------------
    // CICLO SIMULAZIONE (GIORNI E MINUTI)
    // -------------------------------------------------------------
    const int MINUTI_PER_GIORNO = 60;
    int giorni_effettivi_svolti = 0;
    const char *causa_terminazione = "TIMEOUT (Raggiunta durata massima SIM_DURATION)";
    const char *staz_nomi[NUM_STATIONS] = {"Primi", "Secondi", "Coffee", "Cassa"};

    for (int g = 1; g <= conf.sim_duration && shm_ptr->simulazione_attiva; g++) {
        giorni_effettivi_svolti = g;

        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g;
        shm_ptr->minuto_corrente = 0;
        shm_ptr->utenti_serviti_oggi = 0;
        shm_ptr->utenti_non_serviti_oggi = 0;
        shm_ptr->utenti_finito_giornata = 0;
        shm_ptr->ricavo_oggi = 0.0;
        shm_ptr->pause_effettuate_oggi = 0;
        shm_ptr->operatori_attivi_oggi = conf.nof_workers;

        // Reset gruppi per la nuova giornata
        for (int gr = 0; gr < shm_ptr->num_gruppi; gr++) {
            shm_ptr->gruppi[gr].active_today = shm_ptr->gruppi[gr].total_members;
            shm_ptr->gruppi[gr].ready_count = 0;
            shm_ptr->gruppi[gr].barrier_flag = 0;
        }

        for (int s = 0; s < NUM_STATIONS; s++) {
            shm_ptr->piatti_distribuiti_oggi[s] = 0;
            shm_ptr->tempo_attesa_oggi[s] = 0;
            shm_ptr->ordini_completati_oggi[s] = 0;
        }
        int utenti_da_avviare = shm_ptr->utenti_totali_attivi;
        sem_v(sem_id, SEM_MUTEX_SHM);

        printf("\n=========================================\n");
        printf("         INIZIO GIORNATA %d              \n", g);
        printf("=========================================\n");

        // Sblocco passivo di tutti gli utenti per il nuovo giorno
        sem_add(sem_id, SEM_INIZIO_GIORNATA, utenti_da_avviare);

        // Calcolo primo intervallo refill: AVG_REFILL_TIME ± 20%
        int jitter = (conf.avg_refill_time * 20) / 100;
        if (jitter < 1) jitter = 1;
        int delta_refill = (conf.avg_refill_time - jitter) + (rand() % (2 * jitter + 1));
        int prossimo_refill = delta_refill;

        for (int m = 1; m <= MINUTI_PER_GIORNO && shm_ptr->simulazione_attiva; m++) {
            attendi_nanosecondi(conf.n_nano_secs);

            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->minuto_corrente = m;

            // Refill con intervallo dinamico parametrico
            if (m >= prossimo_refill) {
                for (int p = 0; p < MAX_DISHES_PER_TYPE; p++) {
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

                delta_refill = (conf.avg_refill_time - jitter) + (rand() % (2 * jitter + 1));
                prossimo_refill = m + delta_refill;
            }
            sem_v(sem_id, SEM_MUTEX_SHM);
        }

        // Chiusura giornata: segnalazione giorno scaduto e rifiuto ritardatari
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g + 1;
        sem_v(sem_id, SEM_MUTEX_SHM);

        respingi_coda_fine_giornata(g);

        // Attesa che tutti gli utenti completino la registrazione
        int completati = 0;
        for (int tentativi = 0; tentativi < 200; tentativi++) {
            sem_p(sem_id, SEM_MUTEX_SHM);
            completati = shm_ptr->utenti_finito_giornata;
            int tot_attivi = shm_ptr->utenti_totali_attivi;
            sem_v(sem_id, SEM_MUTEX_SHM);

            if (completati >= tot_attivi) break;

            respingi_coda_fine_giornata(g);
            attendi_nanosecondi(conf.n_nano_secs / 2);
        }

        // Allineamento di sicurezza dei non serviti se qualcuno e' rimasto bloccato
        sem_p(sem_id, SEM_MUTEX_SHM);
        int tot_oggi = shm_ptr->utenti_serviti_oggi + shm_ptr->utenti_non_serviti_oggi;
        if (tot_oggi < shm_ptr->utenti_totali_attivi) {
            int mancanti = shm_ptr->utenti_totali_attivi - tot_oggi;
            shm_ptr->utenti_non_serviti_oggi += mancanti;
            shm_ptr->utenti_non_serviti_tot += mancanti;
        }

        // Calcolo piatti rimasti nelle vaschette
        int avanzati_oggi = 0;
        for (int p = 0; p < MAX_DISHES_PER_TYPE; p++) {
            avanzati_oggi += shm_ptr->porzioni_primi[p] + shm_ptr->porzioni_secondi[p];
        }
        shm_ptr->piatti_avanzati_oggi = avanzati_oggi;
        shm_ptr->piatti_avanzati_tot += avanzati_oggi;
        shm_ptr->pause_totali += shm_ptr->pause_effettuate_oggi;

        int non_serviti_oggi = shm_ptr->utenti_non_serviti_oggi;

        // Stampa Report Fine Giorno
        printf("\n--- Report Fine Giorno %d ---\n", g);
        printf("Utenti serviti oggi:        %d\n", shm_ptr->utenti_serviti_oggi);
        printf("Utenti non serviti oggi:    %d\n", non_serviti_oggi);
        printf("Incasso giornaliero:        %.2f EUR\n", shm_ptr->ricavo_oggi);
        printf("Operatori attivi oggi:      %d\n", shm_ptr->operatori_attivi_oggi);
        printf("Pause effettuate oggi:      %d\n", shm_ptr->pause_effettuate_oggi);
        printf("Piatti distribuiti oggi:    Primi: %d | Secondi: %d | Coffee: %d (Tot: %d)\n",
               shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI],
               shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI],
               shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE],
               shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI] +
               shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI] +
               shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE]);
        printf("Piatti avanzati a fine giornata: %d\n", shm_ptr->piatti_avanzati_oggi);

        printf("Tempi medi di attesa oggi:\n");
        for (int s = 0; s < NUM_STATIONS; s++) {
            if (shm_ptr->ordini_completati_oggi[s] > 0) {
                double media_ms = (double)shm_ptr->tempo_attesa_oggi[s] /
                                  (shm_ptr->ordini_completati_oggi[s] * 1000000.0);
                printf("  - %-10s: %.2f ms (%d ordini)\n", staz_nomi[s], media_ms, shm_ptr->ordini_completati_oggi[s]);
            } else {
                printf("  - %-10s: N/D (0 ordini)\n", staz_nomi[s]);
            }
        }

        // Verifica condizione di OVERLOAD
        if (conf.overload_threshold > 0 && non_serviti_oggi > conf.overload_threshold) {
            causa_terminazione = "OVERLOAD (Utenti non serviti a fine giornata > soglia)";
            shm_ptr->motivo_terminazione = 1;
            shm_ptr->simulazione_attiva = 0;
            printf("\n[ALERT] Superata soglia di OVERLOAD (%d > %d). Arresto simulazione.\n",
                   non_serviti_oggi, conf.overload_threshold);
            sem_v(sem_id, SEM_MUTEX_SHM);
            break;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    // Arresto simulazione e report conclusivo
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->simulazione_attiva = 0;

    if (giorni_effettivi_svolti == 0) giorni_effettivi_svolti = 1;

    printf("\n=======================================================\n");
    printf("                  FINE SIMULAZIONE                     \n");
    printf("=======================================================\n");
    printf("Causa di terminazione:      %s\n", causa_terminazione);
    printf("Giorni simulati effettivi:  %d su %d\n", giorni_effettivi_svolti, conf.sim_duration);
    printf("Totale utenti serviti:      %d (Media: %.2f al giorno)\n",
           shm_ptr->utenti_serviti_tot, (double)shm_ptr->utenti_serviti_tot / giorni_effettivi_svolti);
    printf("Totale utenti non serviti:  %d (Media: %.2f al giorno)\n",
           shm_ptr->utenti_non_serviti_tot, (double)shm_ptr->utenti_non_serviti_tot / giorni_effettivi_svolti);
    printf("Incasso totale complessivo: %.2f EUR (Media: %.2f EUR al giorno)\n",
           shm_ptr->ricavo_totale, shm_ptr->ricavo_totale / giorni_effettivi_svolti);
    printf("Totale pause effettuate:    %d (Media: %.2f al giorno)\n",
           shm_ptr->pause_totali, (double)shm_ptr->pause_totali / giorni_effettivi_svolti);

    int tot_piatti = shm_ptr->piatti_distribuiti_tot[STAT_PRIMI] +
                     shm_ptr->piatti_distribuiti_tot[STAT_SECONDI] +
                     shm_ptr->piatti_distribuiti_tot[STAT_COFFEE];
    printf("Piatti totali distribuiti:  Primi: %d | Secondi: %d | Coffee: %d (Tot: %d, Media: %.2f)\n",
           shm_ptr->piatti_distribuiti_tot[STAT_PRIMI],
           shm_ptr->piatti_distribuiti_tot[STAT_SECONDI],
           shm_ptr->piatti_distribuiti_tot[STAT_COFFEE],
           tot_piatti, (double)tot_piatti / giorni_effettivi_svolti);
    printf("Piatti avanzati totali:     %d (Media: %.2f al giorno)\n",
           shm_ptr->piatti_avanzati_tot, (double)shm_ptr->piatti_avanzati_tot / giorni_effettivi_svolti);

    printf("\nTempi medi di attesa complessivi:\n");
    for (int s = 0; s < NUM_STATIONS; s++) {
        if (shm_ptr->ordini_completati[s] > 0) {
            double media_ms = (double)shm_ptr->tempo_attesa_totale[s] /
                              (shm_ptr->ordini_completati[s] * 1000000.0);
            printf("  - %-10s: %.2f ms (%d ordini)\n", staz_nomi[s], media_ms, shm_ptr->ordini_completati[s]);
        } else {
            printf("  - %-10s: N/D (0 ordini)\n", staz_nomi[s]);
        }
    }
    printf("=======================================================\n");

    esporta_statistiche_csv("statistiche.csv", shm_ptr, giorni_effettivi_svolti, causa_terminazione);
    sem_v(sem_id, SEM_MUTEX_SHM);

    cleanup_resources();
    return 0;
}