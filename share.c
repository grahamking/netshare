
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

#define HEADERS "HTTP/1.0 200 OK\n\n"

// Accept and handle a single connection
ssize_t handleSingle(int connfd, int datafd, int datasz) {

    ssize_t wrote;

    write(connfd, HEADERS, strlen(HEADERS));
    wrote = sendfile(connfd, datafd, NULL, datasz);
    if (wrote == -1) {
        error(EXIT_FAILURE, errno, "Error senfile");
    }

    return wrote;
}

// Start here
int main(int argc, char **argv) {

    ssize_t wrote;
    struct in_addr localhost;
    struct sockaddr_in addr;
    struct stat datastat;

    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1) {
        error(EXIT_FAILURE, errno, "Error creating socket");
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        error(EXIT_FAILURE, errno, "Error setting SO_REUSEADDR on socket");
    }

    memset(&localhost, 0, sizeof(struct in_addr));
    int err = inet_pton(AF_INET, "127.0.0.1", &localhost);
    if (err != 1) {
        error(EXIT_FAILURE, errno, "Error converting 127.0.0.1 to network format");
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4321);
    addr.sin_addr = localhost;

    err = bind(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error binding socket");
    }

    err = listen(sockfd, SOMAXCONN);
    if (err == -1) {
        error(err, errno, "Error listening on socket");
    }

    int datafd = open("payload.txt", O_RDONLY);
    if (datafd == -1) {
        error(EXIT_FAILURE, errno, "Error opening payload");
    }
    fstat(datafd, &datastat);

    int efd = epoll_create(1);
    if (efd == -1) {
        error(EXIT_FAILURE, errno, "Error creating epoll descriptor");
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    err = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &ev);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
    }

    int hits = 0;
    int connfd = -1;
    while (1) {
        if (lseek(datafd, 0, SEEK_SET) == -1) {
            error(EXIT_FAILURE, errno, "Error seeking back to payload start");
        }

        err = epoll_wait(efd, &ev, 1, -1);
        if (err == -1) {
            error(EXIT_FAILURE, errno, "Error on epoll_wait");
        }

        printf("Is ready: %d\n", ev.data.fd);
        if (ev.data.fd == sockfd) {

            connfd = accept4(sockfd, NULL, 0, SOCK_NONBLOCK);
            if (connfd == -1) {
                error(EXIT_FAILURE, errno, "Error 'accept' on socket");
            }

            ev.events = EPOLLIN;
            ev.data.fd = connfd;
            err = epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &ev);
            if (err == -1) {
                error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
            }

        } else {
            connfd = ev.data.fd;
            wrote = handleSingle(connfd, datafd, datastat.st_size);

            err = epoll_ctl(efd, EPOLL_CTL_DEL, connfd, NULL);
            if (err == -1) {
                error(EXIT_FAILURE, errno, "Error removing connfd from epoll");
            }
            close(connfd);

            hits++;
            printf("%d: Wrote %d / %d bytes\n",
                    hits, (int)wrote, (unsigned int) datastat.st_size);
        }
    }

    close(datafd);
    close(sockfd);

    return 0;
}
