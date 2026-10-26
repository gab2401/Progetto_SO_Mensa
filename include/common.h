#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>

#define FTOK_PATH "."
#define FTOK_PROJ_SHM 'S'
#define FTOK_PROJ_SEM 'M'
#define FTOK_PROJ_MSG_BASE 100

typedef enum {
    STAT_PRIMI = 0,
    STAT_SECONDI,
    STAT_COFFEE,
    STAT_CASSA,
    NUM_STATIONS
} StationType;

enum {
    SEM_MUTEX_SHM = 0,
    SEM_POSTAZIONI_PRIMI,
    SEM_POSTAZIONI_SECONDI,
    SEM_POSTAZIONI_COFFEE,
    SEM_POSTAZIONI_CASSA,
    SEM_TAVOLI,
    SEM_BARRIERA_START,
    SEM_INIZIO_GIORNATA,
    NUM_SEMAPHORES
};

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

typedef struct {
    int sim_duration;
    long n_nano_secs;
    int nof_workers;
    int nof_users;
    int nof_table_seats;
    int nof_wk_seats[NUM_STATIONS];
    int avg_srvc[NUM_STATIONS];
    int avg_refill_primi;
    int avg_refill_secondi;
    int max_porzioni_primi;
    int max_porzioni_secondi;
    int nof_pause;
    int overload_threshold;
    double price_primi;
    double price_secondi;
    double price_coffee;
} Config;

typedef struct {
    Config conf;
    int simulazione_attiva;
    int giorno_corrente;
    int minuto_corrente;
    int motivo_terminazione; // 0 = Timeout, 1 = Overload

    // Scorte piatti
    int porzioni_primi[2];
    int porzioni_secondi[2];

    // Nomi piatti da menu.txt
    char nomi_primi[2][64];
    char nomi_secondi[2][64];
    char nomi_coffee[4][64];

    // Statistiche utenti
    int utenti_serviti_oggi;
    int utenti_serviti_tot;
    int utenti_non_serviti_oggi;
    int utenti_non_serviti_tot;
    int utenti_finito_giornata; // Conta quanti utenti hanno concluso (serviti o non serviti) il giorno corrente

    // Statistiche piatti
    int piatti_distribuiti_oggi[NUM_STATIONS];
    int piatti_distribuiti_tot[NUM_STATIONS];
    int piatti_avanzati_oggi;
    int piatti_avanzati_tot;

    // Statistiche operatori e pause
    int operatori_in_servizio[NUM_STATIONS];
    int operatori_attivi_oggi;
    int operatori_attivi_tot;
    int pause_effettuate_oggi;
    int pause_totali;

    // Statistiche ricavi
    double ricavo_oggi;
    double ricavo_totale;

    // Tempi di attesa (in nanosecondi)
    long tempo_attesa_oggi[NUM_STATIONS];
    int ordini_completati_oggi[NUM_STATIONS];
    long tempo_attesa_totale[NUM_STATIONS];
    int ordini_completati[NUM_STATIONS];
} SharedData;

typedef struct {
    long mtype;
    pid_t sender_pid;
    int dish_choice;
    double importo;
    int giorno;
    int status; // 0 = OK, -1 = Esaurito, -2 = Giorno scaduto
} OrderMessage;

#endif