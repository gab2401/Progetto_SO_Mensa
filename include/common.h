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

// Indici dei semafori System V
enum {
    SEM_MUTEX_SHM = 0,      // Protezione memoria condivisa
    SEM_POSTAZIONI_PRIMI,   // Postazioni operative primi
    SEM_POSTAZIONI_SECONDI, // Postazioni operative secondi
    SEM_POSTAZIONI_COFFEE,  // Postazioni operative caffe' e dolce
    SEM_POSTAZIONI_CASSA,   // Postazioni operative cassa
    SEM_TAVOLI,             // Posti a sedere in mensa
    SEM_BARRIERA_START,     // Sincronizzazione avvio (Wait-for-Zero)
    SEM_INIZIO_GIORNATA,    // Sblocco giornaliero utenti (zero polling)
    SEM_CASSA_BLOCCO,       // Blocco disservizio cassa
    SEM_LETTORE_TICKET,     // Validazione preliminare ticket 80/20
    
    // Inizio dei semafori di barriera per ciascun gruppo
    SEM_GRUPPI_BASE,
    NUM_SEMAPHORES = SEM_GRUPPI_BASE + MAX_GROUPS
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
    int active_today;           // Membri attivi oggi (esclusi rinunciatari)
    int ready_count;            // Quanti componenti hanno completato la scelta
} UserGroup;

typedef struct {
    Config conf;
    int simulazione_attiva;
    int giorno_corrente;
    int minuto_corrente;
    int motivo_terminazione;    // 0: Timeout, 1: Overload

    pid_t pid_cassiere;

    Menu menu;

    // Scorte piatti per tipo
    int porzioni_primi[MAX_DISHES_PER_TYPE];
    int porzioni_secondi[MAX_DISHES_PER_TYPE];

    // Assegnazione giornaliera operatori
    int postazioni_assegnate[NUM_STATIONS];

    // Statistiche giornaliere
    int utenti_serviti_oggi;
    int utenti_non_serviti_oggi;
    int utenti_non_serviti_rinuncia_oggi; // Non serviti per piatti esauriti
    int utenti_non_serviti_tempo_oggi;    // Non serviti allo scoccare del 60esimo min (overload)
    int utenti_finito_giornata;
    int piatti_distribuiti_oggi[NUM_STATIONS];
    int piatti_avanzati_primi_oggi;
    int piatti_avanzati_secondi_oggi;
    int pause_effettuate_oggi;
    double ricavo_oggi;
    int operatori_attivi_oggi;

    // Tempi di attesa giornalieri (in nanosecondi)
    long tempo_attesa_oggi[NUM_STATIONS];
    int ordini_completati_oggi[NUM_STATIONS];

    // Statistiche complessive
    int utenti_serviti_tot;
    int utenti_non_serviti_tot;
    int utenti_non_serviti_rinuncia_tot;
    int utenti_non_serviti_tempo_tot;
    int piatti_distribuiti_tot[NUM_STATIONS];
    int piatti_avanzati_primi_tot;
    int piatti_avanzati_secondi_tot;
    int pause_totali;
    double ricavo_totale;
    int operatori_attivi_totali;

    // Tempi di attesa totali (in nanosecondi)
    long tempo_attesa_totale[NUM_STATIONS];
    int ordini_completati[NUM_STATIONS];

    // Presidio stazioni
    int operatori_in_servizio[NUM_STATIONS];

    // Gestione gruppi
    int num_gruppi;
    UserGroup gruppi[MAX_GROUPS];

    // Gestione utenti dinamici
    int utenti_totali_attivi;
    pid_t utenti_dinamici_pids[MAX_DYNAMIC_USERS];
    int num_utenti_dinamici;
} SharedData;

typedef struct {
    long mtype;          // PID destinatario o 1 (richiesta generica)
    pid_t sender_pid;
    int dish_choice;     // Indice del piatto scelto
    int dish_category;   // 0: Primi, 1: Secondi, 2: Caffè, 3: Dolce
    int giorno;          // Giorno in cui l'ordine e' stato emesso
    int status;          // 0: successo, -1: esaurito, -2: giorno scaduto

    // Campi per la cassa: l'utente segnala cosa ha preso, il cassiere calcola l'importo
    int ha_primo;
    int ha_secondo;
    int ha_coffee;
    int has_ticket;      // 1: utente con ticket, 0: standard
    double importo_calcolato; // Restituito dal cassiere
} OrderMessage;

#endif