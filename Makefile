CC ?= cc
CFLAGS ?= -O2 -Wall

all: sender receiver

sender: sender.c
	$(CC) $(CFLAGS) -o sender sender.c -lm

receiver: receiver.c
	$(CC) $(CFLAGS) -o receiver receiver.c -lm

clean:
	rm -f sender receiver
