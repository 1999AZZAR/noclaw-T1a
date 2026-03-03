#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int nc_cmd_agent(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    nc_config_apply_env(&cfg);

    nc_log_min_level = NC_LOG_INFO;

    const char *chan_name = "cli";
    const char *msg_arg = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            chan_name = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            msg_arg = argv[++i];
        }
    }

    nc_provider prov = nc_provider_from_config(&cfg, false);
    if (cfg.fallback_provider[0] && cfg.fallback_api_key[0]) {
        nc_provider fallback = nc_provider_from_config(&cfg, true);
        prov = nc_provider_chain(prov, fallback, cfg.fallback_model);
        nc_log(NC_LOG_INFO, "  Fallback: %s (%s)", cfg.fallback_provider, 
               cfg.fallback_model[0] ? cfg.fallback_model : "default model");
    }

    nc_memory mem = nc_memory_flat("workspace/memories.tsv");
    nc_tool tools[NC_MAX_TOOLS];
    int tool_count = 0;

    tools[tool_count++] = nc_tool_shell(&cfg);
    tools[tool_count++] = nc_tool_file_read(&cfg);
    tools[tool_count++] = nc_tool_file_write(&cfg);
    tools[tool_count++] = nc_tool_memory_store(&mem);
    tools[tool_count++] = nc_tool_memory_recall(&mem);
    tools[tool_count++] = nc_tool_get_time();
    tools[tool_count++] = nc_tool_sys_info();
    tools[tool_count++] = nc_tool_calc();
    tools[tool_count++] = nc_tool_http_fetch();
    tools[tool_count++] = nc_tool_list_dir(&cfg);

    tool_count = nc_mcp_register_all(&cfg, tools, tool_count);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    if (msg_arg) {
        printf("%s\n", nc_agent_chat(&agent, msg_arg));
        return 0;
    }

    nc_channel ch;
    if (strcmp(chan_name, "telegram") == 0) {
        /* Priority to ENV, then config file */
        const char *env_token = getenv("NOCLAW_TELEGRAM_TOKEN");
        const char *token = (env_token && env_token[0]) ? env_token : cfg.telegram_token;
        ch = nc_channel_telegram(token);
    } else {
        ch = nc_channel_cli();
    }

    nc_log(NC_LOG_INFO, "T1a v%s -- %s mode", NC_VERSION, chan_name);
    nc_log(NC_LOG_INFO, "  Provider: %s", cfg.default_provider);
    nc_log(NC_LOG_INFO, "  Model:    %s", cfg.default_model);
    nc_log(NC_LOG_INFO, "  Tools:    %d loaded", tool_count);

    while (1) {
        ch.poll(&ch, &agent);
        usleep(50000); 
    }

    return 0;
}

bool nc_commands_execute(nc_agent *agent, const char *cmd, long chat_id, nc_channel *chan) {
    if (cmd[0] != '/') return false;

    char reply[1024];
    char to_buf[32];
    snprintf(to_buf, sizeof(to_buf), "%ld", chat_id);

    if (strcmp(cmd, "/status") == 0) {
        snprintf(reply, sizeof(reply), 
            "T1a Unit Status\n\n"
            "- Model: %s\n"
            "- Tools: %d active\n"
            "- Memory: %s\n"
            "- Uptime: Stable",
            agent->config->default_model, agent->tool_count, agent->config->memory_backend);
    } else if (strcmp(cmd, "/restart") == 0) {
        chan->send(chan, to_buf, "Restarting T1a binary...");
        exit(0);
    } else if (strcmp(cmd, "/reset") == 0) {
        nc_agent_reset(agent);
        snprintf(reply, sizeof(reply), "Conversation reset. Brain is fresh now.");
    } else if (strcmp(cmd, "/help") == 0) {
        snprintf(reply, sizeof(reply),
            "T1a Commands\n\n"
            "/status - Show unit health\n"
            "/reset  - Clear chat history\n"
            "/restart - Force binary reboot\n"
            "/help   - Show this list");
    } else {
        return false;
    }

    chan->send(chan, to_buf, reply);
    return true;
}
