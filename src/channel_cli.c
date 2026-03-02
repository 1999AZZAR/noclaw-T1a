#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/select.h>
#include <unistd.h>

typedef struct {
    int dummy;
} cli_ctx;

static void cli_poll(nc_channel *self, nc_agent *agent) {
    /* Non-blocking poll using select */
    fd_set fds;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    
    int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
        char buf[4096];
        if (fgets(buf, sizeof(buf), stdin)) {
            buf[strcspn(buf, "\n")] = 0;
            if (buf[0]) {
                const char *reply = nc_agent_chat(agent, buf);
                printf("\nT1a: %s\n\n> ", reply);
                fflush(stdout);
            } else {
                /* Empty enter, just reprint prompt */
                printf("> ");
                fflush(stdout);
            }
        }
    }
}

static bool cli_send(nc_channel *self, const char *to, const char *text) {
    printf("\n[CLI SEND to %s]: %s\n", to, text);
    return true;
}

nc_channel nc_channel_cli(void) {
    cli_ctx *ctx = calloc(1, sizeof(cli_ctx));
    return (nc_channel){
        .name = "cli",
        .ctx = ctx,
        .poll = cli_poll,
        .send = cli_send,
        .free = NULL
    };
}
