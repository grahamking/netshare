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
 * On loopback with 8k jpeg can get:
 *  - splice: ~ 9k requests / sec
 *  - sendfile: ~ 7k requests / sec
 * Using ab -n 5000 -c 50 for tests.
 * Concurrency from 20 - 500 gets similar results
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

#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/mman.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_MIME_TYPE "text/plain"
#define USAGE "USAGE: share [-h host] [-p port] [-m mime/type] <filename>\n"
#define HEAD_TMPL "HTTP/1.0 200 OK\nContent-Type: %s\nContent-Length: %ld\n\n"

// Max bytes a Linux pipe can hold
#define PIPE_SIZE (64 * 1024)

// Smaller files are loaded in kernel pipes. Bigger files use sendfile.
#define MEGABYTES (1024 * 1024)
#define THRESHOLD (5 * MEGABYTES)

char *headers;      // HTTP headers

// Used by sendfile case
off_t *offset;              // Current offset within data file at index's fd
uint32_t offsetsz = 100;    // Size of 'offset'

// Used by pipe case
int *pipes;                 // Kernel pipes we put payload into
int num_pipes = 0;          // Length of 'pipes' array
int *pipe_index;            // index = fd, val = pipes index number that fd is at
int pipe_index_sz = 100;    // Number of active connections

int is_pipe = 0;        // Are we using pipes? Otherwise sendfile.

// Write to socket using pipes.
// Return 1 if we're done writing, 0 if more needed.
int swrite_pipe(int connfd, int efd, off_t datasz) {

    int curr_index = pipe_index[connfd]++;
    //printf("connfd: %d, curr_index: %d, num_pipes: %d\n", connfd, curr_index, num_pipes);

    // Copy pipe so we don't consume it
    int pipefds[2];
    pipe(pipefds);
    if (tee(pipes[curr_index], pipefds[1], datasz, 0) == -1) {
        error(EXIT_FAILURE, errno, "Error on tee");
    }
    close(pipefds[1]);

    int pipefd = pipefds[0];

    ssize_t num_wrote = splice(
            pipefd,
            0,
            connfd,
            0,
            PIPE_SIZE,
            SPLICE_F_NONBLOCK);

    if (num_wrote == -1) {
        if (errno == EAGAIN || errno == ECONNRESET) {
            // No data or client closed connection. epoll will tell us next step.
            //printf("EAGAIN\n");
            return 0;
        }
        error(EXIT_FAILURE, errno, "Error splicing out to socket");
    }

    close(pipefd);

    if (curr_index == num_pipes - 1) {
        return 1;
    }

    return 0;
}

// Write to socket using sendfile
// Return 1 if we're done writing, 0 if more is needed.
int swrite_sendfile(int connfd, int efd, int datafd, off_t datasz) {

    if (offset[connfd] == 0) {
        if ( write(connfd, headers, strlen(headers)) == -1 ) {
            error(0, errno, "Error writing headers\n");
        }
    }

    ssize_t num_wrote = sendfile(connfd, datafd, &offset[connfd], datasz - offset[connfd]);
    if (num_wrote == -1) {
        if (errno == EAGAIN || errno == ECONNRESET) {
            // No data or client closed connection. epoll will tell us next step.
            return 0;
        }

        error(EXIT_FAILURE, errno, "Error senfile");
    }

    //printf("%d: Wrote total %ld / %ld bytes\n", connfd, offset[connfd], datasz);

    if (offset[connfd] >= datasz) {
        return 1;
    }

    return 0;
}

// Close socket
void sclose(int connfd) {

    if (is_pipe) {
        pipe_index[connfd] = 0;
    } else {
        offset[connfd] = 0;
    }

    if (close(connfd) == -1) {      // close also removes it from epoll
        error(EXIT_FAILURE, errno, "Error closing connfd");
    }
}

// Increase size of offset storage
void grow_offset() {

    int offtsz = sizeof(off_t);
    off_t *old_offset = offset;
    off_t *new_offset = malloc(offtsz * offsetsz * 2);
    memset(new_offset, 0, offtsz * offsetsz * 2);

    memcpy(new_offset, old_offset, offtsz * offsetsz);

    offset = new_offset;
    free(old_offset);

    offsetsz *= 2;
}

