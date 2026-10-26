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

    shm_id = init_shared_memory(&conf, &shm_ptr);
    if (shm_id == -1) { cleanup_resources(); exit(EXIT_FAILURE); }

    // Caricamento del menù nella memoria condivisa
    if (parse_menu("config/menu.txt", shm_ptr) == -1) {
        fprintf(stderr, "Avviso: impossibile caricare config/menu.txt\n");
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

    // 2. Avvio Operatori (garantendo almeno 1 operatore per stazione primi, secondi, caffe')
    int operatori_da_creare = conf.nof_workers - 1;
    int op_assegnati[3] = {0, 0, 0};

    for (int i = 0; i < operatori_da_creare; i++) {
        int staz_id = 0;
        if (i < 3) {
            // Garantiamo almeno 1 operatore per stazione (Primi, Secondi, Caffè)
            staz_id = i;
        } else {
            // Criterio preferenziale: scegliamo la stazione con il rapporto (avg_srvc / op_assegnati) maggiore
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

    // Attesa sincrona che tutti i figli abbiano completato il setup
    sem_wait_zero(sem_id, SEM_BARRIERA_START);
    printf("[RESPONSABILE] Tutti i processi pronti. AVVIO SIMULAZIONE!\n");

    // -------------------------------------------------------------
    // CICLO SIMULAZIONE (GIORNI E MINUTI)
    // -------------------------------------------------------------
    const int MINUTI_PER_GIORNO = 60;

    for (int g = 1; g <= conf.sim_duration && shm_ptr->simulazione_attiva; g++) {
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->giorno_corrente = g;
        shm_ptr->utenti_serviti_oggi = 0;
        shm_ptr->utenti_non_serviti_oggi = 0;
        shm_ptr->ricavo_oggi = 0.0;
        sem_v(sem_id, SEM_MUTEX_SHM);

        printf("\n=========================================\n");
        printf("         INIZIO GIORNATA %d              \n", g);
        printf("=========================================\n");

        for (int m = 1; m <= MINUTI_PER_GIORNO && shm_ptr->simulazione_attiva; m++) {
            attendi_nanosecondi(conf.n_nano_secs);

            sem_p(sem_id, SEM_MUTEX_SHM);
            shm_ptr->minuto_corrente = m;

            // Refill ogni 10 minuti
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

        // Calcolo utenti rimasti in coda a fine giornata
        sem_p(sem_id, SEM_MUTEX_SHM);
        int utenti_in_attesa = conf.nof_users - shm_ptr->utenti_serviti_oggi;
        shm_ptr->utenti_non_serviti_oggi = utenti_in_attesa;
        shm_ptr->utenti_non_serviti_tot += utenti_in_attesa;
        printf("\n--- Report Fine Giorno %d ---\n", g);
        printf("Utenti serviti oggi: %d\n", shm_ptr->utenti_serviti_oggi);
        printf("Utenti non serviti oggi: %d\n", shm_ptr->utenti_non_serviti_oggi);
        printf("Incasso giornaliero: %.2f EUR\n", shm_ptr->ricavo_oggi);
        printf("Scorte residue primi: [%d, %d] | secondi: [%d, %d]\n",
               shm_ptr->porzioni_primi[0], shm_ptr->porzioni_primi[1],
               shm_ptr->porzioni_secondi[0], shm_ptr->porzioni_secondi[1]);
        // Verifica condizione di OVERLOAD
        if (utenti_in_attesa > conf.overload_threshold) {
            printf("\n[ATTENZIONE] Rilevato OVERLOAD: %d utenti in attesa superano la soglia (%d)!\n", utenti_in_attesa, conf.overload_threshold);
            shm_ptr->motivo_terminazione = 1; // 1 = Overload
            shm_ptr->simulazione_attiva = 0;
            sem_v(sem_id, SEM_MUTEX_SHM);
            break;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    // Segnalazione fine simulazione
    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->simulazione_attiva = 0;
    sem_v(sem_id, SEM_MUTEX_SHM);

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->simulazione_attiva = 0;

    printf("\n=========================================\n");
    printf("         FINE SIMULAZIONE                \n");
    if (shm_ptr->motivo_terminazione == 1) {
        printf("Causa terminazione: OVERLOAD (soglia superata)\n");
    } else {
        printf("Causa terminazione: TIMEOUT (raggiunti %d giorni)\n", conf.sim_duration);
    }
    printf("Totale utenti serviti: %d\n", shm_ptr->utenti_serviti_tot);
    printf("Totale utenti non serviti: %d\n", shm_ptr->utenti_non_serviti_tot);
    printf("Incasso totale complessivo: %.2f EUR\n", shm_ptr->ricavo_totale);
    printf("Pause totali effettuate dagli operatori: %d\n", shm_ptr->pause_totali);

    printf("\n--- Tempi Medi di Attesa ---\n");
    const char *nomi_stazioni[] = {"Primi", "Secondi", "Coffee", "Cassa"};
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

    // Pulizia e attesa di tutti i processi figli
    cleanup_resources();
    return 0;
}