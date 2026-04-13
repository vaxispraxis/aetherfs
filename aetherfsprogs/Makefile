CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L
CPPFLAGS += -I../kernel/include/uapi

OBJS := main.o mkfs.o mount.o image.o scrub.o health.o rebalance.o
BIN := aetherfs

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJS)

%.o: %.c mkfs.h mount.h image.h scrub.h health.h rebalance.h ../kernel/include/uapi/linux/aetherfs_format.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
