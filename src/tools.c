#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/* ── Helper: extract string from tool args JSON ───────────────── */

static bool extract_json_string(const char *json, const char *key,
                                char *out, size_t out_cap) {
    nc_arena a;
    nc_arena_init(&a, strlen(json) * 2 + 1024); /* Increased arena for deeper JSON */
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) { nc_arena_free(&a); return false; }

    nc_json *val = nc_json_get(root, key);
    nc_str s = nc_json_str(val, "");
    if (s.len == 0) { nc_arena_free(&a); return false; }

    size_t cplen = s.len < out_cap - 1 ? s.len : out_cap - 1;
    memcpy(out, s.ptr, cplen);
    out[cplen] = '\0';
    nc_arena_free(&a);
    return true;
}

/* ── Shell tool ───────────────────────────────────────────────── */

static bool shell_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char command[4096];

    if (!extract_json_string(args_json, "command", command, sizeof(command))) {
        nc_strlcpy(out, "error: missing 'command' argument", out_cap);
        return false;
    }

    if (strstr(command, "noclaw") || strstr(command, "start_t1a.sh")) {
        nc_strlcpy(out, "error: recursive command execution forbidden", out_cap);
        return false;
    }

    char final_cmd[8192];
    if (cfg->workspace_only) {
        snprintf(final_cmd, sizeof(final_cmd), "cd '%s' && %s 2>&1", cfg->workspace_dir, command);
    } else {
        snprintf(final_cmd, sizeof(final_cmd), "%s 2>&1", command);
    }

    FILE *fp = popen(final_cmd, "r");
    if (!fp) {
        nc_strlcpy(out, "error: popen failed", out_cap);
        return false;
    }

    size_t total = 0;
    char buf[2048];
    while (fgets(buf, sizeof(buf), fp) && total < out_cap - 1) {
        size_t n = strlen(buf);
        if (total + n >= out_cap - 1) n = out_cap - 1 - total;
        memcpy(out + total, buf, n);
        total += n;
    }
    out[total] = '\0';
    int status = pclose(fp);

    if (total == 0 && status != 0) {
        snprintf(out, out_cap, "error: exit code %d", WEXITSTATUS(status));
        return false;
    }

    return status == 0;
}

nc_tool nc_tool_shell(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "shell",
            .description = "Execute a shell command and return its output.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
        },
        .ctx = (void *)cfg,
        .execute = shell_execute,
        .free = NULL,
    };
}

/* ── File read tool ───────────────────────────────────────────── */

static bool file_read_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];

    if (!extract_json_string(args_json, "path", path, sizeof(path))) {
        nc_strlcpy(out, "error: missing 'path'", out_cap);
        return false;
    }

    char full_path[2048];
    if (path[0] == '/') {
        nc_strlcpy(full_path, path, sizeof(full_path));
    } else {
        nc_path_join(full_path, sizeof(full_path), cfg->workspace_dir, path);
    }

    size_t len;
    char *content = nc_read_file(full_path, &len);
    if (!content) {
        snprintf(out, out_cap, "error: cannot read %s", full_path);
        return false;
    }

    size_t cplen = len < out_cap - 1 ? len : out_cap - 1;
    memcpy(out, content, cplen);
    out[cplen] = '\0';
    free(content);
    return true;
}

nc_tool nc_tool_file_read(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_read",
            .description = "Read file content.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_read_execute,
        .free = NULL,
    };
}

/* ── File write tool ──────────────────────────────────────────── */

static bool file_write_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];
    /* 256KB for large content on small SBCs */
    char *content = malloc(262144);
    if (!content) return false;

    if (!extract_json_string(args_json, "path", path, sizeof(path)) ||
        !extract_json_string(args_json, "content", content, 262144)) {
        free(content);
        return false;
    }

    char full_path[2048];
    if (path[0] == '/') {
        nc_strlcpy(full_path, path, sizeof(full_path));
    } else {
        nc_path_join(full_path, sizeof(full_path), cfg->workspace_dir, path);
    }

    bool ok = nc_write_file(full_path, content, strlen(content));
    if (ok) snprintf(out, out_cap, "OK: wrote %zu bytes to %s", strlen(content), path);
    else snprintf(out, out_cap, "error: write failed");

    free(content);
    return ok;
}

