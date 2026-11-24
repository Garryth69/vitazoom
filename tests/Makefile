# SPDX-License-Identifier: GPL-3.0-or-later
# Host tests: real sources, stubbed system calls, AddressSanitizer + UBSan.
CC ?= cc
CFLAGS = -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter -Werror \
         -fsanitize=address,undefined -fno-sanitize-recover=all \
         -Ishim -I../src
SRC = test_main.c stubs.c ../src/levels.c ../src/gesture.c ../src/config.c \
      ../src/menu.c ../src/font.c ../src/font_big.c

all: run

vitazoom_tests: $(SRC) $(wildcard ../src/*.h) $(wildcard shim/psp2kern/*.h shim/psp2kern/*/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC)

run: vitazoom_tests
	./vitazoom_tests

clean:
	rm -f vitazoom_tests

.PHONY: all run clean
