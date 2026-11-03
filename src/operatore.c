#include "common.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {
    setup_term_handler();

    if (argc < 2) {
        fprintf(stderr, "Uso: %s <id_stazione>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int station_id = atoi(argv[1]);
    if (station_id < 0 || station_id >= STAT_CASSA) {
        fprintf(stderr, "[OPERATORE] ID stazione non valido: %d\n", station_id);
        exit(EXIT_FAILURE);
    }

    srand(getpid() ^ (unsigned int)time(NULL));

    // 1. Aggancio Memoria Condivisa
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) { 
        perror("[OPERATORE] shmget"); 
        exit(EXIT_FAILURE); 
    }

    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) { 
        perror("[OPERATORE] shmat"); 
        exit(EXIT_FAILURE); 
    }

    // 2. Recupero Semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) { 
        perror("[OPERATORE] semget"); 
        shmdt(shm_ptr);
        exit(EXIT_FAILURE); 
    }

    // 3. Recupero Coda Messaggi
    key_t msg_key = ftok(FTOK_PATH, FTOK_PROJ_MSG_BASE + station_id);
    int msg_q_id = msgget(msg_key, 0600);
    if (msg_q_id == -1) { 
        perror("[OPERATORE] msgget"); 
        shmdt(shm_ptr);
        exit(EXIT_FAILURE); 
    }

    int sem_postazione = SEM_POSTAZIONI_PRIMI + station_id;

    // 4. Barriera avvio sincrono
    sem_p(sem_id, SEM_BARRIERA_START);
    sem_wait_zero(sem_id, SEM_BARRIERA_START);

    // 5. Occupazione postazione fisica
    if (sem_p(sem_id, sem_postazione) == -1) {
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    shm_ptr->operatori_in_servizio[station_id]++;
    sem_v(sem_id, SEM_MUTEX_SHM);

    printf("[OPERATORE %d] In servizio alla stazione %d\n", getpid(), station_id);

    // 6. Ciclo di servizio richieste
    OrderMessage msg;
    int pause_fatte_oggi = 0;
    int giorno_locale = 1;
    int clienti_serviti_da_ultima_pausa = 0;

    while (shm_ptr->simulazione_attiva) {
        ssize_t res = msgrcv(msg_q_id, &msg, sizeof(OrderMessage) - sizeof(long), 1, 0);
        if (res == -1) {
            if (errno == EINTR || errno == EIDRM) break;
            continue;
        }

        sem_p(sem_id, SEM_MUTEX_SHM);
        int g_curr = shm_ptr->giorno_corrente;
        if (g_curr != giorno_locale) {
            giorno_locale = g_curr;
            pause_fatte_oggi = 0;
            clienti_serviti_da_ultima_pausa = 0;
        }

        // Se l'ordine appartiene a una giornata terminata, respinge l'utente
        if (msg.giorno != g_curr) {
            sem_v(sem_id, SEM_MUTEX_SHM);
            msg.status = -2;
            msg.mtype = msg.sender_pid;
            msgsnd(msg_q_id, &msg, sizeof(OrderMessage) - sizeof(long), 0);
            continue;
        }

        int piatto_disponibile = 0;
        int scelta = msg.dish_choice;

        if (station_id == STAT_PRIMI) {
            if (scelta >= 0 && scelta < shm_ptr->menu.num_primi && shm_ptr->porzioni_primi[scelta] > 0) {
                shm_ptr->porzioni_primi[scelta]--;
                shm_ptr->piatti_distribuiti_oggi[STAT_PRIMI]++;
                shm_ptr->piatti_distribuiti_tot[STAT_PRIMI]++;
                piatto_disponibile = 1;
            }
        } else if (station_id == STAT_SECONDI) {
            if (scelta >= 0 && scelta < shm_ptr->menu.num_secondi && shm_ptr->porzioni_secondi[scelta] > 0) {
                shm_ptr->porzioni_secondi[scelta]--;
                shm_ptr->piatti_distribuiti_oggi[STAT_SECONDI]++;
                shm_ptr->piatti_distribuiti_tot[STAT_SECONDI]++;
                piatto_disponibile = 1;
            }
        } else if (station_id == STAT_COFFEE) {
            shm_ptr->piatti_distribuiti_oggi[STAT_COFFEE]++;
            shm_ptr->piatti_distribuiti_tot[STAT_COFFEE]++;
            piatto_disponibile = 1;
        }
        sem_v(sem_id, SEM_MUTEX_SHM);

        if (piatto_disponibile) {
            int variazione = (station_id == STAT_COFFEE) ? 80 : 50;
            simula_servizio(shm_ptr->conf.avg_srvc[station_id], variazione, shm_ptr->conf.n_nano_secs);
            msg.status = 0;
        } else {
            msg.status = -1;
        }

        msg.mtype = msg.sender_pid;
        msgsnd(msg_q_id, &msg, sizeof(OrderMessage) - sizeof(long), 0);

        clienti_serviti_da_ultima_pausa++;

        // Valutazione pausa con vincolo di presidio minimo
        if (clienti_serviti_da_ultima_pausa >= 2 && pause_fatte_oggi < shm_ptr->conf.nof_pause) {
            if ((rand() % 10) < 3) {
                int puo_andare_in_pausa = 0;

                sem_p(sem_id, SEM_MUTEX_SHM);
                if (shm_ptr->operatori_in_servizio[station_id] > 1) {
                    shm_ptr->operatori_in_servizio[station_id]--;
                    shm_ptr->pause_effettuate_oggi++;
                    puo_andare_in_pausa = 1;
                }
                sem_v(sem_id, SEM_MUTEX_SHM);

                if (puo_andare_in_pausa) {
                    pause_fatte_oggi++;
                    clienti_serviti_da_ultima_pausa = 0;

                    sem_v(sem_id, sem_postazione);
                    printf("[OPERATORE %d] Va in pausa (stazione %d, pausa %d)\n",
                           getpid(), station_id, pause_fatte_oggi);

                    attendi_nanosecondi(2 * shm_ptr->conf.n_nano_secs);

                    if (shm_ptr->simulazione_attiva) {
                        if (sem_p(sem_id, sem_postazione) == -1) break;

                        sem_p(sem_id, SEM_MUTEX_SHM);
                        shm_ptr->operatori_in_servizio[station_id]++;
                        sem_v(sem_id, SEM_MUTEX_SHM);
                        printf("[OPERATORE %d] Rientrato dalla pausa alla stazione %d\n",
                               getpid(), station_id);
                    }
                }
            }
        }
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    if (shm_ptr->operatori_in_servizio[station_id] > 0) {
        shm_ptr->operatori_in_servizio[station_id]--;
    }
    sem_v(sem_id, SEM_MUTEX_SHM);

    sem_v(sem_id, sem_postazione);
    shmdt(shm_ptr);
    printf("[OPERATORE %d] Chiusura servizio.\n", getpid());
    return 0;
}