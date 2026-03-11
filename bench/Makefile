CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -pthread -I../common
LDFLAGS := -lm -lpthread

all: bench

bench: bench.c
	$(CC) $(CFLAGS) -o bench bench.c $(LDFLAGS)

clean:
	rm -f bench
