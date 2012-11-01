CFLAGS=-Wall -Ofast

all: share

debug:
	gcc -Wall -g -O0 share.c -o share

clean:
	rm -f share

install:
	cp share /usr/bin/netshare
