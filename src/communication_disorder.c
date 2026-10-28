#include "../include/common.h"
#include "../include/config.h"
#include "../include/ipc_utils.h"

int main(int argc, char *argv[]) {
    const char *conf_path = (argc > 1) ? argv[1] : "config/config_timeout.conf";
    Config conf;
    if (parse_config(conf_path, &conf) == -1) {
        fprintf(stderr, "[DISORDER] Impossibile leggere il file di configurazione %s\n", conf_path);
        exit(EXIT_FAILURE);
    }

    key_t sem_key = ftok(FTOK_PATH, FTOK_PROJ_SEM);
    if (sem_key == -1) {
        perror("[DISORDER] ftok fallita");
        exit(EXIT_FAILURE);
    }

    int sem_id = semget(sem_key, NUM_SEMAPHORES, 0600);
    if (sem_id == -1) {
        perror("[DISORDER] semget fallita (la simulazione e' avviata?)");
        exit(EXIT_FAILURE);
    }

    int durata = (conf.stop_duration > 0) ? conf.stop_duration : 5;
    printf("[COMMUNICATION DISORDER] Avvio blocco cassa per %d secondi...\n", durata);

    // Blocca il funzionamento della cassa acquisendo il semaforo a 0
    if (sem_p(sem_id, SEM_CASSA_BLOCCO) == -1) {
        perror("[DISORDER] sem_p su SEM_CASSA_BLOCCO fallita");
        exit(EXIT_FAILURE);
    }

    // Attesa bloccante senza consumo di CPU
    sleep(durata);

    // Rilascia la cassa riportando il semaforo a 1
    if (sem_v(sem_id, SEM_CASSA_BLOCCO) == -1) {
        perror("[DISORDER] sem_v su SEM_CASSA_BLOCCO fallita");
        exit(EXIT_FAILURE);
    }

    printf("[COMMUNICATION DISORDER] Ripristino completato: la cassa e' nuovamente attiva.\n");
    return 0;
}