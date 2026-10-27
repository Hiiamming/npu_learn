CC = hexagon-clang
ARCH = v68
CFLAGS = -m$(ARCH) -mhvx -mhvx-length=128b -O3 -Wall
SIM = hexagon-sim

all: main

main: main.c
	$(CC) $(CFLAGS) $< -o $@

run: main
	$(SIM) --timing ./main

clean:
	rm -f main