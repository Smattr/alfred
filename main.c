/* alfred - A no-nonsense SQLite server.
 *  Matthew Fernandez <matthew.fernandez@gmail.com> January, 2012
 *
 * This code is licensed under a CC BY 3.0 licence. In brief, this means you
 * can do anything you like with it as long as you retain attribution to the
 * original author. For more information see
 * https://creativecommons.org/licenses/by/3.0/.
 *
 * This code implements a TCP server for accessing a SQLite database. It is
 * designed to be simple to use, simple to understand and simple to modify. Of
 * course this means some compromises have been made in its design. For
 * example, there is absolutely no security model applied. This means you
 * should never run alfred on an untrusted network. Anyone on your local
 * network (or the internet if you are forwarding ports) can easily locate
 * alfred by port sniffing and connect with no authentication.
 *
 * Alfred expects plain text data sent from a client representing SQL queries.
 * It treats every \n terminated block of text as a query to execute. The data
 * returned to the client is plain text in the form "ID: STATUS: [DATA]".
 * Alfred will generate an ID for each query executed and return "ID: 0: " if
 * the query is executed successfully. Otherwise it will return "ID: -1: DATA"
 * where DATA is a textual description of the error. When results are ready
 * they will be returned as "ID: 1: DATA" where ID corresponds to the query
 * that initiated this result and DATA is of the form "column name = value".
 * The easiest way to see how this works is to try it out. Note that query IDs
 * are 32-bit values that wrap around, so the client must anticipate collisions
 * if it is not throttling itself or checking for "ID: 0: " messages before
 * sending new queries.
 *
 * You are free to modify this code in any way you see fit. If you are making
 * changes and have questions, feel free to email me. Similarly if there are
 * features you would like added email me and I may do it. Similarly, please
 * email me if you find bugs.
 */

#include <arpa/inet.h> /* Non-standard. */
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

/* By default, the Makefile downloads the SQLite sources and statically links
 * them during compilation to make things foolproof. If you have the libraries
 * installed you can disable this behaviour by using `make STATIC=0`.
 */
#include <sqlite3.h>

/* Use this as a safe guard for marking code that should never be executed. */
#define UNREACHABLE assert(!"Unreachable code executed!")

/* GCC's excessive warnings are nice to have on, but sometimes really hard to
 * quash when you know what you're doing. Use this macro to wrap a function
 * call where you really, really do want to ignore the result.
 */
#define IGNORE_RESULT(x) \
    do { \
        if (x) { \
            /* Nothing. */ \
        } \
    } while (0)

#define compile_time_assert(name, condition) \
    typedef char name[(condition) ? 1 : -1 ]

/* Default port to listen on. I just picked this port randomly, but it can be
 * controlled with the -p command line option.
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
 * large enough to hold an IP address.
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

/* Verbosity. 0 for disabled, 1 for enabled. This setting is controlled by the
 * command line argument -v.
 */
static int verbose;
/* Print to stdout if verbosity is enabled. */
#define dprintf(...) \
    do { \
        if (verbose) { \
            printf(__VA_ARGS__); \
        } \
    } while (0)

/* Responses that may be received from alfred. */
enum response {
    OK = 0,   /* Query executed successfully. */
    ERR = -1, /* Query failed.                */
    DATA = 1, /* Query result being returned. */
};

/* Whether to send a prompt character to the client when ready for more input.
 * You can control this behaviour with the -n command line argument and may
 * wish to disable it if you are scripting applications.
 */
static int prompt = 1;
#define SEND_PROMPT \
    do { \
        if (prompt) { \
            IGNORE_RESULT(write(read_sockfd, "> ", 2)); \
        } \
    } while (0)

/* These variables need to be global because they are accessed from the signal
 * handler (quit()) and callback. Minor yuck.
 */
static int sockfd,      /* Socket to bind and listen on.        */
           read_sockfd; /* Socket to communicate with a client. */
static sqlite3 *db;     /* The database we are accessing.       */

/* Print program usage information. */
static inline void usage(char *progname) {
    (void)fprintf(stderr,
        "alfred - a no-nonsense SQLite server.\n"
        " usage: %s [options] database\n\n"
        " options:\n"
        "    " BOLD("-h") "\n"
        "     Print this help information.\n"
        "    " BOLD("-n") "\n"
        "     Disable the prompt that is sent to the client.\n"
        "    " BOLD("-p port") "\n"
        "     Listen on the designated port (default %d).\n"
        "    " BOLD("-r") "\n"
        "     Open the database read-only.\n"
        "    " BOLD("-v") "\n"
        "     Be verbose.\n"
        , progname, DEFAULT_PORT);
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

/* Send data to the client. Note that alfred's protocol is implicit in this
 * function.
 *
 * @param req Request ID.
 * @param code Response type.
 * @param data Data to send.
 * @return Number of characters sent.
 */
static int transmit(uint32_t req, enum response code, char *data) {
    char *msg;
    int n;

    /* We shouldn't be transmitting anything before a connection has been made.
     */
    assert(read_sockfd > 0);

    /* We should be sending a valid code. */
    assert(code == OK || code == ERR || code == DATA);

    msg = (char*)xrealloc(NULL, 10 /* characters to store uint32_t. */
                              + strlen(": ")
                              + 2 /* characters to store enum response. */
                              + strlen(": ")
                              + strlen(data)
                              + 2 /* \n\0 */);
    sprintf(msg, "%u: %d: %s\n", req, code, data);
    n = write(read_sockfd, msg, strlen(msg));
    if (n < 0) dprintf("Error writing to socket.\n");
    free(msg);
    return n;
}

/* This function is invoked when a query returns data. */
static int callback(void *req, int argc, char **argv, char **column) {
    char *data = NULL;
    int i;

    for (i = 0; i < argc; ++i) {
        data = (char*)xrealloc(data, strlen(column[i])
                                   + strlen(" = ")
                                   + (argv[i] ? strlen(argv[i]) : strlen("NULL"))
                                   + 1 /* \0 */);
        sprintf(data, "%s = %s", column[i], argv[i] ? argv[i] : "NULL");
        (void)transmit((uint32_t)(uintptr_t)req, DATA, data);
    }
    free(data);
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
    uint32_t req = 0; /* Request sequence number. */

    /* Default settings. */
    int port = DEFAULT_PORT;
    int readonly = 0;

    /* Command line argument parsing. */
    while ((c = getopt(argc, argv, "hnp:v")) != -1) {
        switch (c) {
            case 'h': {
                usage(argv[0]);
                return 0;
            } case 'n': {
                prompt = 0;
                break;
            } case 'p': {
                port = atoi(optarg);
                if (port == 0) {
                    DIE("Invalid port specified.\n");
                }
                break;
            } case 'r': {
                readonly = 1;
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
        if ((readonly && sqlite3_open_v2(argv[optind], &db, SQLITE_OPEN_READONLY, NULL)) ||
            (!readonly && sqlite3_open(argv[optind], &db))) {
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
        SEND_PROMPT;
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
                (void)transmit(req, ERR, err);
                sqlite3_free(err);
            } else {
                (void)transmit(req, OK, "");
            }
            /* We could free offload_buffer at this point, but it saves time to
             * just let the next realloc take care of this.
             */
            offload_buffer_sz = 0;
            SEND_PROMPT;
        }
        dprintf("Client disconnected.\n");
        (void)close(read_sockfd);
        read_sockfd = 0;
    } while (1);

    /* quit() handles exit actions. */
    UNREACHABLE;
    return 0;
}
