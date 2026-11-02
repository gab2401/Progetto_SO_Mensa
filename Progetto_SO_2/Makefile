CC = gcc
CFLAGS = -g -Wvla -Wextra -Werror -D_GNU_SOURCE -Iinclude
SRC_DIR = src
BIN_DIR = bin
OBJ_DIR = obj

# Eseguibili previsti per la versione completa (30/30)
TARGETS = $(BIN_DIR)/responsabile \
          $(BIN_DIR)/cassiere \
          $(BIN_DIR)/operatore \
          $(BIN_DIR)/utente \
          $(BIN_DIR)/communication_disorder \
          $(BIN_DIR)/aggiungi_utenti

all: directories $(TARGETS)

directories:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR)

$(BIN_DIR)/responsabile: $(OBJ_DIR)/responsabile.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/operatore: $(OBJ_DIR)/operatore.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/cassiere: $(OBJ_DIR)/cassiere.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/utente: $(OBJ_DIR)/utente.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/communication_disorder: $(OBJ_DIR)/communication_disorder.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/aggiungi_utenti: $(OBJ_DIR)/aggiungi_utenti.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o
	$(CC) $(CFLAGS) $^ -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Pulizia di emergenza delle risorse IPC System V create dall'utente corrente
clean_ipc:
	@echo "Rimozione code di messaggi..."
	@ipcs -q | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -q
	@echo "Rimozione memoria condivisa..."
	@ipcs -m | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -m
	@echo "Rimozione semafori..."
	@ipcs -s | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -s
	@echo "Pulizia completata."

.PHONY: all clean clean_ipc directories