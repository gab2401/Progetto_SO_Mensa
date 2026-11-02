#include "common.h"
#include "config.h"
#include "ipc_utils.h"

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
    printf("[COMMUNICATION DISORDER] Avvio blocco cassa per %d secondi virtuali/reali...\n", durata);

    // Blocco cassa protetto con SEM_UNDO (sicuro contro interruzioni brutali)
    if (sem_p_undo(sem_id, SEM_CASSA_BLOCCO) == -1) {
        perror("[DISORDER] sem_p su SEM_CASSA_BLOCCO fallita");
        exit(EXIT_FAILURE);
    }

    sleep(durata);

    if (sem_v_undo(sem_id, SEM_CASSA_BLOCCO) == -1) {
        perror("[DISORDER] sem_v su SEM_CASSA_BLOCCO fallita");
        exit(EXIT_FAILURE);
    }

    printf("[COMMUNICATION DISORDER] Ripristino completato: la cassa e' nuovamente operativa.\n");
    return 0;
}