nc_tool nc_tool_file_write(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_write",
            .description = "Write content to file.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_write_execute,
        .free = NULL,
    };
}

/* ── Memory tools ────────────────────────────────────────────── */

static bool memory_store_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;
    char key[256], content[16384]; /* Larger content for memories */
    if (!extract_json_string(args_json, "key", key, sizeof(key)) ||
        !extract_json_string(args_json, "content", content, sizeof(content))) return false;
    if (mem->store(mem, key, content)) {
        snprintf(out, out_cap, "Stored: %s", key);
        return true;
    }
    return false;
}

nc_tool nc_tool_memory_store(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_store",
            .description = "Store info in memory.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"key\",\"content\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_store_execute,
        .free = NULL,
    };
}

static bool memory_recall_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;
    char query[1024];
    if (!extract_json_string(args_json, "query", query, sizeof(query))) return false;
    return mem->recall(mem, query, out, out_cap);
}

nc_tool nc_tool_memory_recall(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_recall",
            .description = "Recall from memory.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_recall_execute,
        .free = NULL,
    };
}

/* ── Time tool (WorldTimeAPI) ────────────────────────────────── */

static bool get_time_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    char timezone[64];
    timezone[0] = '\0';
    
    extract_json_string(args_json, "timezone", timezone, sizeof(timezone));
    
    char url[256];
    if (timezone[0]) {
        /* Sanitize timezone: alphanumeric, /, _, -, + only */
        for (char *p = timezone; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '/' || *p == '_' || *p == '-' || *p == '+')) {
                snprintf(out, out_cap, "error: invalid characters in timezone");
                return false;
            }
        }
        if (strstr(timezone, "..") || strstr(timezone, "//")) {
            snprintf(out, out_cap, "error: invalid timezone format");
            return false;
        }
        snprintf(url, sizeof(url), "https://worldtimeapi.org/api/timezone/%s", timezone);
    } else {
        snprintf(url, sizeof(url), "https://worldtimeapi.org/api/ip");
    }

    nc_http_response resp;
    memset(&resp, 0, sizeof(resp));
    if (!nc_http_get(url, NULL, 0, &resp)) {
        snprintf(out, out_cap, "error: http request failed");
        nc_http_response_free(&resp);
        return false;
    }

    if (resp.status != 200) {
        snprintf(out, out_cap, "error: api returned status %d", resp.status);
        nc_http_response_free(&resp);
        return false;
    }

    /* Parse JSON response */
    nc_arena a;
    nc_arena_init(&a, resp.body_len * 2 + 1024);
    nc_json *root = nc_json_parse(&a, resp.body, resp.body_len);
    
    if (!root) {
        snprintf(out, out_cap, "error: invalid json response");
        nc_arena_free(&a);
        nc_http_response_free(&resp);
        return false;
    }

    nc_str dt = nc_json_str(nc_json_get(root, "datetime"), "");
    nc_str tz = nc_json_str(nc_json_get(root, "timezone"), "");
    nc_str utc_off = nc_json_str(nc_json_get(root, "utc_offset"), "");
    int day_of_week = (int)nc_json_num(nc_json_get(root, "day_of_week"), -1);
    
    const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    const char *day_str = (day_of_week >= 0 && day_of_week <= 6) ? days[day_of_week] : "Unknown";

    snprintf(out, out_cap, "%.*s %.*s (%.*s), %s", 
             NC_STR_ARG(dt), NC_STR_ARG(utc_off), NC_STR_ARG(tz), day_str);

    nc_arena_free(&a);
    nc_http_response_free(&resp);
    return true;
}

nc_tool nc_tool_get_time(void) {
    return (nc_tool){
        .def = {
            .name = "get_time",
            .description = "Get current date and time from WorldTimeAPI. Use when you need accurate current time. Optional timezone in IANA format (e.g. Europe/London, Asia/Tokyo). Omit to auto-detect from server IP.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"timezone\":{\"type\":\"string\",\"description\":\"IANA timezone (e.g. Europe/London)\"}},\"required\":[]}",
        },
        .ctx = NULL,
        .execute = get_time_execute,
        .free = NULL,
    };
}
