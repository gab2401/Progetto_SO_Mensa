#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

#define FTOK_PATH "Makefile"
#define FTOK_PROJ_SHM 10
#define FTOK_PROJ_SEM 20
#define FTOK_PROJ_MSG_BASE 30 // offset per code stazioni

// Union richiesta esplicitamente su Linux per semctl()
#if defined(__GNU_LIBRARY__) && !defined(_SEM_SEMUN_UNDEFINED)
/* union semun e' gia' definita */
#else
union semun {
    int val;                    // Valore per SETVAL
    struct semid_ds *buf;       // Buffer per IPC_STAT, IPC_SET
    unsigned short *array;      // Array per GETALL, SETALL
    struct seminfo *__buf;      // Buffer per IPC_INFO
};
#endif

// Tipi di stazione
typedef enum {
    STAT_PRIMI = 0,
    STAT_SECONDI,
    STAT_COFFEE,
    STAT_CASSA,
    NUM_STATIONS
} StationType;

// Indici per il set di semafori (semget)
enum {
    SEM_MUTEX_SHM = 0,     // Protezione lettura/scrittura memoria condivisa
    SEM_POSTAZIONI_PRIMI,  // Postazioni disponibili primi
    SEM_POSTAZIONI_SECONDI,// Postazioni disponibili secondi
    SEM_POSTAZIONI_COFFEE, // Postazioni disponibili caffe'
    SEM_POSTAZIONI_CASSA,  // Postazioni disponibili cassa
    SEM_TAVOLI,            // Posti a tavola disponibili (NOF_TABLE_SEATS)
    NUM_SEMAPHORES
};

// Configurazione letta da file
typedef struct {
    int sim_duration;
    long n_nano_secs;
    int nof_workers;
    int nof_users;
    int nof_table_seats;
    int nof_wk_seats[NUM_STATIONS];
    int avg_refill_primi;
    int avg_refill_secondi;
    int max_porzioni_primi;
    int max_porzioni_secondi;
    int nof_pause;
    int overload_threshold;
    double price_primi;
    double price_secondi;
    double price_coffee;
    int avg_srvc[NUM_STATIONS];
} Config;

// Struttura in Shared Memory per lo stato globale e le statistiche
typedef struct {
    int simulazione_attiva;     // 1 attiva, 0 stop
    int giorno_corrente;        // Giorno in corso
    int minuto_corrente;        // Minuto virtuale del giorno corrente (per refill ogni 10 min)
    
    // Piatti disponibili (2 tipologie per primi e secondi)
    int porzioni_primi[2];
    int porzioni_secondi[2];
    
    // Contatori per la giornata corrente
    int utenti_serviti_oggi;
    int utenti_non_serviti_oggi;
    int piatti_distribuiti_oggi[NUM_STATIONS];
    int pause_effettuate_oggi;
    double ricavo_oggi;
    int operatori_attivi_oggi;
    
    // Statistiche complessive
    int utenti_serviti_tot;
    int utenti_non_serviti_tot;
    int piatti_distribuiti_tot[NUM_STATIONS];
    int piatti_avanzati_tot[NUM_STATIONS];
    int pause_totali;
    double ricavo_totale;
    
    // Controllo terminazione
    int utenti_in_coda_a_fine_giorno;
    int motivo_terminazione;    // 0: timeout, 1: overload
} SharedData;

// Messaggio per code IPC (scambio tra Utente e Operatore/Cassiere)
typedef struct {
    long mtype;          // Canale: PID del destinatario oppure 1 (richiesta generica)
    pid_t sender_pid;    // Chi invia il messaggio
    int dish_choice;     // 0 o 1 (per primi/secondi), o tipo caffe'
    double importo;      // Usato per la cassa
    int status;          // 0: successo, -1: piatto esaurito
} OrderMessage;

#endif