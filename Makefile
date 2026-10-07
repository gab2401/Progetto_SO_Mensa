CC = gcc
CFLAGS = -Wvla -Wextra -Werror -D_GNU_SOURCE -Iinclude
SRC_DIR = src
BIN_DIR = bin
OBJ_DIR = obj

# Eseguibili previsti
TARGETS = $(BIN_DIR)/responsabile_mensa $(BIN_DIR)/operatore $(BIN_DIR)/cassiere $(BIN_DIR)/utente

all: directories $(TARGETS)

directories:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR)

$(BIN_DIR)/responsabile_mensa: $(OBJ_DIR)/responsabile_mensa.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/operatore: $(OBJ_DIR)/operatore.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/cassiere: $(OBJ_DIR)/cassiere.o
	$(CC) $(CFLAGS) $^ -o $@

$(BIN_DIR)/utente: $(OBJ_DIR)/utente.o
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