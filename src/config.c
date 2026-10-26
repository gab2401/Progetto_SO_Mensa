#include "config.h"

int parse_menu(const char *filepath, SharedData *shm_ptr) {
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("Errore apertura menu.txt (uso nomi di default)");
        // Fallback su nomi predefiniti in caso di assenza file
        strncpy(shm_ptr->nomi_primi[0], "Primo A", 64);
        strncpy(shm_ptr->nomi_primi[1], "Primo B", 64);
        strncpy(shm_ptr->nomi_secondi[0], "Secondo A", 64);
        strncpy(shm_ptr->nomi_secondi[1], "Secondo B", 64);
        strncpy(shm_ptr->nomi_coffee[0], "Espresso", 64);
        strncpy(shm_ptr->nomi_coffee[1], "Macchiato", 64);
        strncpy(shm_ptr->nomi_coffee[2], "Cappuccino", 64);
        strncpy(shm_ptr->nomi_coffee[3], "Decaffeinato", 64);
        return 0;
    }

    char line[128];
    int count = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        // Rimuove eventuale newline finale
        line[strcspn(line, "\r\n")] = '\0';

        // Salta righe vuote o commenti
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        if (count < 2) {
            strncpy(shm_ptr->nomi_primi[count], line, 63);
            shm_ptr->nomi_primi[count][63] = '\0';
        } else if (count < 4) {
            strncpy(shm_ptr->nomi_secondi[count - 2], line, 63);
            shm_ptr->nomi_secondi[count - 2][63] = '\0';
        } else if (count < 8) {
            strncpy(shm_ptr->nomi_coffee[count - 4], line, 63);
            shm_ptr->nomi_coffee[count - 4][63] = '\0';
        }
        count++;
    }

    fclose(fp);
    return 0;
}

int parse_config(const char *filepath, Config *conf) {
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        perror("Errore apertura file di configurazione");
        return -1;
    }

    char line[256];
    char key[128];
    char val[128];

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '\n' || line[0] == '#') {
            continue;
        }

        if (sscanf(line, "%127[^=]=%127s", key, val) == 2) {
            if (strcmp(key, "SIM_DURATION") == 0) {
                conf->sim_duration = atoi(val);
            } else if (strcmp(key, "NOF_WORKERS") == 0) {
                conf->nof_workers = atoi(val);
            } else if (strcmp(key, "NOF_USERS") == 0) {
                conf->nof_users = atoi(val);
            } else if (strcmp(key, "NOF_TABLE_SEATS") == 0) {
                conf->nof_table_seats = atoi(val);
            } else if (strcmp(key, "AVG_REFILL_PRIMI") == 0) {
                conf->avg_refill_primi = atoi(val);
            } else if (strcmp(key, "AVG_REFILL_SECONDI") == 0) {
                conf->avg_refill_secondi = atoi(val);
            } else if (strcmp(key, "MAX_PORZIONI_PRIMI") == 0) {
                conf->max_porzioni_primi = atoi(val);
            } else if (strcmp(key, "MAX_PORZIONI_SECONDI") == 0) {
                conf->max_porzioni_secondi = atoi(val);
            } else if (strcmp(key, "NOF_PAUSE") == 0) {
                conf->nof_pause = atoi(val);
            } else if (strcmp(key, "OVERLOAD_THRESHOLD") == 0) {
                conf->overload_threshold = atoi(val);
            } else if (strcmp(key, "N_NANO_SECS") == 0) {
                conf->n_nano_secs = atol(val);
            } else if (strcmp(key, "PRICE_PRIMI") == 0) {
                conf->price_primi = atof(val);
            } else if (strcmp(key, "PRICE_SECONDI") == 0) {
                conf->price_secondi = atof(val);
            } else if (strcmp(key, "PRICE_COFFEE") == 0) {
                conf->price_coffee = atof(val);
            } else if (strcmp(key, "NOF_WK_SEATS_PRIMI") == 0) {
                conf->nof_wk_seats[STAT_PRIMI] = atoi(val);
            } else if (strcmp(key, "NOF_WK_SEATS_SECONDI") == 0) {
                conf->nof_wk_seats[STAT_SECONDI] = atoi(val);
            } else if (strcmp(key, "NOF_WK_SEATS_COFFEE") == 0) {
                conf->nof_wk_seats[STAT_COFFEE] = atoi(val);
            } else if (strcmp(key, "NOF_WK_SEATS_CASSA") == 0) {
                conf->nof_wk_seats[STAT_CASSA] = atoi(val);
            } else if (strcmp(key, "AVG_SRVC_PRIMI") == 0) {
                conf->avg_srvc[STAT_PRIMI] = atoi(val);
            } else if (strcmp(key, "AVG_SRVC_SECONDI") == 0) {
                conf->avg_srvc[STAT_SECONDI] = atoi(val);
            } else if (strcmp(key, "AVG_SRVC_COFFEE") == 0) {
                conf->avg_srvc[STAT_COFFEE] = atoi(val);
            } else if (strcmp(key, "AVG_SRVC_CASSA") == 0) {
                conf->avg_srvc[STAT_CASSA] = atoi(val);
            }
        }
    }

    fclose(fp);
    return 0;
}