
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

// Accept and handle a single connection
void handleSingle(int sockfd) {

    int connfd, datafd, wrote;
    struct stat datastat;

    connfd = accept(sockfd, NULL, 0);
    if (connfd == -1) {
        error(-1, errno, "Error 'accept' on socket");
    }

    datafd = open("payload.txt", O_RDONLY);
    if (datafd == -1) {
        error(-1, errno, "Error opening payload");
    }
    fstat(datafd, &datastat);

    wrote = sendfile(connfd, datafd, NULL, datastat.st_size);
    if (wrote == -1) {
        error(-1, errno, "Error senfile");
    }
    printf("Wrote %d / %d bytes\n", wrote, (unsigned int) datastat.st_size);

    close(connfd);
}

// Start here
int main(int argc, char **argv) {

    int sockfd, err, optval;
    struct in_addr localhost;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);  // Later: SOCK_STREAM | SOCK_NONBLOCK. Or not??
    if (sockfd == -1) {
        error(-1, errno, "Error creating socket");
    }

    optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        error(-1, errno, "Error setting SO_REUSEADDR on socket");
    }

    memset(&localhost, 0, sizeof(struct in_addr));
    err = inet_pton(AF_INET, "127.0.0.1", &localhost);
    if (err != 1) {
        error(-1, errno, "Error converting 127.0.0.1 to network format");
    }

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(4321);
    addr.sin_addr = localhost;

    err = bind(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if (err == -1) {
        error(-1, errno, "Error binding socket");
    }

    err = listen(sockfd, SOMAXCONN);
    if (err == -1) {
        error(err, errno, "Error listening on socket");
    }

    while (1) {
        handleSingle(sockfd);
    }

    close(sockfd);

    return 0;
}
