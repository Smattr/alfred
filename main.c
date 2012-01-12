#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <sqlite3.h>

/* Use this as a safe guard for marking code that should never be executed. */
#define UNREACHABLE assert(!"Unreachable code executed!")

#define compile_time_assert(name, condition) \
    typedef char name[(condition) ? 1 : -1 ]

/* Default port to listen on. This can be controlled with the -p command line
 * option.
 */
#define DEFAULT_PORT 3876

/* Default maximum queue size of waiting clients on the port we're listening
 * on. Alfred only handles one concurrent connection anyway, so adjusting this
 * limit is unlikely to be useful.
 */
#define DEFAULT_LISTEN_QUEUE_SIZE 5

/* Chunk size for reading data from the client. Adjust as you see fit. */
#define BUFFER_SIZE 128
/* The buffer is used for printing some debugging information and needs to be
 * large enough to fulfill this purpose.
 */
compile_time_assert(buffer_can_fit_IPv4_address,
    BUFFER_SIZE >= INET_ADDRSTRLEN + 1);

/* Embolden a string when printed to the terminal. Note, this macro cannot be
 * used carelessly and requires a bit of forethought about the effect of its
 * expansion in a given context.
 */
#define BOLD(s) "\033[1m" s "\033[0m"

/* Print an error message and exit. */
#define DIE(...) \
    do { \
        fprintf(stderr, __VA_ARGS__); \
        exit(1); \
    } while (0)

/* Print to stdout if verbosity is enabled. */
#define dprintf(...) \
    do { \
        if (verbose) { \
            printf(__VA_ARGS__); \
        } \
    } while (0)

/* Verbosity. 0 for disabled, 1 for enabled. This setting is controlled by the
 * command line argument -v.
 */
static int verbose;

static int sockfd,      /* Socket to bind and listen on. */
           read_sockfd; /* Socket to communicate with a client. */

static sqlite3 *db;

/* Print program usage information. */
static inline void usage(char *progname) {
    (void)fprintf(stderr,
        "alfred - a no-nonsense SQLite server.\n"
        " usage: %s [options] database\n\n"
        " options:\n"
        "    " BOLD("-h") " Print this help information.\n",
        progname);
}

/* Invoked on SIGTERM/SIGINT to clean up and exit. */
static void quit(int signum) {
    /* We should only be handling SIGTERM or SIGINT. */
    assert(signum == SIGTERM || signum == SIGINT);
    (void)signum; /* Suppress warnings when assertions are off. */

    /* Clean up. */
    dprintf("\nCaught %s. Exiting...\n",
        (signum == SIGTERM ? "SIGTERM" : "SIGINT"));
    (void)close(read_sockfd);
    (void)close(sockfd);
    sqlite3_close(db);
    exit(0);
}

/* Wrapper on realloc to avoid handling out of memory. */
static void *xrealloc(void *ptr, size_t size) {
    ptr = realloc(ptr, size);
    if (ptr == NULL) {
        DIE("Out of memory.\n");
    }
    return ptr;
}

static void transmit(char *format, ...) {
    (void)format;
}

static int callback(void *req, int argc, char **argv, char **column) {
    /* TODO: implement me. */
    (void)req;
    (void)argc;
    (void)argv;
    (void)column;
    return 0;
}