// Increase the size of pipe index storage
void grow_pipe_index() {

    int new_pipe_index_sz = pipe_index_sz * 2;

    int *old_pi = pipe_index;
    int *new_pi = malloc(sizeof(int) * new_pipe_index_sz);
    memset(new_pi, 0, new_pipe_index_sz);

    memcpy(new_pi, old_pi, pipe_index_sz);

    pipe_index = new_pi;
    free(old_pi);

    pipe_index_sz = new_pipe_index_sz;
}

// Accept a new connection on sockfd, and add it to epoll
// We re-used the epoll_event to save allocating a new one each time on
// the stack. I _think_ that's a good idea.
int acceptnew(int sockfd, int efd, struct epoll_event *evp) {

    int connfd = accept4(sockfd, NULL, 0, SOCK_NONBLOCK);
    if (connfd == -1) {
        error(EXIT_FAILURE, errno, "Error 'accept' on socket");
    }

    // TCP_CORK means headers and first part of data will go in same TCP packet
    // Only needed for sendfile case
    if (is_pipe == 0) {
        int optval = 1;
        setsockopt(connfd, IPPROTO_TCP, TCP_CORK, &optval, sizeof(optval));
    }

    if (is_pipe && connfd >= pipe_index_sz) {
        grow_pipe_index();
    }
    else if (connfd >= offsetsz) {
        grow_offset();
    }

    evp->events = EPOLLOUT;
    evp->data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, connfd, evp) == -1) {
        error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
    }

    return connfd;
}

// We're done writing.
// Shutdown our site of connfd connection, and stop epoll-ing it for out ready.
void shut(int connfd, int efd) {

    shutdown(connfd, SHUT_WR);

    // Stop listening to EPOLLOUT. Only waiting for HUP now.
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, connfd, &ev) == -1) {
        error(EXIT_FAILURE, errno, "Error changing epoll descriptor");
    }
}

// Process an epoll event
void do_event(
    struct epoll_event *evp, int sockfd, int efd, int datafd, off_t datasz) {

    int connfd = -1;
    int done = 0;        // Are we done writing?

    //printf("Is ready: %d\n", evp->data.fd);
    if (evp->data.fd == sockfd) {
        connfd = acceptnew(sockfd, efd, evp);

    } else {
        connfd = evp->data.fd;
        //printf("Events: %d\n", evp->events);

        if (evp->events & EPOLLOUT) {

            if (is_pipe) {
                done = swrite_pipe(connfd, efd, datasz);
            } else {
                done = swrite_sendfile(connfd, efd, datafd, datasz);
            }

            if (done) {
                shut(connfd, efd);
            }

        }

        if (evp->events & EPOLLHUP) {
            sclose(connfd);
        }
    }
}

// Open the socket and listen on it. Returns the sockets fd.
int start_sock(char *address, int port) {

    struct in_addr localhost;
    struct sockaddr_in addr;

    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1) {
        error(EXIT_FAILURE, errno, "Error creating socket");
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        error(EXIT_FAILURE, errno, "Error setting SO_REUSEADDR on socket");
    }

    memset(&localhost, 0, sizeof(struct in_addr));
    int err = inet_pton(AF_INET, address, &localhost);
    if (err != 1) {
        error(EXIT_FAILURE, errno, "Error converting address to network format");
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = localhost;

    err = bind(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error binding socket");
    }

    err = listen(sockfd, SOMAXCONN);
    if (err == -1) {
        error(err, errno, "Error listening on socket");
    }

    return sockfd;
}

// Create epoll fd and add sockfd to it. Returns epoll fd.
int start_epoll(int sockfd) {

    int efd = epoll_create(1);
    if (efd == -1) {
        error(EXIT_FAILURE, errno, "Error creating epoll descriptor");
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));

    ev.events = EPOLLIN;
    ev.data.fd = sockfd;

    int err = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &ev);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
    }

    return efd;
}

// Wait for epoll events and act on them
void main_loop(int efd, int sockfd, int datafd, off_t datasz) {

    int i;
    int num_ready;
    struct epoll_event events[100];

    while (1) {

        num_ready = epoll_wait(efd, events, 100, -1);
        if (num_ready == -1) {
            error(EXIT_FAILURE, errno, "Error on epoll_wait");
        }

        for (i = 0; i < num_ready; i++) {
            do_event(&events[i], sockfd, efd, datafd, datasz);
        }
    }
}

