CC = riscv64-unknown-linux-gnu-gcc
CXX = riscv64-unknown-linux-gnu-g++
CFLAGS = -O3 -Wall -g -DRISCV
CXXFLAGS = $(CFLAGS)

PGMS = latency bandwidth bandwidth-mon bandwidth-rt pll pagetype cpuhog

all: $(PGMS)

bandwidth-rt: bandwidth-rt.o
	$(CC) $(CFLAGS) $< -o $@ -lrt -lpthread

install:
	cp -v $(PGMS) /usr/local/bin

clean:
	rm -f *.o *~ $(PGMS)
