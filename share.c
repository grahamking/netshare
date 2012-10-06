
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

off_t offset[100];
char *headers;

// Process an epoll event
void handleEvent(
        struct epoll_event *evp,
        int sockfd,
        int datafd,
        int efd,
        off_t datasz) {

    int err;
    int connfd = -1;
    ssize_t wrote;

    printf("Is ready: %d\n", evp->data.fd);
    if (evp->data.fd == sockfd) {

        connfd = accept4(sockfd, NULL, 0, SOCK_NONBLOCK);
        if (connfd == -1) {
            error(EXIT_FAILURE, errno, "Error 'accept' on socket");
        }

        evp->events = EPOLLOUT;
        evp->data.fd = connfd;
        err = epoll_ctl(efd, EPOLL_CTL_ADD, connfd, evp);
        if (err == -1) {
            error(EXIT_FAILURE, errno, "Error adding to epoll descriptor");
        }

    } else {
        connfd = evp->data.fd;

        if (offset[connfd] == 0) {
            // Later: Set TCP CORK
            write(connfd, headers, strlen(headers));
        }
        wrote = sendfile(connfd, datafd, &offset[connfd], datasz - offset[connfd]);
        if (wrote == -1) {
            if (errno == EAGAIN) {
                return;
            } else {
                error(EXIT_FAILURE, errno, "Error senfile");
            }
        }

        printf("%d: Wrote %ld / %ld bytes\n",
                connfd,
                offset[connfd],
                datasz);

        if (offset[connfd] >= datasz) {

            err = epoll_ctl(efd, EPOLL_CTL_DEL, connfd, NULL);
            if (err == -1) {
                error(EXIT_FAILURE, errno, "Error removing connfd from epoll");
            }

            printf("Closing %d\n", connfd);
            shutdown(connfd, SHUT_WR);
            /*
            err = close(connfd);  // Also removes it from epoll
            if (err == -1) {
                error(EXIT_FAILURE, errno, "Error closing connfd");
            }

            offset[connfd] = 0;
            */
        }
    }
}

// Start here
int main(int argc, char **argv) {

    struct in_addr localhost;
    struct sockaddr_in addr;
    struct stat datastat;

    memset(&offset, 0, 100);

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
    off_t datasz = datastat.st_size;

    headers = malloc(strlen(HEAD_TMPL) + 12);
    sprintf(headers, HEAD_TMPL, datasz);

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

    int i;
    int num_ready;
    struct epoll_event events[100];

    while (1) {

        num_ready = epoll_wait(efd, events, 100, -1);
        if (num_ready == -1) {
            error(EXIT_FAILURE, errno, "Error on epoll_wait");
        }

        for (i = 0; i < num_ready; i++) {
            handleEvent(&events[i], sockfd, datafd, efd, datasz);
        }
    }

    err = close(datafd);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error closing payload fd");
    }

    err = close(sockfd);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error closing socket fd");
    }

    return 0;
}
