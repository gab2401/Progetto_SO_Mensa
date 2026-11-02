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

// Percorso fisso per ftok (evita cambi di inode legati al Makefile)
#define FTOK_PATH "."
#define FTOK_PROJ_SHM 'S'
#define FTOK_PROJ_SEM 'M'
#define FTOK_PROJ_MSG_BASE 100

#define MAX_DISH_NAME 64
#define MAX_DISHES_PER_TYPE 4
#define MAX_GROUPS 128
#define MAX_DYNAMIC_USERS 128

#if defined(__GNU_LIBRARY__) && !defined(_SEM_SEMUN_UNDEFINED)
/* union semun e' gia' definita */
#else
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};
#endif

typedef enum {
    STAT_PRIMI = 0,
    STAT_SECONDI,
    STAT_COFFEE,
    STAT_CASSA,
    NUM_STATIONS
} StationType;

// Set vettoriale di semafori System V
enum {
    SEM_MUTEX_SHM = 0,      // Mutua esclusione per SharedData
    SEM_POSTAZIONI_PRIMI,   // Postazioni operative primi
    SEM_POSTAZIONI_SECONDI, // Postazioni operative secondi
    SEM_POSTAZIONI_COFFEE,  // Postazioni operative caffe'
    SEM_POSTAZIONI_CASSA,   // Postazioni operative cassa
    SEM_TAVOLI,             // Posti tavolo (NOF_TABLE_SEATS)
    SEM_BARRIERA_START,     // Barriera iniziale (wait-for-zero)
    SEM_INIZIO_GIORNATA,    // Semaforo di sblocco passivo giornaliero per utenti
    SEM_CASSA_BLOCCO,       // Gestione disservizio POS (communication_disorder)
    SEM_LETTORE_TICKET,     // Coda/Postazione validazione ticket 80/20
    NUM_SEMAPHORES
};

typedef struct {
    char nome[MAX_DISH_NAME];
} Piatto;

typedef struct {
    Piatto primi[MAX_DISHES_PER_TYPE];
    int num_primi;
    Piatto secondi[MAX_DISHES_PER_TYPE];
    int num_secondi;
    Piatto dolci[MAX_DISHES_PER_TYPE];
    int num_dolci;
    Piatto caffe[MAX_DISHES_PER_TYPE];
    int num_caffe;
} Menu;

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

    // Parametri versione 30/30
    int stop_duration;
    int max_users_per_group;
    int avg_refill_time;
    double ticket_discount;
} Config;

typedef struct {
    int total_members;          // Componenti totali del gruppo
    int active_today;           // Membri attivi nella giornata
    int ready_count;            // Quanti hanno terminato il ritiro dei piatti
    int barrier_flag;           // Segnale di via libera per il gruppo
} UserGroup;

typedef struct {
    Config conf;                // Parametri globali accessibili a tutti i processi
    int simulazione_attiva;     // 1 attiva, 0 stop
    int giorno_corrente;
    int minuto_corrente;
    int motivo_terminazione;    // 0: timeout, 1: overload

    pid_t pid_cassiere;         // Per communication_disorder

    Menu menu;

    // Scorte piatti (supporta fino a MAX_DISHES_PER_TYPE)
    int porzioni_primi[MAX_DISHES_PER_TYPE];
    int porzioni_secondi[MAX_DISHES_PER_TYPE];

    // Statistiche giornaliere
    int utenti_serviti_oggi;
    int utenti_non_serviti_oggi;
    int utenti_finito_giornata;
    int piatti_distribuiti_oggi[NUM_STATIONS];
    int pause_effettuate_oggi;
    double ricavo_oggi;
    int operatori_attivi_oggi;

    // Tempi di attesa giornalieri (in nanosecondi)
    long tempo_attesa_oggi[NUM_STATIONS];
    int ordini_completati_oggi[NUM_STATIONS];

    // Statistiche complessive
    int utenti_serviti_tot;
    int utenti_non_serviti_tot;
    int piatti_distribuiti_tot[NUM_STATIONS];
    int piatti_avanzati_oggi;
    int piatti_avanzati_tot;
    int pause_totali;
    double ricavo_totale;

    // Tempi di attesa totali (in nanosecondi)
    long tempo_attesa_totale[NUM_STATIONS];
    int ordini_completati[NUM_STATIONS];

    // Presidio stazioni (controllo pause)
    int operatori_in_servizio[NUM_STATIONS];

    // Gestione gruppi (30/30)
    int num_gruppi;
    UserGroup gruppi[MAX_GROUPS];

    // Gestione utenti dinamici (aggiungi_utenti)
    int utenti_totali_attivi;
    pid_t utenti_dinamici_pids[MAX_DYNAMIC_USERS];
    int num_utenti_dinamici;
} SharedData;

typedef struct {
    long mtype;          // PID destinatario o 1 (richiesta generica)
    pid_t sender_pid;
    int dish_choice;
    double importo;
    int giorno;          // Giorno in cui l'ordine e' stato emesso
    int status;          // 0: successo, -1: esaurito, -2: giorno scaduto
    int has_ticket;      // 1: utente con ticket, 0: standard
} OrderMessage;

#endif