#include "common.h"
#include "config.h"
#include "ipc_utils.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <N_NEW_USERS>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int n_new_users = atoi(argv[1]);
    if (n_new_users <= 0) {
        fprintf(stderr, "Errore: N_NEW_USERS deve essere maggiore di zero.\n");
        exit(EXIT_FAILURE);
    }

    // 1. Connessione alla memoria condivisa esistente
    key_t shm_key = ftok(FTOK_PATH, FTOK_PROJ_SHM);
    if (shm_key == -1) {
        perror("[ADD_USERS] ftok SHM");
        exit(EXIT_FAILURE);
    }

    int shm_id = shmget(shm_key, sizeof(SharedData), 0600);
    if (shm_id == -1) {
        perror("[ADD_USERS] shmget fallita (la simulazione e' avviata?)");
        exit(EXIT_FAILURE);
    }

    SharedData *shm_ptr = (SharedData *)shmat(shm_id, NULL, 0);
    if (shm_ptr == (void *)-1) {
        perror("[ADD_USERS] shmat fallita");
        exit(EXIT_FAILURE);
    }

    // 2. Connessione ai semafori
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[ADD_USERS] semget fallita");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    sem_p(sem_id, SEM_MUTEX_SHM);
    if (!shm_ptr->simulazione_attiva) {
        fprintf(stderr, "[ADD_USERS] Simulazione non attiva o terminata.\n");
        sem_v(sem_id, SEM_MUTEX_SHM);
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }
    sem_v(sem_id, SEM_MUTEX_SHM);

    printf("[ADD_USERS] Iniezione dinamica di %d nuovi processi utente...\n", n_new_users);

    for (int i = 0; i < n_new_users; i++) {
        sem_p(sem_id, SEM_MUTEX_SHM);

        if (shm_ptr->num_utenti_dinamici >= MAX_DYNAMIC_USERS) {
            fprintf(stderr, "[ADD_USERS] Limite massimo utenti dinamici raggiunto.\n");
            sem_v(sem_id, SEM_MUTEX_SHM);
            break;
        }

        int g_id = -1;
        if (shm_ptr->num_gruppi < MAX_GROUPS) {
            g_id = shm_ptr->num_gruppi++;
            shm_ptr->gruppi[g_id].total_members = 1;
            shm_ptr->gruppi[g_id].active_today = 1;
            shm_ptr->gruppi[g_id].ready_count = 0;

            // Inizializza il semaforo di barriera del nuovo gruppo a 0
            union semun uarg;
            uarg.val = 0;
            semctl(sem_id, SEM_GRUPPI_BASE + g_id, SETVAL, uarg);
        }

        shm_ptr->utenti_totali_attivi++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        pid_t pid = fork();
        if (pid == -1) {
            perror("[ADD_USERS] fork fallita");
            break;
        }

        if (pid == 0) {
            char g_buf[16];
            snprintf(g_buf, sizeof(g_buf), "%d", g_id);

            // argv[1] = group_id, argv[2] = is_dynamic (1 = salta la barriera iniziale)
            char *args[] = {"utente", g_buf, "1", NULL};

            execve("./bin/utente", args, NULL);
            perror("[ADD_USERS] execve utente fallita");
            _exit(EXIT_FAILURE);
        }

        // Tracciamento del PID in SHM per il cleanup del master
        sem_p(sem_id, SEM_MUTEX_SHM);
        shm_ptr->utenti_dinamici_pids[shm_ptr->num_utenti_dinamici++] = pid;
        sem_v(sem_id, SEM_MUTEX_SHM);
    }

    printf("[ADD_USERS] Avviati con successo i nuovi utenti.\n");
    shmdt(shm_ptr);
    return 0;
}