int main(int argc, char **argv) {
    int c;
    struct sockaddr_in server, client;
    socklen_t client_sz;
    int sz;
    char buffer[BUFFER_SIZE];
    char *offload_buffer;
    int offload_buffer_sz;
    char *err;
    uint32_t *req = 0; /* Request sequence number. */

    /* Default settings. */
    int port = DEFAULT_PORT;

    /* Command line argument parsing. */
    while ((c = getopt(argc, argv, "hp:v")) != -1) {
        switch (c) {
            case 'h': {
                usage(argv[0]);
                return 0;
            } case 'p': {
                port = atoi(optarg);
                if (port == 0) {
                    DIE("Invalid port specified.\n");
                }
                break;
            } case 'v': {
                verbose = 1;
                break;
            } case '?': /* Fall through. */
              default: {
                usage(argv[0]);
                return 1;
            }
        }
    }

    /* Get the database to open. */
    if (optind == argc) {
        DIE("Missing required database argument. %s -h for usage information.\n", argv[0]);
    } else if (optind < argc - 1) {
        DIE("You can only open a single database per alfred instance. %s -h for usage information.\n", argv[0]);
    } else {
        if (sqlite3_open(argv[optind], &db)) {
            sqlite3_close(db);
            DIE("Failed to open %s.\n", argv[optind]);
        }
    }

    /* Bind and listen for connections. */
    if ((sockfd = socket(AF_INET, SOCK_STREAM /* TCP */, 0)) < 0) {
        DIE("Could not open socket.\n");
    }
    c = 1; /* Abuse a variable to store socket option. */
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&c, sizeof(c)) == -1) {
        DIE("Could not set socket to reuse addresses.\n");
    }
    (void)memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr*)&server, sizeof(server)) < 0) {
        DIE("Could not bind socket.\n");
    }
    if (listen(sockfd, DEFAULT_LISTEN_QUEUE_SIZE) != 0) {
        DIE("Could not listen on socket.\n");
    }

    /* Event loop. Use Ctrl+C to exit. */
    if ((signal(SIGTERM, &quit) == SIG_ERR) || (signal(SIGINT, &quit) == SIG_ERR)) {
        dprintf("Warning: cannot establish signal handler. Ctrl+C will not exit cleanly.\n");
    }
    do {
        /* Wait for an incoming connection. */
        client_sz = sizeof(client);
        dprintf("Waiting for connection on %s:%d\n",
            (server.sin_addr.s_addr == 0 ? "*" : inet_ntop(AF_INET, &server.sin_addr,
            buffer, INET_ADDRSTRLEN)), ntohs(server.sin_port));
        if ((read_sockfd = accept(sockfd, (struct sockaddr*)&client,
            &client_sz)) < 0) {
            DIE("Could not establish connection.\n");
        }
        dprintf("Connection from %s:%d\n",
            inet_ntop(AF_INET, &client.sin_addr, buffer, INET_ADDRSTRLEN),
            ntohs(client.sin_port));

        /* TODO: Add support for a magic exit command. */
        /* Read data from the client. */
        offload_buffer = NULL;
        offload_buffer_sz = 0;
        while ((sz = read(read_sockfd, buffer, BUFFER_SIZE)) > 0) {

            /* Shunt the characters into the offload buffer. The purpose of
             * this move is to support requests greater than the buffer size.
             */
            offload_buffer = (char*)xrealloc(offload_buffer,
                sizeof(char) * (offload_buffer_sz + sz));
            (void)strncpy(offload_buffer + offload_buffer_sz, buffer, sz);
            offload_buffer_sz += sz;
            if (offload_buffer[offload_buffer_sz - 1] != '\n') {
                /* The request is overflowing the buffer. */
                continue;
            }

            /* Terminate the buffer. */
            offload_buffer = (char*)xrealloc(offload_buffer,
                sizeof(char) * (offload_buffer_sz + 1));
            offload_buffer[offload_buffer_sz] = '\0';

            dprintf("Received %d character(s): %s", offload_buffer_sz,
                offload_buffer);
            /* Bonus marks if you can see why req is typed as uint32_t and then
             * cast multiple times in the following line. Not a typo...
             */
            if (sqlite3_exec(db, offload_buffer, &callback,
                (void*)(uintptr_t)++req, &err) != SQLITE_OK) {
                dprintf("Failed to execute query %lu: %s\n", (unsigned long)req, err);
                transmit("%d: Error: %s\n", req, err);
                sqlite3_free(err);
            }
            /* We could free offload_buffer at this point, but it saves time to
             * just let the next realloc take care of this.
             */
            offload_buffer_sz = 0;
        }
        dprintf("Client disconnected.\n");
    } while (1);

    /* quit() handles exit actions. */
    UNREACHABLE;
    return 0;
}
