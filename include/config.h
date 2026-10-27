#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

// Legge il file di configurazione e popola la struct Config
int parse_config(const char *filepath, Config *conf);
int carica_menu(const char *filepath, Menu *m);

#endif