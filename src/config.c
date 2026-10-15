#include "config.h"

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