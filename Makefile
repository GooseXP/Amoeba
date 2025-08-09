# Makefile

APP      := amoeba
CC       := gcc

# ---- configuration selection ----
# CONFIG can be: release, debug, gdb
CONFIG ?= release

STD      := -std=c11
WARN     := -Wall -Wextra -Wpedantic
OPT_R    := -O2
OPT_D    := -O0
DBG_GDB  := -ggdb3
DBG_GEN  := -g
THREADS  := -pthread

CPPFLAGS := -Iinclude -D_GNU_SOURCE
LDFLAGS  :=
LDLIBS   := $(THREADS) -lrt

# Build dir and binary name depend on configuration
SRC_DIR  := src
BLD_DIR  := build/$(CONFIG)
BIN      := $(APP)-$(CONFIG)

SRCS := \
  $(SRC_DIR)/assoc.c \
  $(SRC_DIR)/main.c \
  $(SRC_DIR)/database.c \
  $(SRC_DIR)/learning.c \
  $(SRC_DIR)/command.c \
  $(SRC_DIR)/exec.c \
  $(SRC_DIR)/trend.c \
  $(SRC_DIR)/threads.c

OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BLD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

# ---- per-config flags ----
ifeq ($(CONFIG),release)
  CFLAGS := $(STD) $(WARN) $(OPT_R) $(DBG_GEN) $(THREADS)
endif

ifeq ($(CONFIG),debug)
  CFLAGS  := $(STD) $(WARN) $(OPT_D) -fno-omit-frame-pointer -fsanitize=address,undefined $(DBG_GEN) $(THREADS)
  LDFLAGS := -fsanitize=address,undefined
  LDLIBS  := $(THREADS) -lrt -ldl
endif

ifeq ($(CONFIG),gdb)
  CFLAGS := $(STD) $(WARN) $(OPT_D) -fno-omit-frame-pointer $(DBG_GDB) $(THREADS)
endif

# ---- rules ----
.PHONY: all clean run release debug gdb

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BLD_DIR)/%.o: $(SRC_DIR)/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

run: all
	./$(BIN)

release:
	$(MAKE) CONFIG=release clean all

debug:
	$(MAKE) CONFIG=de
