
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {

    int c;
    int index;
    while ((c = getopt (argc, argv, "p:m:")) != -1) {

        printf("%c\n", c);
        printf("%s\n", optarg);
    }

    for (index = optind; index < argc; index++) {
        printf ("Non-option argument %s\n", argv[index]);
    }

    return 0;
}
