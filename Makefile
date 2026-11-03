CC = gcc
CFLAGS = -g -Wvla -Wextra -Werror -D_GNU_SOURCE -Iinclude
LDFLAGS = -lm

SRC_DIR = src
BIN_DIR = bin
OBJ_DIR = obj

TARGETS = $(BIN_DIR)/responsabile \
          $(BIN_DIR)/cassiere \
          $(BIN_DIR)/operatore \
          $(BIN_DIR)/utente \
          $(BIN_DIR)/communication_disorder \
          $(BIN_DIR)/aggiungi_utenti

all: $(TARGETS)

directories:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR)

$(BIN_DIR)/responsabile: $(OBJ_DIR)/responsabile.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/operatore: $(OBJ_DIR)/operatore.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/cassiere: $(OBJ_DIR)/cassiere.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/utente: $(OBJ_DIR)/utente.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/communication_disorder: $(OBJ_DIR)/communication_disorder.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/aggiungi_utenti: $(OBJ_DIR)/aggiungi_utenti.o $(OBJ_DIR)/config.o $(OBJ_DIR)/ipc_utils.o | directories
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | directories
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) statistiche.csv

clean_ipc:
	@echo "Rimozione code di messaggi..."
	@ipcs -q | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -q
	@echo "Rimozione memoria condivisa..."
	@ipcs -m | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -m
	@echo "Rimozione semafori..."
	@ipcs -s | awk '$$2 ~ /^[0-9]+$$/ {print $$2}' | xargs -r ipcrm -s
	@echo "Pulizia completata."

.PHONY: all clean clean_ipc directories