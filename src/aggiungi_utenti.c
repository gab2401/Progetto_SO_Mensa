#include "../include/common.h"
#include "../include/config.h"
#include "../include/ipc_utils.h"

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

    // 1. Aggancio alla memoria condivisa esistente
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

    // 2. Controllo semaforo mutex
    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[ADD_USERS] semget fallita");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    if (!shm_ptr->simulazione_attiva) {
        fprintf(stderr, "[ADD_USERS] Simulazione non attiva o terminata.\n");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    printf("[ADD_USERS] Iniezione dinamica di %d nuovi processi utente...\n", n_new_users);

    srand((unsigned int)(time(NULL) ^ getpid()));

    for (int i = 0; i < n_new_users; i++) {
        // Registrazione in SHM di un nuovo gruppo monoposto per il nuovo arrivato
        sem_p(sem_id, SEM_MUTEX_SHM);
        int g_id = -1;
        if (shm_ptr->num_gruppi < MAX_GROUPS) {
            g_id = shm_ptr->num_gruppi++;
            shm_ptr->gruppi[g_id].total_members = 1;
            shm_ptr->gruppi[g_id].ready_count = 0;
            shm_ptr->gruppi[g_id].barrier_flag = 0;
        }
        shm_ptr->utenti_totali_generati++;
        sem_v(sem_id, SEM_MUTEX_SHM);

        pid_t pid = fork();
        if (pid == -1) {
            perror("[ADD_USERS] fork fallita");
            break;
        }

        if (pid == 0) {
            char g_buf[16];
            snprintf(g_buf, sizeof(g_buf), "%d", g_id);

            // Passiamo "1" come terzo parametro (argv[2]) per indicare is_dynamic = 1
            char *args[] = {"utente", g_buf, "1", NULL};

            execve("./bin/utente", args, NULL);
            perror("[ADD_USERS] execve utente fallita");
            _exit(EXIT_FAILURE);
        }
    }

    printf("[ADD_USERS] Avviati con successo i nuovi utenti.\n");
    shmdt(shm_ptr);
    return 0;
}