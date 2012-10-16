/*
 * Serve a single file over the web, quite fast.
 *
 * Run it:
 *   share <myfile.txt>
 *   OR
 *   share -h example.com -p 80 -m text/html maintenance.html
 *
 * Default host / port is localhost:8080, mimetype text/plain.
 *
 * ---
 *
 * On loopback with 8k jpeg:
 *  - Thinkpad R61 (Centrino) can get ~11k requests / sec.
 *  - Thinkpad X1 Carbon (Core i5 1.7Ghz) gets ~26k requests/sec.
 *
 * Using ab -n 100000 -c 100 for tests.
 *
 */

/*
 * Copyright 2012 Graham King <graham@gkgk.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <errno.h>
#include <error.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/epoll.h>
#include <sys/mman.h>

//#define DEBUG     // Comment in for verbose output

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_MIME_TYPE "text/plain"
#define USAGE "USAGE: share [-h host] [-p port] [-m mime/type] <filename>\n"

// HTTP headers
#define HEAD_TMPL "HTTP/1.0 200 OK\nCache-Control: max-age=31536000\nExpires: Thu, 31 Dec 2037 23:55:55 GMT\nContent-Type: %s\nContent-Length: %ld\n\n"

char *headers;      // HTTP headers

off_t *offset;              // Current offset within data file at index's fd
uint32_t offsetsz = 100;    // Size of 'offset'

/* Write to socket using sendfile
 * Return 1 if we're done writing, 0 if more is needed.
 */
int swrite_sendfile(int connfd, int datafd, off_t datasz) {

    ssize_t num_wrote = 0;

#ifdef DEBUG
    printf("Calling sendfile with: %d %d &%ld %ld\n", connfd, datafd, offset[connfd], datasz - offset[connfd]);
#endif
    num_wrote = sendfile(connfd, datafd, &offset[connfd], datasz - offset[connfd]);
    if (num_wrote == -1) {
        if (errno == EAGAIN || errno == ECONNRESET) {
            // No data or client closed connection. epoll will tell us next step.
            return 0;
        }
        error(EXIT_FAILURE, errno, "Error %d sendfile", errno);
    }

#ifdef DEBUG
    printf("%d: Wrote total %ld / %ld bytes\n", connfd, offset[connfd], datasz);
#endif

    if (offset[connfd] >= datasz) {
        return 1;
    }

    return 0;
}

/* Close socket */
void sclose(int connfd) {

    offset[connfd] = 0;

    if (close(connfd) == -1) {      // close also removes it from epoll
        error(EXIT_FAILURE, errno, "Error %d closing connfd", errno);
    }
}

/* Increase size of offset storage */
void grow_offset() {

    size_t offt_sz = sizeof(off_t);
    size_t current_sz = offsetsz * offt_sz;

    // Double size of offset storage
    offset = realloc(offset, current_sz * 2);
    if (offset == NULL) {
        error(EXIT_FAILURE, errno, "Error on realloc of offset");
    }

    // Initialize second (new) half with 0's
    memset(&offset[offsetsz], 0, current_sz);

    offsetsz *= 2;
}

/* Accept a new connection on sockfd, and add it to epoll.
 *
 * We re-used the epoll_event to save allocating a new one each time on
 * the stack. I _think_ that's a good idea.
 *
 * Returns -1 if error.
 */
int acceptnew(int sockfd, int efd, struct epoll_event *evp) {

    int connfd = accept4(sockfd, NULL, NULL, SOCK_NONBLOCK);
    if (connfd == -1) {
        if (errno == EAGAIN) {
            // If we were multi-process, we'd get this error if another
            // worker process got there before us - no problem
            return 0;
        } else {
            error(0, errno, "Error %d 'accept' on socket", errno);
            return -1;
        }
    }

    if (connfd >= offsetsz) {
        grow_offset();
    }

#ifdef DEBUG
    printf("%d: Accepted: %d\n", getpid(), connfd);
#endif

    evp->events = EPOLLOUT;
    evp->data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, connfd, evp) == -1) {
        error(EXIT_FAILURE, errno, "Error %d adding to epoll descriptor", errno);
    }

    return 0;
}

/* We're done writing.
 * Shutdown our side of connfd connection, and stop epoll-ing it for out ready.
 */
