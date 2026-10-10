CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean

all: msh

msh: main.c main.h
	$(CC) $(CFLAGS) -o $@ main.c

clean:
	rm -f msh
