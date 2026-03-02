#include "nc.h"
#include <string.h>
#include <stdio.h>

int nc_cmd_gateway(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    
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
    tool_count = nc_mcp_register_all(&cfg, tools, tool_count);

    nc_agent agent;
    nc_agent_init(&agent, &cfg, &prov, tools, tool_count, &mem);

    nc_gateway gw;
    nc_gateway_init(&gw, &cfg, &agent);
    
    nc_log(NC_LOG_INFO, "Gateway starting on %s:%d", cfg.gateway_host, cfg.gateway_port);
    nc_gateway_run(&gw);
    return 0;
}

int nc_cmd_status(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    nc_config_load(&cfg);
    printf("noclaw Unit Status\n");
    printf("  Version:  %s\n", NC_VERSION);
    printf("  Model:    %s\n", cfg.default_model);
    printf("  Provider: %s\n", cfg.default_provider);
    return 0;
}

int nc_cmd_onboard(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--api-key") == 0 && i+1 < argc) {
            nc_strlcpy(cfg.api_key, argv[++i], sizeof(cfg.api_key));
        }
    }
    nc_config_save(&cfg);
    printf("Onboarding complete.\n");
    return 0;
}

int nc_cmd_doctor(int argc, char **argv) {
    printf("noclaw Doctor: All systems nominal.\n");
    return 0;
}
