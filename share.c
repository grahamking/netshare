
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
#include <arpa/inet.h>
#include <sys/sendfile.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <sys/epoll.h>

#define HEAD_TMPL "HTTP/1.0 200 OK\nContent-Type: text/plain\nContent-Length: %ld\n\n"

off_t *offset;  // Stores current offset within data file at index's fd
uint32_t offsetsz = 100;    // Size of 'offset'

char *headers;      // HTTP headers
char buf[128];      // Buffer for consuming reads

// Write to socket
void swrite(int connfd, int datafd, off_t datasz) {

    if (offset[connfd] == 0) {
        // Later: Set TCP CORK
        write(connfd, headers, strlen(headers));
    }

    ssize_t num_wrote = sendfile(connfd, datafd, &offset[connfd], datasz - offset[connfd]);
    if (num_wrote == -1) {
        if (errno == EAGAIN || errno == ECONNRESET) {
            // No data or client closed connection.
            // epoll will tell us the next step
            return;
        }

        error(EXIT_FAILURE, errno, "Error senfile");
    }

    //printf("%d: Wrote total %ld / %ld bytes\n", connfd, offset[connfd], datasz);

    if (offset[connfd] >= datasz) {
        // We're done writing
        //printf("Shutdown %d\n", connfd);
        shutdown(connfd, SHUT_WR);
    }
}

// Close socket
void sclose(int connfd) {

    //printf("Closing %d\n", connfd);
    offset[connfd] = 0;

    if (close(connfd) == -1) {      // close also removes it from epoll
        error(EXIT_FAILURE, errno, "Error closing connfd");
    }
}

// Read from socket
void sread(int connfd) {

    ssize_t num_read = read(connfd, &buf, sizeof(buf));
    //printf("Read: %d\n", num_read);

    if (num_read == -1) {

        if (errno == EAGAIN || errno == ECONNRESET) {
            // Socket not ready or client closed connection
            // epoll will tell us the next step
            return;
        }
        error(EXIT_FAILURE, errno, "Error reading from connfd");
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
    printf("Offset size: %d\n", offsetsz);
}

// Accept a new connection on sockfd, and add it to epoll
// We re-used the epoll_event to save allocating a new one each time on
// the stack. I _think_ that's a good idea.
int acceptnew(int sockfd, int efd, struct epoll_event *evp) {

    int connfd = accept4(sockfd, NULL, 0, SOCK_NONBLOCK);
    if (connfd == -1) {
        error(EXIT_FAILURE, errno, "Error 'accept' on socket");
    }

    if (connfd >= offsetsz) {
        grow_offset();
    }

    evp->events = EPOLLIN | EPOLLOUT;
    evp->data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, connfd, evp) == -1) {
        error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
    }

    return connfd;
}

// Process an epoll event
void do_event(
        struct epoll_event *evp,
        int sockfd,
        int datafd,
        int efd,
        off_t datasz) {

    int connfd = -1;

    //printf("Is ready: %d\n", evp->data.fd);
    if (evp->data.fd == sockfd) {
        connfd = acceptnew(sockfd, efd, evp);

    } else {
        connfd = evp->data.fd;
        //printf("Events: %d\n", evp->events);

        if (evp->events & EPOLLOUT) {
            swrite(connfd, datafd, datasz);
        }

        if (evp->events & EPOLLIN) {
            sread(connfd);
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
            do_event(&events[i], sockfd, datafd, efd, datasz);
        }
    }
}

// Start here
int main(int argc, char **argv) {

    offset = malloc(sizeof(off_t) * offsetsz);
    memset(offset, 0, sizeof(off_t) * offsetsz);

    int sockfd = start_sock("127.0.0.1", 4321);

    int datafd = open("payload.txt", O_RDONLY);
    if (datafd == -1) {
        error(EXIT_FAILURE, errno, "Error opening payload");
    }
    struct stat datastat;
    fstat(datafd, &datastat);
    off_t datasz = datastat.st_size;

    headers = malloc(strlen(HEAD_TMPL) + 12);
    sprintf(headers, HEAD_TMPL, datasz);

    int efd = start_epoll(sockfd);

    main_loop(efd, sockfd, datafd, datasz);

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
