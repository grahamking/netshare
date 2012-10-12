
Serve a single file over the web quite fast.

Build:

    gcc -Ofast -Wall share.c -o share

Run locally:

    # Serve on localhost:8080 with mime type text/plain
    share test.txt

Test:

    curl http://localhost:8080/
    telnet localhost 8080

    wget http://localhost:8008/anything.txt  # Part after the host is ignored

Run properly:

    share -h my.example.com -p 8000 -m image/png come_back_later.png

    share -h 123.456.7.8 -p 80 -m text/html maintenance.html

    share -h file.example.com -p 8001 -m application/octet-stream backup.tar.gz

Defaults: Host is localhost, port is 8080, mime type is text/plain.

Note that this serves _only a single file_. If you serve HTML, all your media must be inline (base64 url's, for example).

This program does some unorthodox things, in an effort to be very fast:

 - It shuts down the TCP connection once the file is sent - traditionally the client should initiate connection close. It sets SO_REUSEADDR to handle the resulting TIME_WAIT sockets.
 - It doesn't read anything the client sends.
 - It adds the headers to the payload, and writes the whole thing to a temporary file.

Other than than it mostly just uses `epoll` and `sendfile`, which are both wonderful facilities the kernel provides.

Linux only, recent kernel's (2.6.17+).

Performance: Well that's difficult to judge. With an 8k jpeg, on loopback, I can get 11k requests a second on a low-spec laptop. The limiting factor in a performance test is nearly always going to be the bandwidth of your test clients.

BUGS: /tmp/netshare_XXXXXX files are not cleaned up.
