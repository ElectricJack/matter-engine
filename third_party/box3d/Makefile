# Static-lib build of box3d (v0.1.0). -ffp-contract=off is REQUIRED for
# box3d's cross-platform determinism (upstream sets it via CMake).
CC ?= gcc
CFLAGS = -std=c17 -O2 -ffp-contract=off -Iinclude -Isrc -Wall -Wno-unused-parameter

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

libbox3d.a: $(OBJ)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) libbox3d.a

.PHONY: clean
