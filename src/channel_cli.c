#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct {
    int dummy;
} cli_ctx;

static void cli_poll(nc_channel *self, nc_agent *agent) {
    char buf[4096];
    printf("> ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin)) {
        buf[strcspn(buf, "\n")] = 0;
        if (buf[0]) {
            const char *reply = nc_agent_chat(agent, buf);
            printf("\nT1a: %s\n\n", reply);
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
