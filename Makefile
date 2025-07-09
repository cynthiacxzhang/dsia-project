# ECE 252 Lab 2 Makefile

CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude
LDFLAGS = -pthread -lcurl -lz
SRC_DIR = src
OBJ_DIR = obj
BIN = paster

SRCS = \
	$(SRC_DIR)/crc.c \
	$(SRC_DIR)/lab_png.c \
	$(SRC_DIR)/zutil.c \
	$(SRC_DIR)/paster.c

OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(BIN) $(LDFLAGS)

all: $(BIN)

clean:
	rm -rf $(OBJ_DIR) *.o $(BIN) all.png
