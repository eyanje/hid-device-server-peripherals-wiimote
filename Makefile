CFLAGS += -I/usr/local/include
LDFLAGS += -L/usr/local/lib -lxwiimote

all: main

clean:
	rm -f main

main: main.c
	LD_RUN_PATH=/usr/local/lib $(CC) $(CFLAGS) $< $(LDFLAGS) -o $@

run: main
	./main