int shut(int connfd, int efd) {

    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));

    ev.events = EPOLLHUP;
    ev.data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, connfd, &ev) == -1) {
        error(EXIT_FAILURE, errno, "Error %d changing epoll descriptor", errno);
    }

    if (shutdown(connfd, SHUT_WR) == -1) {
        error(0, errno, "Error %d on connection shutdown", errno);
        return -1;
    }

    return 0;
}

/* Process an epoll event */
void do_event(
    struct epoll_event *evp, int sockfd, int efd, int datafd, off_t datasz) {

    int connfd = -1;
    int done = 0;        // Are we done writing?

#ifdef DEBUG
    printf("%d: fd = %d, offset = %jd\n", getpid(), evp->data.fd, (intmax_t) offset[evp->data.fd]);
#endif

    if (evp->data.fd == sockfd) {
#ifdef DEBUG
        printf("%d: New connection\n", getpid());
#endif
        acceptnew(sockfd, efd, evp);

    } else {
        connfd = evp->data.fd;

        if (evp->events & EPOLLOUT) {
#ifdef DEBUG
            printf("%d: EPOLLOUT\n", getpid());
#endif

            done = swrite_sendfile(connfd, datafd, datasz);

            if (done == 1) {
                shut(connfd, efd);
            }

        }

        if (evp->events & EPOLLHUP) {
#ifdef DEBUG
            printf("%d: EPOLLHUP\n", getpid());
#endif
            sclose(connfd);
        }
    }
}

/* Convert domain name to IP address, if needed */
char *as_numeric(char *address) {

    if ('0' <= address[0] && address[0] <= '9') {
        // Already numeric
        return address;
    }

    struct addrinfo hints;
    struct addrinfo *result;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;          /* Any protocol */

    int err = getaddrinfo(address, NULL, &hints, &result);
    if (err < 0) {
        printf("getaddrinfo: %s\n", gai_strerror(err));
        error(EXIT_FAILURE, 0, "Error converting domain name to IP address\n");
    }

    // Result can be several addrinfo records, we use the first
    struct sockaddr_in* saddr = (struct sockaddr_in*)result->ai_addr;
    char *ip_address = inet_ntoa(saddr->sin_addr);

    freeaddrinfo(result);

    return ip_address;
}

/* Open the socket and listen on it. Returns the sockets fd. */
int start_sock(char *address, int port) {

    struct in_addr iaddr;
    struct sockaddr_in saddr;

    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1) {
        error(EXIT_FAILURE, errno, "Error %d creating socket", errno);
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        error(EXIT_FAILURE, errno, "Error %d setting SO_REUSEADDR on socket", errno);
    }

    address = as_numeric(address);
    printf("Listening on: %s:%d\n", address, port);

    memset(&iaddr, 0, sizeof(struct in_addr));
    int err = inet_pton(AF_INET, address, &iaddr);
    if (err != 1) {
        error(EXIT_FAILURE, errno, "Error %d converting address to network format", errno);
    }

    memset(&saddr, 0, sizeof(struct sockaddr_in));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr = iaddr;

    err = bind(sockfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error %d binding socket", errno);
    }

    err = listen(sockfd, SOMAXCONN);
    if (err == -1) {
        error(err, errno, "Error %d listening on socket", errno);
    }

    return sockfd;
}

/* Create epoll fd and add sockfd to it. Returns epoll fd. */
int start_epoll(int sockfd) {

    int efd = epoll_create(1);
    if (efd == -1) {
        error(EXIT_FAILURE, errno, "Error %d creating epoll descriptor", errno);
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));

    ev.events = EPOLLIN;
    ev.data.fd = sockfd;

    int err = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &ev);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error %d adding to epoll descriptor", errno);
    }

    return efd;
}

/* Wait for epoll events and act on them */
void main_loop(int efd, int sockfd, int datafd, off_t datasz) {

    int i;
    int num_ready;
    struct epoll_event events[100];

    while (1) {

        num_ready = epoll_wait(efd, events, 100, -1);
        if (num_ready == -1) {
            error(EXIT_FAILURE, errno, "Error %d on epoll_wait", errno);
        }

#ifdef DEBUG
        printf("%d: Num fd's ready = %d\n", getpid(), num_ready);
#endif
        for (i = 0; i < num_ready; i++) {
            do_event(&events[i], sockfd, efd, datafd, datasz);
        }
    }
}

