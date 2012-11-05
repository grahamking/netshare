CFLAGS=-Wall -Ofast
BIN=$(DESTDIR)/usr/bin

all: netshare

debug:
	$(CC) -Wall -g -O0 netshare.c -o netshare

clean:
	rm -f netshare

install:
	install -d $(BIN)
	install netshare $(BIN)
