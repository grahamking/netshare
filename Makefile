CFLAGS=-Wall -Ofast
CC=gcc
bindir=/usr/bin/
INSTALL_PROGRAM=$(bindir)install

all: netshare

debug:
	$(CC) -Wall -g -O0 netshare.c -o netshare

clean:
	rm -f netshare

install:
	$(INSTALL_PROGRAM) netshare $(DESTDIR)$(bindir)/netshare
