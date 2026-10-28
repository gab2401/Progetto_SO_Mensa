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

#define MAX_DISH_NAME 64
#define MAX_DISHES_PER_TYPE 4
#define MAX_GROUPS 128        // Numero massimo di gruppi gestibili in simulazione

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
    SEM_BARRIERA_START,    // Barriera per avvio sincrono di tutti i processi

    // Nuovi semafori (30/30)
    SEM_CASSA_BLOCCO,      // Mutex per blocco cassa da communication_disorder (init a 1)
    SEM_LETTORE_TICKET,    // Postazione per validazione ticket (init a 1)

    NUM_SEMAPHORES         // DEVE ESSERE IN FONDO per avere la dimensione corretta del set
};

// Struttura del singolo piatto/bevanda
typedef struct {
    char nome[MAX_DISH_NAME];
} Piatto;

// Struttura del menu caricato da menu.txt
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

    // Parametri versione 30/30
    int stop_duration;
    int max_users_per_group;
    int avg_refill_time;
    double ticket_discount;
} Config;

// Struttura per la sincronizzazione del gruppo (30/30)
typedef struct {
    int total_members;          // Numero totale componenti del gruppo
    int ready_count;            // Quanti hanno preso tutti i piatti scelti
    int barrier_flag;           // 1 quando tutti i componenti sono pronti ad andare alla cassa
} UserGroup;

// Struttura in Shared Memory per lo stato globale e le statistiche
typedef struct {
    int simulazione_attiva;     // 1 attiva, 0 stop
    int giorno_corrente;        // Giorno in corso
    int minuto_corrente;        // Minuto virtuale del giorno corrente

    // Menu caricato all'avvio
    Menu menu;

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

    // Tempi di attesa giornalieri (in secondi reali)
    double tempo_attesa_oggi[NUM_STATIONS];
    int visite_stazione_oggi[NUM_STATIONS];

    // Statistiche complessive
    int utenti_serviti_tot;
    int utenti_non_serviti_tot;
    int piatti_distribuiti_tot[NUM_STATIONS];
    int piatti_avanzati_tot[NUM_STATIONS];
    int pause_totali;
    double ricavo_totale;

    // Tempi di attesa complessivi dell'intera simulazione
    double tempo_attesa_tot[NUM_STATIONS];
    int visite_stazione_tot[NUM_STATIONS];

    // Controllo terminazione
    int utenti_in_coda_a_fine_giorno;
    int motivo_terminazione;    // 0: timeout, 1: overload

    // Operatori attualmente presenti e attivi alla stazione (per evitare stazioni vuote)
    int operatori_in_servizio[NUM_STATIONS];

    // Supporto per gruppi e utenti dinamici (30/30)
    int num_gruppi;
    UserGroup gruppi[MAX_GROUPS];
    int utenti_totali_generati;
} SharedData;

// Messaggio per code IPC (scambio tra Utente e Operatore/Cassiere)
typedef struct {
    long mtype;          // Canale: PID del destinatario oppure 1 (richiesta generica)
    pid_t sender_pid;    // Chi invia il messaggio
    int dish_choice;     // 0 o 1 (per primi/secondi), o tipo caffe'
    double importo;      // Usato per la cassa
    int status;          // 0: successo, -1: piatto esaurito
    int has_ticket;      // 1: utente con ticket (sconto in cassa), 0: standard
} OrderMessage;

#endif