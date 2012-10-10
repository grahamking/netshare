
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

Files under 5M are served by stashing them in kernel pipes, files above that use sendfile. Feel free to tweak THRESHOLD - the limiting factor is open files.

This program does some unorthodox things, in an effort to be very fast:

 - It splits the file into 64k chunks and stores those chunks in kernel pipes (if the file is under 5M).
 - It shuts down the TCP connection once the file is sent - traditionally the client should initiate connection close. It sets SO_REUSEADDR to handle the resulting TIME_WAIT sockets.
 - It doesn't read anything the client sends.

Linux only, recent kernel's (2.6.17+).
