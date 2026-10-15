#include <stdio.h>
#include "common.h"
#include "config.h"

// Variabili globali per permettere la pulizia nel Signal Handler
static int shm_id = -1;
static SharedData *shm_ptr = NULL;

// Funzione centralizzata di rimozione risorse IPC
void cleanup_resources(void) {
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        shmdt(shm_ptr);
        shm_ptr = NULL;
    }
    if (shm_id != -1) {
        shmctl(shm_id, IPC_RMID, NULL);
        shm_id = -1;
        printf("\n[CLEANUP] Memoria condivisa rimossa con successo.\n");
    }
}

// Handler per SIGINT (Ctrl+C) e SIGTERM
void sig_handler(int signum) {
    (void)signum; // Evita warning parametro non usato con -Wextra
    cleanup_resources();
    _exit(EXIT_SUCCESS); // _exit e' async-signal-safe
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_configurazione>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    Config conf;
    if (parse_config(argv[1], &conf) == -1) {
        exit(EXIT_FAILURE);
    }

    // Stampa di verifica a video
    printf("Configurazione caricata con successo:\n");
    printf("- Durata simulazione: %d giorni\n", conf.sim_duration);
    printf("- Lavoratori: %d, Utenti: %d\n", conf.nof_workers, conf.nof_users);
    printf("- Prezzo primi: %.2f EUR\n", conf.price_primi);

    //Registrazione Handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    //MEMORIA CONDIVISA
    //Dichiarazione variabili per ID e puntatore
    int shm_id;
    SharedData *shm_ptr = NULL;

    //Generare chiave con ftok
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if(shm_key == -1){
        perror("Errore generazione chiave ftok per Memoria condivisa");
        exit(EXIT_FAILURE);
    }

    //Creare il segmento di emoria condivisa nel kernel
    shm_id = shmget(shm_key, sizeof(SharedData), IPC_CREAT | IPC_EXCL | 0600);
    if (shm_id == -1) {
        perror("Errore creazione shmget");
        exit(EXIT_FAILURE);
    }

    //Agganciare la memoria con shmat
    shm_ptr = (SharedData *) shmat(shm_id, NULL, 0);
    if(shm_ptr == (void *)-1){
        perror("Errore aggancio shmat");
        shmctl(shm_id,IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    //Inizializzare a zero la struttura e impostare i valori iniziali
    memset(shm_ptr, 0, sizeof(SharedData));
    shm_ptr->simulazione_attiva = 1;
    shm_ptr->giorno_corrente = 1;
    shm_ptr->minuto_corrente = 0;
    shm_ptr->porzioni_primi[0] = conf.avg_refill_primi;
    shm_ptr->porzioni_primi[1] = conf.avg_refill_primi;
    shm_ptr->porzioni_secondi[0] = conf.avg_refill_secondi;
    shm_ptr->porzioni_secondi[1] = conf.avg_refill_secondi;

    printf("Porzioni iniziali primo 1: %d\n", shm_ptr->porzioni_primi[0]);

    //CREAZIONE SEMAFORI
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, IPC_CREAT | IPC_EXCL | 0600);

    union semun arg;

    // 1. Mutex per la Shared Memory (deve partire a 1 per consentire il primo accesso)
    arg.val = 1;
    semctl(sem_id, SEM_MUTEX_SHM, SETVAL, arg);

    // 2. Postazioni operatori (partono con i valori letti dalla config)
    arg.val = conf.nof_wk_seats[STAT_PRIMI];
    semctl(sem_id, SEM_POSTAZIONI_PRIMI, SETVAL, arg);

    arg.val = conf.nof_wk_seats[STAT_SECONDI];
    semctl(sem_id, SEM_POSTAZIONI_SECONDI, SETVAL, arg);

    arg.val = conf.nof_wk_seats[STAT_COFFEE];
    semctl(sem_id, SEM_POSTAZIONI_COFFEE, SETVAL, arg);

    arg.val = conf.nof_wk_seats[STAT_CASSA];
    semctl(sem_id, SEM_POSTAZIONI_CASSA, SETVAL, arg);

    // 3. Posti tavoli
    arg.val = conf.nof_table_seats;
    semctl(sem_id, SEM_TAVOLI, SETVAL, arg);

    printf("Posti tavoli semaforo:");

    //Deallocazione SHM
    shmdt(shm_ptr);
    shmctl(shm_id, IPC_RMID, NULL);

    //Deallocazione Semafori
    semctl(sem_id, 0, IPC_RMID);

    return 0;
}