/* Load the payload file and return it's fd.
 * Second param datasz is output param, size of file in bytes.
 */
int load_file(char *filename, off_t *datasz) {

    int datafd = open(filename, O_RDONLY);
    if (datafd == -1) {
        printf("Attempted to read: '%s'\n", filename);
        error(EXIT_FAILURE, errno, "Error %d opening payload", errno);
    }

    struct stat datastat;
    fstat(datafd, &datastat);
    *datasz = datastat.st_size; // Output param

    return datafd;
}

/* Group headers and data file into a temporary file,
 * so that we can send it with a single sendfile, rather than write
 * headers then sendfile - 1 less syscall.
 * Closes the data file and returns fd of grouped file.
 * groupedsz is output parameter: grouped file size.
*/
int group(char *headers, int datafd, off_t datasz, off_t *groupedsz) {

    char tname[] = "/tmp/netshare_XXXXXX";
    int newfd = mkstemp(tname);
    if (newfd == -1) {
        error(EXIT_FAILURE, errno, "Error %d creating temporary file %s", errno, tname);
    }

    if (write(newfd, headers, strlen(headers)) == -1) {
        error(EXIT_FAILURE, errno, "Error %d writing headers to grouped file", errno);
    }

    // Copy datafd to newfd
    if (sendfile(newfd, datafd, NULL, datasz) == -1) {
        error(EXIT_FAILURE, errno, "Error %d sendfile-ing payload to grouped file", errno);
    }

    fsync(newfd);
    lseek(newfd, 0, SEEK_SET);

    if (close(datafd) == -1) {
        error(EXIT_FAILURE, errno, "Error %d closing payload fd", errno);
    }

    struct stat datastat;
    fstat(newfd, &datastat);
    *groupedsz = datastat.st_size; // Output param

    if (readahead(newfd, 0, *groupedsz) == -1) {
        error(EXIT_FAILURE, errno, "Error %d readahead of grouped file", errno);
    }

    return newfd;
}

/* Parse command line arguments */
void parse_args(int argc, char **argv, char **address, int *port, char **mimetype, char **filename) {

    int ch;
    while ((ch = getopt(argc, argv, "h:p:m:")) != -1) {

        switch (ch) {
            case 'h':
                *address = optarg;
                break;
            case 'p':
                *port = atoi(optarg);
                break;
            case 'm':
                *mimetype = optarg;
                break;
            case '?':
                printf(USAGE);
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, USAGE);
        exit(EXIT_FAILURE);
    }
    *filename = argv[optind];
}

/* Start here */
int main(int argc, char **argv) {

    int port = DEFAULT_PORT;
    char *address = DEFAULT_ADDRESS;
    char *mimetype = DEFAULT_MIME_TYPE;
    char **filename = malloc(sizeof(char*));
    parse_args(argc, argv, &address, &port, &mimetype, filename);

    printf("Serving %s with mime type %s\n", *filename, mimetype);

    off_t datasz;
    int datafd = load_file(*filename, &datasz);

    int sockfd = start_sock(address, port);

    // 12 is for number of chars in content-length
    // Allows max "999999999999" bytes, i.e 1 Gig
    headers = malloc(strlen(HEAD_TMPL) + 12 + strlen(mimetype));
    sprintf(headers, HEAD_TMPL, mimetype, datasz);

    datafd = group(headers, datafd, datasz, &datasz);
    // datasz is now the combined headers + payload size - 'group' changed it

    /* To make multi-process, fork here. Everything should just work.
     * In my tests it goes _slower_ if we fork.
     */
    // fork();

    int efd = start_epoll(sockfd);

    offset = malloc(sizeof(off_t) * offsetsz);
    memset(offset, 0, sizeof(off_t) * offsetsz);

    main_loop(efd, sockfd, datafd, datasz);

    if (close(sockfd) == -1) {
        error(EXIT_FAILURE, errno, "Error %d closing socket fd", errno);
    }
    if (close(datafd) == -1) {
        error(EXIT_FAILURE, errno, "Error %d closing grouped fd", errno);
    }

    return 0;
}