// Load the payload file, pre-fetch it, and return it's fd.
// Second param datasz is output param, size of file in bytes.
int load_file(char *filename, off_t *datasz) {

    int datafd = open(filename, O_RDONLY);
    if (datafd == -1) {
        printf("Attempted to read: '%s'\n", filename);
        error(EXIT_FAILURE, errno, "Error opening payload");
    }

    struct stat datastat;
    fstat(datafd, &datastat);
    *datasz = datastat.st_size; // Output param

    if (readahead(datafd, 0, *datasz) == -1) {
        error(EXIT_FAILURE, errno, "Error readahead of data file");
    }

    return datafd;
}

// Return next biggest page multiple after initial.
size_t page_multiple(size_t initial) {

    int page_size = sysconf(_SC_PAGESIZE);
    if (initial < page_size) {
        return page_size;
    } else {
        int pages = (int) (initial / page_size);
        return page_size * (pages + 1);
    }
}

// Store headers + data file in kernel pipes, int 'pipes' global array.
void preload(int datafd, off_t datasz) {

    int total_payload = datasz + strlen(headers);
    int page_align = page_multiple(total_payload);

    char *store = mmap(NULL, page_align, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (store == MAP_FAILED) {
        error(EXIT_FAILURE, errno, "Error mmap storage area");
    }

    memcpy(store, headers, strlen(headers));

    char *mem_fd = mmap(NULL, datasz, PROT_READ, MAP_PRIVATE, datafd, 0);
    if (mem_fd == MAP_FAILED) {
        error(EXIT_FAILURE, errno, "Error mmap of data file");
    }
    memcpy(store + strlen(headers), mem_fd, datasz);

    num_pipes = (int) total_payload / PIPE_SIZE + 1;
    pipes = malloc(sizeof(int) * num_pipes);
    memset(pipes, 0, sizeof(pipes));

    int total_spliced = 0;
    int next_pipe = 0;
    while (total_spliced < total_payload) {

        int pfd[2];
        if (pipe(pfd) == -1) {
            error(EXIT_FAILURE, errno, "Error creating pipe\n");
        }

        struct iovec iov;
        iov.iov_base = store + total_spliced;
        iov.iov_len = page_align - total_spliced;
        ssize_t bytes_spliced = vmsplice(pfd[1], &iov, 1, SPLICE_F_GIFT);
        if (bytes_spliced == -1) {
            error(EXIT_FAILURE, errno, "Error vmsplice");
        }
        close(pfd[1]);

        total_spliced += bytes_spliced;

        pipes[next_pipe++] = pfd[0];
    }

}

// Parse command line arguments
void parse_args(
        int argc,
        char **argv,
        char **address,
        int *port,
        char **mimetype,
        char **filename) {

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

// Start here
int main(int argc, char **argv) {

    int port = DEFAULT_PORT;
    char *address = DEFAULT_ADDRESS;
    char *mimetype = DEFAULT_MIME_TYPE;
    char **filename = malloc(sizeof(char*));
    parse_args(argc, argv, &address, &port, &mimetype, filename);

    printf("Serving %s with mime type %s on %s:%d\n",
            *filename, mimetype, address, port);

    off_t datasz;
    int datafd = load_file(*filename, &datasz);

    int sockfd = start_sock(address, port);

    // 12 is for number of chars in content-length
    // Allows max "999999999999" bytes, i.e 1 Gig
    headers = malloc(strlen(HEAD_TMPL) + 12 + strlen(mimetype));
    sprintf(headers, HEAD_TMPL, mimetype, datasz);

    int transmit_sz;    // How much data to send to clients

    if (datasz < THRESHOLD) {
        printf("Serving with kernel pipes\n");
        is_pipe = 1;

        pipe_index = malloc(sizeof(int) * pipe_index_sz);
        memset(pipe_index, 0, 100);

        preload(datafd, datasz);

        transmit_sz = datasz + strlen(headers);

    } else {
        printf("Serving with sendfile\n");
        is_pipe = 0;

        offset = malloc(sizeof(off_t) * offsetsz);
        memset(offset, 0, sizeof(off_t) * offsetsz);

        transmit_sz = datasz;
    }

    int efd = start_epoll(sockfd);

    main_loop(efd, sockfd, datafd, transmit_sz);

    int err = close(datafd);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error closing payload fd");
    }

    err = close(sockfd);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error closing socket fd");
    }

    return 0;
}
