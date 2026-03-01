#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

typedef struct {
    char name[64];
    char command[512];
    char **args;
    int arg_count;
} mcp_server_ctx;

static bool mcp_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    mcp_server_ctx *mcp = (mcp_server_ctx *)self->ctx;
    
    /* Simplified MCP-over-stdio bridge */
    /* 1. Open pipes */
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return false;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: Connect pipes to stdio */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]);
        close(out_pipe[0]);
        
        /* Exec the server */
        execl("/bin/sh", "sh", "-c", mcp->command, (char *)NULL);
        exit(1);
    }

    /* Parent: Write call, Read result */
    close(in_pipe[0]);
    close(out_pipe[1]);

    /* Wrap the arguments into an MCP request */
    char request[9000];
    snprintf(request, sizeof(request), 
             "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"%s\",\"arguments\":%s},\"id\":1}\n", 
             self->def.name, args_json);
    
    write(in_pipe[1], request, strlen(request));
    close(in_pipe[1]);

    /* Read response */
    size_t total = 0;
    char buf[1024];
    ssize_t n;
    while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0 && total < out_cap - 1) {
        memcpy(out + total, buf, (size_t)n);
        total += (size_t)n;
    }
    out[total] = '\0';
    close(out_pipe[0]);
    waitpid(pid, NULL, 0);

    /* Parse MCP result from JSON-RPC response */
    /* Minimalist hack: just find the "content" or "result" block */
    return true; 
}

int nc_mcp_register_all(const nc_config *cfg, nc_tool *tools, int start_idx) {
    char mcp_path[1024];
    nc_path_join(mcp_path, sizeof(mcp_path), cfg->config_dir, "mcp.json");
    
    size_t len;
    char *data = nc_read_file(mcp_path, &len);
    if (!data) return start_idx;

    nc_arena a;
    nc_arena_init(&a, len * 2 + 1024);
    nc_json *root = nc_json_parse(&a, data, len);
    if (!root) { free(data); nc_arena_free(&a); return start_idx; }

    nc_json *servers = nc_json_get(root, "mcpServers");
    if (!servers || servers->type != NC_JSON_OBJECT) { free(data); nc_arena_free(&a); return start_idx; }

    int count = start_idx;
    for (int i = 0; i < servers->object.count && count < NC_MAX_TOOLS; i++) {
        nc_str name = servers->object.keys[i];
        nc_json *s_cfg = &servers->object.vals[i];
        nc_str cmd = nc_json_str(nc_json_get(s_cfg, "command"), "");
        
        if (cmd.len > 0) {
            mcp_server_ctx *ctx = malloc(sizeof(mcp_server_ctx));
            nc_strlcpy(ctx->name, name.ptr, name.len + 1);
            nc_strlcpy(ctx->command, cmd.ptr, cmd.len + 1);

            /* For now, we register the server itself as a tool that proxies all calls */
            /* In a real implementation, we would first call 'list_tools' on the server */
            /* But for T1a's footprint, we'll start with one proxy tool per server */
            
            tools[count].def.name = strdup(ctx->name);
            tools[count].def.description = "MCP Server Proxy";
            tools[count].def.parameters_json = "{\"type\":\"object\"}";
            tools[count].ctx = ctx;
            tools[count].execute = mcp_execute;
            tools[count].free = NULL;
            count++;
        }
    }

    free(data);
    nc_arena_free(&a);
    return count;
}
