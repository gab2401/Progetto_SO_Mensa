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
    SEM_MUTEX_SHM = 0,    // Protezione per lettura/scrittura su memoria condivisa
    SEM_POSTAZIONI_PRIMI, // Semaforo contatore per i posti operatore ai primi
    SEM_POSTAZIONI_SECONDI,// Semaforo contatore per i posti operatore ai secondi
    SEM_POSTAZIONI_COFFEE, // Semaforo contatore per i posti operatore al caffè
    SEM_POSTAZIONI_CASSA,  // Semaforo contatore per i posti cassiere
    SEM_TAVOLI,           // Semaforo contatore posti a sedere (NOF_TABLE_SEATS)
    NUM_SEMAPHORES
};

// Configurazione letta da file
typedef struct {
    int sim_duration;           // Giorni totali simulazione
    long n_nano_secs;           // Nanosecondi per simulare 1 minuto
    int nof_workers;            // Numero processi operatore
    int nof_users;              // Numero processi utente
    int nof_table_seats;        // Posti a tavola disponibili
    int nof_wk_seats[NUM_STATIONS]; // Postazioni per stazione
    int avg_refill_primi;       // Ricarica porzioni primi
    int avg_refill_secondi;     // Ricarica porzioni secondi
    int max_porzioni_primi;     // Massimo stoccabile primi
    int max_porzioni_secondi;   // Massimo stoccabile secondi
    int nof_pause;              // Numero max pause operatore
    int overload_threshold;     // Soglia terminazione utenti in attesa
    double price_primi;
    double price_secondi;
    double price_coffee;
    int avg_srvc[NUM_STATIONS]; // Tempi medi di servizio
} Config;

// Struttura in Shared Memory per lo stato globale e le statistiche
typedef struct {
    int simulazione_attiva;     // Flag booleano (1 attiva, 0 stop)
    int giorno_corrente;        // Giorno di simulazione in corso
    
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
    int motivo_terminazione; // 0: timeout, 1: overload
} SharedData;

// Messaggio per code IPC (scambio tra Utente e Operatore/Cassiere)
typedef struct {
    long mtype;          // Canale: PID del destinatario oppure 1 (richiesta generica)
    pid_t sender_pid;    // Chi invia il messaggio
    int dish_choice;     // 0 o 1 (per primi/secondi), o tipo caffè
    double importo;      // Usato per la cassa
    int status;          // 0: successo, -1: piatto esaurito
} OrderMessage;

#endif