# Simple Makefile for Docs++ (OSN Course Project)
# Builds three binaries: nm (Name Server), ss (Storage Server), client (CLI)

CC := gcc
CFLAGS := -Wall -Wextra -Werror -O2 -std=c11
LDFLAGS := -lpthread

BIN_DIR := bin
BUILD_DIR := build
SRC_COMMON := common/net_proto.c common/tickets.c
INC := -Icommon

NM_SRC := nm/nm_main.c nm/nm_persist.c nm/nm_dir.c $(SRC_COMMON)
SS_SRC := ss/ss_main.c ss/ss_tokenize.c $(SRC_COMMON)
CLI_SRC := client/cli_main.c $(SRC_COMMON)

NM_OBJ := $(NM_SRC:%.c=$(BUILD_DIR)/%.o)
SS_OBJ := $(SS_SRC:%.c=$(BUILD_DIR)/%.o)
CLI_OBJ := $(CLI_SRC:%.c=$(BUILD_DIR)/%.o)

TARGETS := $(BIN_DIR)/nm $(BIN_DIR)/ss $(BIN_DIR)/client

.PHONY: all clean dirs

all: dirs $(TARGETS)

$(BIN_DIR)/nm: $(NM_OBJ)
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/ss: $(SS_OBJ)
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/client: $(CLI_OBJ)
	$(CC) $(CFLAGS) $(INC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) ss_data/ nm_state.json

dirs:
	@mkdir -p $(BIN_DIR) $(BUILD_DIR)
