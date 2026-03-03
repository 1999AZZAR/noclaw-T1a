#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <ctype.h>

/* ── Helper: extract string from tool args JSON ───────────────── */

static bool extract_json_string(const char *json, const char *key,
                                char *out, size_t out_cap) {
    nc_arena a;
    nc_arena_init(&a, strlen(json) * 2 + 1024);
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

static nc_json *parse_args_root(const char *json, nc_arena *a) {
    nc_arena_init(a, strlen(json) * 2 + 1024);
    nc_json *root = nc_json_parse(a, json, strlen(json));
    return (root && root->type == NC_JSON_OBJECT) ? root : NULL;
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

/* ── Time tool (local system clock) ──────────────────────────── */

static bool get_time_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    char tz_arg[64];
    tz_arg[0] = '\0';
    extract_json_string(args_json, "timezone", tz_arg, sizeof(tz_arg));

    /* Sanitize and apply timezone if provided */
    char *old_tz = NULL;
    if (tz_arg[0]) {
        for (char *p = tz_arg; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '/' || *p == '_' || *p == '-' || *p == '+')) {
                snprintf(out, out_cap, "error: invalid characters in timezone");
                return false;
            }
        }
        const char *cur = getenv("TZ");
        if (cur) old_tz = strdup(cur);
        setenv("TZ", tz_arg, 1);
        tzset();
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char date_buf[64], time_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &tm);

    const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    char tz_name[32];
    strftime(tz_name, sizeof(tz_name), "%Z", &tm);

    long utc_off = tm.tm_gmtoff;
    int off_h = (int)(utc_off / 3600);
    int off_m = abs((int)((utc_off % 3600) / 60));

    snprintf(out, out_cap, "%s %s %+03d:%02d (%s), %s",
             date_buf, time_buf, off_h, off_m,
             tz_arg[0] ? tz_arg : tz_name, days[tm.tm_wday]);

    /* Restore original TZ */
    if (tz_arg[0]) {
        if (old_tz) { setenv("TZ", old_tz, 1); free(old_tz); }
        else unsetenv("TZ");
        tzset();
    }
    return true;
}

nc_tool nc_tool_get_time(void) {
    return (nc_tool){
        .def = {
            .name = "get_time",
            .description = "Get current date and time from the system clock. Use when you need accurate current time. Optional timezone in IANA format (e.g. Asia/Jakarta, Europe/London). Defaults to server local time.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"timezone\":{\"type\":\"string\",\"description\":\"IANA timezone (e.g. Asia/Jakarta)\"}},\"required\":[]}",
        },
        .ctx = NULL,
        .execute = get_time_execute,
        .free = NULL,
    };
}

/* ── Sys info tool ────────────────────────────────────────────── */

static long read_proc_long(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    size_t key_len = strlen(key);
    long val = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            char *p = line + key_len + 1;
            while (*p == ' ') p++;
            val = (long)strtol(p, NULL, 10);
            break;
        }
    }
    fclose(f);
    return val;
}

static bool sys_info_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    (void)args_json;

    char hostname[256] = "unknown";
    gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';

    double uptime_sec = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &uptime_sec) != 1) uptime_sec = 0;
        fclose(f);
    }

    long mem_total = read_proc_long("/proc/meminfo", "MemTotal");
    long mem_avail = read_proc_long("/proc/meminfo", "MemAvailable");
    if (mem_avail < 0) mem_avail = read_proc_long("/proc/meminfo", "MemFree");

    double load1 = 0, load5 = 0, load15 = 0;
    f = fopen("/proc/loadavg", "r");
    if (f) {
        fscanf(f, "%lf %lf %lf", &load1, &load5, &load15);
        fclose(f);
    }

    unsigned long disk_total = 0, disk_avail = 0;
    struct statvfs vfs;
    if (statvfs(".", &vfs) == 0) {
        disk_total = vfs.f_blocks * vfs.f_frsize / 1024;
        disk_avail = vfs.f_bavail * vfs.f_frsize / 1024;
    }

    struct utsname uts;
    char cpu_arch[64] = "unknown";
    if (uname(&uts) == 0) {
        nc_strlcpy(cpu_arch, uts.machine, sizeof(cpu_arch));
    }

    int n = snprintf(out, out_cap,
        "{\"hostname\":\"%s\",\"uptime_seconds\":%.0f,\"memory_total_kb\":%ld,\"memory_available_kb\":%ld,"
        "\"disk_total_kb\":%lu,\"disk_available_kb\":%lu,\"load_avg\":\"%.2f %.2f %.2f\",\"cpu_arch\":\"%s\"}",
        hostname, uptime_sec, mem_total, mem_avail, disk_total, disk_avail, load1, load5, load15, cpu_arch);

    return n > 0 && (size_t)n < out_cap;
}

nc_tool nc_tool_sys_info(void) {
    return (nc_tool){
        .def = {
            .name = "sys_info",
            .description = "System health and resource info: hostname, uptime, memory, disk, load average, CPU arch. For self-awareness on constrained hardware.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        },
        .ctx = NULL,
        .execute = sys_info_execute,
        .free = NULL,
    };
}

/* ── Calc tool (recursive descent) ──────────────────────────────── */

typedef struct {
    const char *s;
    const char *p;
} calc_ctx;

static void calc_skip(calc_ctx *c) {
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static double calc_parse_expr(calc_ctx *c, bool *ok);

static double calc_parse_primary(calc_ctx *c, bool *ok) {
    calc_skip(c);
    if (*c->p == '(') {
        c->p++;
        double v = calc_parse_expr(c, ok);
        calc_skip(c);
        if (*c->p != ')') { *ok = false; return 0; }
        c->p++;
        return v;
    }
    if (*c->p == '+' || *c->p == '-') {
        int s = (*c->p == '-') ? -1 : 1;
        c->p++;
        double v = calc_parse_primary(c, ok);
        return *ok ? (double)s * v : 0;
    }
    char *end;
    double v = strtod(c->p, &end);
    if (end == c->p) { *ok = false; return 0; }
    c->p = end;
    *ok = true;
    return v;
}

static double calc_parse_term(calc_ctx *c, bool *ok) {
    double v = calc_parse_primary(c, ok);
    if (!*ok) return 0;
    for (;;) {
        calc_skip(c);
        char op = *c->p;
        if (op == '*' || op == '/' || op == '%') {
            c->p++;
            double r = calc_parse_primary(c, ok);
            if (!*ok) return 0;
            if (op == '*') v *= r;
            else if (op == '/') {
                if (r == 0) { *ok = false; return 0; }
                v /= r;
            } else {
                if (r == 0) { *ok = false; return 0; }
                v = (double)((long)v % (long)r);
            }
        } else break;
    }
    return v;
}

static double calc_parse_expr(calc_ctx *c, bool *ok) {
    double v = calc_parse_term(c, ok);
    if (!*ok) return 0;
    for (;;) {
        calc_skip(c);
        char op = *c->p;
        if (op == '+') {
            c->p++;
            double r = calc_parse_term(c, ok);
            if (!*ok) return 0;
            v += r;
        } else if (op == '-') {
            c->p++;
            double r = calc_parse_term(c, ok);
            if (!*ok) return 0;
            v -= r;
        } else break;
    }
    return v;
}

static bool calc_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char expr[1024];
    if (!extract_json_string(args_json, "expression", expr, sizeof(expr))) {
        nc_strlcpy(out, "error: missing 'expression'", out_cap);
        return false;
    }
    /* Reject anything that could be shell injection */
    for (char *p = expr; *p; p++) {
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '+' && *p != '-' && *p != '*' &&
            *p != '/' && *p != '%' && *p != '(' && *p != ')' && *p != ' ' && *p != '\t') {
            snprintf(out, out_cap, "error: invalid character in expression");
            return false;
        }
    }
    calc_ctx c = { .s = expr, .p = expr };
    bool ok = false;
    double result = calc_parse_expr(&c, &ok);
    calc_skip(&c);
    if (!ok || *c.p != '\0') {
        snprintf(out, out_cap, "error: invalid expression");
        return false;
    }
    /* Check for integer result to avoid ".0" when not needed */
    long li = (long)result;
    if (result == (double)li) {
        snprintf(out, out_cap, "%ld", li);
    } else {
        snprintf(out, out_cap, "%.16g", result);
    }
    return true;
}

nc_tool nc_tool_calc(void) {
    return (nc_tool){
        .def = {
            .name = "calc",
            .description = "Evaluate a math expression. Supports +, -, *, /, %, parentheses, integers and floats. More accurate than LLM arithmetic.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\",\"description\":\"Math expression e.g. 127 * 389 + 42\"}},\"required\":[\"expression\"]}",
        },
        .ctx = NULL,
        .execute = calc_execute,
        .free = NULL,
    };
}

/* ── HTTP fetch tool ──────────────────────────────────────────── */

#define HTTP_FETCH_MAX_BODY 32768

static void strip_html_tags(const char *in, char *out, size_t out_cap) {
    size_t j = 0;
    bool in_tag = false;
    for (const char *p = in; *p && j < out_cap - 1; p++) {
        if (*p == '<') in_tag = true;
        else if (*p == '>') in_tag = false;
        else if (!in_tag) {
            if (*p == '\r') continue;
            if (*p == '\n' && j > 0 && out[j - 1] == '\n') continue;
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

static bool http_fetch_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char url[2048];
    if (!extract_json_string(args_json, "url", url, sizeof(url))) {
        nc_strlcpy(out, "error: missing 'url'", out_cap);
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        nc_strlcpy(out, "error: url must be http or https", out_cap);
        return false;
    }

    const char *hdrs[8];
    char hdr_bufs[8][256];
    int hdr_count = 0;
    nc_arena a;
    nc_json *root = parse_args_root(args_json, &a);
    if (root) {
        nc_json *headers = nc_json_get(root, "headers");
        if (headers && headers->type == NC_JSON_OBJECT) {
            for (int i = 0; i < headers->object.count && hdr_count < 8; i++) {
                nc_str k = headers->object.keys[i];
                nc_str v = nc_json_str(&headers->object.vals[i], "");
                int n = snprintf(hdr_bufs[hdr_count], sizeof(hdr_bufs[0]),
                    "%.*s: %.*s", (int)k.len, k.ptr, (int)v.len, v.ptr);
                if (n > 0 && (size_t)n < sizeof(hdr_bufs[0])) {
                    hdrs[hdr_count] = hdr_bufs[hdr_count];
                    hdr_count++;
                }
            }
        }
    }
    nc_arena_free(&a);

    nc_http_response resp;
    memset(&resp, 0, sizeof(resp));
    if (!nc_http_get(url, hdr_count ? hdrs : NULL, hdr_count, &resp)) {
        nc_strlcpy(out, "error: http request failed", out_cap);
        nc_http_response_free(&resp);
        return false;
    }

    size_t body_limit = resp.body_len > HTTP_FETCH_MAX_BODY ? HTTP_FETCH_MAX_BODY : resp.body_len;
    int off = snprintf(out, out_cap, "status: %d\n\n", resp.status);

    bool is_html = (strstr(resp.body, "<html") != NULL || strstr(resp.body, "<HTML") != NULL ||
                    strstr(resp.body, "<!DOCTYPE") != NULL);
    size_t avail = out_cap > (size_t)off ? out_cap - (size_t)off - 1 : 0;
    if (is_html && avail > 0) {
        char stripped[HTTP_FETCH_MAX_BODY + 1];
        strip_html_tags(resp.body, stripped, sizeof(stripped));
        size_t slen = strlen(stripped);
        if (slen > avail) slen = avail;
        memcpy(out + off, stripped, slen);
        out[off + slen] = '\0';
    } else if (avail > 0) {
        size_t cplen = body_limit < avail ? body_limit : avail;
        memcpy(out + off, resp.body, cplen);
        out[off + cplen] = '\0';
    }
    nc_http_response_free(&resp);
    return true;
}

nc_tool nc_tool_http_fetch(void) {
    return (nc_tool){
        .def = {
            .name = "http_fetch",
            .description = "Fetch a URL and return status and body. Supports http/https. Optional headers as object. HTML content is stripped for readability. Body truncated to 32KB.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"},\"headers\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"string\"}}},\"required\":[\"url\"]}",
        },
        .ctx = NULL,
        .execute = http_fetch_execute,
        .free = NULL,
    };
}

/* ── List dir tool ───────────────────────────────────────────── */

static int list_dir_append(const char *base, int depth, int max_depth,
                           bool recursive, char *out, size_t out_cap, size_t *off) {
    char path[2048];
    if (base[0]) nc_strlcpy(path, base, sizeof(path));
    else nc_strlcpy(path, ".", sizeof(path));

    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;

        char full[4096];
        int fn = snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (fn <= 0 || (size_t)fn >= sizeof(full)) continue;

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        const char *typ = "file";
        if (S_ISDIR(st.st_mode)) typ = "dir";
        else if (S_ISLNK(st.st_mode)) typ = "symlink";

        char mtime_buf[32];
        struct tm tm;
        localtime_r(&st.st_mtime, &tm);
        strftime(mtime_buf, sizeof(mtime_buf), "%Y-%m-%dT%H:%M:%S", &tm);

        /* Escape name for JSON (backslash, quote) */
        char name_esc[512];
        size_t ne = 0;
        for (const char *p = e->d_name; *p && ne < sizeof(name_esc) - 2; p++) {
            if (*p == '\\' || *p == '"') name_esc[ne++] = '\\';
            if (*p >= 0x20) name_esc[ne++] = *p;
        }
        name_esc[ne] = '\0';

        int n = snprintf(out + *off, out_cap - *off,
            "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"modified\":\"%s\"}",
            *off > 1 ? "," : "", name_esc, typ, (long)st.st_size, mtime_buf);
        if (n <= 0 || (size_t)(*off + n) >= out_cap) break;
        *off += n;

        if (recursive && S_ISDIR(st.st_mode) && depth < max_depth) {
            list_dir_append(full, depth + 1, max_depth, recursive, out, out_cap, off);
        }
    }
    closedir(d);
    return 0;
}

static bool list_dir_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];
    if (!extract_json_string(args_json, "path", path, sizeof(path))) {
        nc_strlcpy(out, "error: missing 'path'", out_cap);
        return false;
    }

    nc_arena a;
    nc_json *root = parse_args_root(args_json, &a);
    bool recursive = false;
    int max_depth = 2;
    if (root) {
        recursive = nc_json_bool(nc_json_get(root, "recursive"), false);
        max_depth = (int)nc_json_num(nc_json_get(root, "max_depth"), 2);
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 8) max_depth = 8;
        nc_arena_free(&a);
    }

    char full_path[2048];
    if (path[0] == '/') {
        nc_strlcpy(full_path, path, sizeof(full_path));
    } else {
        nc_path_join(full_path, sizeof(full_path), cfg->workspace_dir, path);
    }

    if (strstr(full_path, "..")) {
        nc_strlcpy(out, "error: path traversal not allowed", out_cap);
        return false;
    }

    out[0] = '[';
    out[1] = '\0';
    size_t off = 1;
    list_dir_append(full_path, 0, max_depth, recursive, out, out_cap, &off);
    if (off + 2 < out_cap) { out[off++] = ']'; out[off] = '\0'; }
    return true;
}

nc_tool nc_tool_list_dir(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "list_dir",
            .description = "List directory contents. Returns JSON array of {name, type, size, modified}. Faster than shell ls.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"recursive\":{\"type\":\"boolean\"},\"max_depth\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
        },
        .ctx = (void *)cfg,
        .execute = list_dir_execute,
        .free = NULL,
    };
}

/* ── Env get tool ─────────────────────────────────────────────── */

static const char *const env_whitelist[] = {
    "PATH", "HOME", "USER", "LOGNAME", "SHELL", "PWD", "TERM",
    "LANG", "LC_ALL", "LC_CTYPE", "EDITOR", "VISUAL", "XDG_CONFIG_HOME",
    "XDG_DATA_HOME", "USERPROFILE", "TMPDIR", "TEMP", "TMP",
    NULL
};

static bool env_allowed(const char *name) {
    if (strncmp(name, "NOCLAW_", 7) == 0) return true;
    for (int i = 0; env_whitelist[i]; i++) {
        if (strcmp(name, env_whitelist[i]) == 0) return true;
    }
    return false;
}

static bool env_get_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char name[128];
    if (!extract_json_string(args_json, "name", name, sizeof(name))) {
        nc_strlcpy(out, "error: missing 'name'", out_cap);
        return false;
    }
    if (!env_allowed(name)) {
        nc_strlcpy(out, "error: variable not whitelisted", out_cap);
        return false;
    }
    const char *val = getenv(name);
    if (!val) {
        nc_strlcpy(out, "not set", out_cap);
        return true;
    }
    size_t len = strlen(val);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, val, len);
    out[len] = '\0';
    return true;
}

nc_tool nc_tool_env_get(void) {
    return (nc_tool){
        .def = {
            .name = "env_get",
            .description = "Read allowed environment variables. Whitelist: PATH, HOME, USER, NOCLAW_*, etc. Returns 'not set' if unset.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}",
        },
        .ctx = NULL,
        .execute = env_get_execute,
        .free = NULL,
    };
}

/* ── Base64 tool ──────────────────────────────────────────────── */

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static bool base64_encode(const char *in, size_t in_len, char *out, size_t out_cap) {
    size_t out_len = 4 * ((in_len + 2) / 3);
    if (out_len >= out_cap) return false;
    size_t j = 0;
    for (size_t i = 0; i + 2 < in_len; i += 3) {
        unsigned a = (unsigned char)in[i] << 16 | (unsigned char)in[i+1] << 8 | (unsigned char)in[i+2];
        out[j++] = b64_chars[(a >> 18) & 63];
        out[j++] = b64_chars[(a >> 12) & 63];
        out[j++] = b64_chars[(a >> 6) & 63];
        out[j++] = b64_chars[a & 63];
    }
    size_t rem = in_len % 3;
    if (rem == 0) { out[j] = '\0'; return true; }
    unsigned a = (unsigned char)in[in_len - rem] << 16;
    if (rem == 2) a |= (unsigned char)in[in_len - 1] << 8;
    out[j++] = b64_chars[(a >> 18) & 63];
    out[j++] = b64_chars[(a >> 12) & 63];
    out[j++] = (rem == 1) ? '=' : b64_chars[(a >> 6) & 63];
    out[j++] = '=';
    out[j] = '\0';
    return true;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(const char *in, size_t in_len, char *out, size_t out_cap, size_t *out_len) {
    while (in_len > 0 && in[in_len-1] == '=') in_len--;
    size_t decoded = (in_len * 3) / 4;
    if (decoded >= out_cap) return false;
    size_t j = 0;
    for (size_t i = 0; i + 3 < in_len; i += 4) {
        int a = b64_decode_char(in[i]), b = b64_decode_char(in[i+1]),
            c = b64_decode_char(in[i+2]), d = b64_decode_char(in[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        unsigned v = (a << 18) | (b << 12) | (c << 6) | d;
        out[j++] = (char)(v >> 16);
        out[j++] = (char)(v >> 8);
        out[j++] = (char)v;
    }
    if (in_len % 4 == 2) {
        int a = b64_decode_char(in[in_len-2]), b = b64_decode_char(in[in_len-1]);
        if (a < 0 || b < 0) return false;
        out[j++] = (char)((a << 2) | (b >> 4));
    } else if (in_len % 4 == 3) {
        int a = b64_decode_char(in[in_len-3]), b = b64_decode_char(in[in_len-2]), c = b64_decode_char(in[in_len-1]);
        if (a < 0 || b < 0 || c < 0) return false;
        unsigned v = (a << 18) | (b << 12) | (c << 6);
        out[j++] = (char)(v >> 16);
        out[j++] = (char)(v >> 8);
    }
    *out_len = j;
    out[j] = '\0';
    return true;
}

static bool base64_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char input[16384];
    char mode[16];
    if (!extract_json_string(args_json, "input", input, sizeof(input))) {
        nc_strlcpy(out, "error: missing 'input'", out_cap);
        return false;
    }
    if (!extract_json_string(args_json, "mode", mode, sizeof(mode))) {
        nc_strlcpy(out, "error: missing 'mode'", out_cap);
        return false;
    }
    size_t in_len = strlen(input);
    if (strcmp(mode, "encode") == 0) {
        return base64_encode(input, in_len, out, out_cap);
    }
    if (strcmp(mode, "decode") == 0) {
        size_t dec_len;
        if (!base64_decode(input, in_len, out, out_cap, &dec_len)) {
            nc_strlcpy(out, "error: invalid base64 or buffer overflow", out_cap);
            return false;
        }
        return true;
    }
    nc_strlcpy(out, "error: mode must be 'encode' or 'decode'", out_cap);
    return false;
}

nc_tool nc_tool_base64(void) {
    return (nc_tool){
        .def = {
            .name = "base64",
            .description = "Encode or decode base64 strings. Mode: encode | decode.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"}},\"required\":[\"input\",\"mode\"]}",
        },
        .ctx = NULL,
        .execute = base64_execute,
        .free = NULL,
    };
}

/* ── Hash tool (pure C MD5/SHA-256) ───────────────────────────── */

static void hash_to_hex(const unsigned char *bin, size_t len, char *out, size_t out_cap) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len && (size_t)(i * 2 + 2) < out_cap; i++) {
        out[i*2] = hex[bin[i] >> 4];
        out[i*2+1] = hex[bin[i] & 15];
    }
    out[len * 2] = '\0';
}

/* MD5: RFC 1321, compact implementation */
#define F(x,y,z) (((x)&(y))|((~(x))&(z)))
#define G(x,y,z) (((x)&(z))|((y)&(~(z))))
#define H(x,y,z) ((x)^(y)^(z))
#define I(x,y,z) ((y)^((x)|(~(z))))
#define RL(x,n) (((x)<<(n))|((x)>>(32-(n))))

static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static void md5_transform(uint32_t *s, const unsigned char *block) {
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)block[i*4] | ((uint32_t)block[i*4+1]<<8) | ((uint32_t)block[i*4+2]<<16) | ((uint32_t)block[i*4+3]<<24);
    static const int rot[] = {7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = F(b,c,d); g = i; }
        else if (i < 32) { f = G(b,c,d); g = (5*i+1)%16; }
        else if (i < 48) { f = H(b,c,d); g = (3*i+5)%16; }
        else { f = I(b,c,d); g = (7*i)%16; }
        uint32_t t = a + f + K[i] + x[g];
        t = RL(t, rot[i%16]);
        uint32_t nb = b + t;
        a = d; d = c; c = b; b = nb;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

static void md5_hash(const void *data, size_t len, unsigned char *out) {
    uint32_t s[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const unsigned char *p = (const unsigned char *)data;
    uint64_t bits = len * 8;
    while (len >= 64) { md5_transform(s, p); p += 64; len -= 64; }
    unsigned char block[128];
    memcpy(block, p, len);
    block[len++] = 0x80;
    if (len > 56) { while (len < 64) block[len++] = 0; md5_transform(s, block); memset(block, 0, 56); len = 0; }
    else while (len < 56) block[len++] = 0;
    for (int i = 0; i < 8; i++) block[56+i] = (unsigned char)(bits >> (i*8));
    md5_transform(s, block);
    for (int i = 0; i < 4; i++) {
        out[i*4] = (unsigned char)(s[i]); out[i*4+1] = (unsigned char)(s[i]>>8);
        out[i*4+2] = (unsigned char)(s[i]>>16); out[i*4+3] = (unsigned char)(s[i]>>24);
    }
}

/* SHA-256: FIPS 180-2, compact implementation */
static const uint32_t ShaK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t *h, const unsigned char *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i*4]<<24 | (uint32_t)block[i*4+1]<<16 | (uint32_t)block[i*4+2]<<8 | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = k + s1 + ch + ShaK[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_hash(const void *data, size_t len, unsigned char *out) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const unsigned char *p = (const unsigned char *)data;
    uint64_t bits = len * 8;
    while (len >= 64) { sha256_transform(h, p); p += 64; len -= 64; }
    unsigned char block[128];
    memcpy(block, p, len);
    block[len++] = 0x80;
    if (len > 56) { while (len < 64) block[len++] = 0; sha256_transform(h, block); memset(block, 0, 56); len = 0; }
    else while (len < 56) block[len++] = 0;
    for (int i = 7; i >= 0; i--) block[63-i] = (unsigned char)(bits >> (i*8));
    sha256_transform(h, block);
    for (int i = 0; i < 8; i++)
        for (int j = 3; j >= 0; j--)
            out[i*4 + (3-j)] = (unsigned char)(h[i] >> (j*8));
}

static bool hash_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    (void)self;
    char input[4096];
    char algorithm[16];
    if (!extract_json_string(args_json, "input", input, sizeof(input))) {
        nc_strlcpy(out, "error: missing 'input'", out_cap);
        return false;
    }
    if (!extract_json_string(args_json, "algorithm", algorithm, sizeof(algorithm))) {
        nc_strlcpy(out, "error: missing 'algorithm'", out_cap);
        return false;
    }

    const char *data;
    size_t data_len;
    char *file_buf = NULL;

    if (nc_file_exists(input)) {
        size_t len;
        file_buf = nc_read_file(input, &len);
        if (!file_buf) {
            snprintf(out, out_cap, "error: cannot read file %s", input);
            return false;
        }
        if (len > 1048576) {
            free(file_buf);
            nc_strlcpy(out, "error: file too large (max 1MB)", out_cap);
            return false;
        }
        data = file_buf;
        data_len = len;
    } else {
        data = input;
        data_len = strlen(input);
    }

    bool ok = false;
    if (strcmp(algorithm, "md5") == 0) {
        unsigned char digest[16];
        md5_hash(data, data_len, digest);
        hash_to_hex(digest, 16, out, out_cap);
        ok = true;
    } else if (strcmp(algorithm, "sha256") == 0) {
        unsigned char digest[32];
        sha256_hash(data, data_len, digest);
        hash_to_hex(digest, 32, out, out_cap);
        ok = true;
    } else {
        nc_strlcpy(out, "error: algorithm must be 'md5' or 'sha256'", out_cap);
    }

    if (file_buf) free(file_buf);
    return ok;
}

nc_tool nc_tool_hash(void) {
    return (nc_tool){
        .def = {
            .name = "hash",
            .description = "Compute MD5 or SHA-256 of a string or file. Input: text or file path.",
            .parameters_json = "{\"type\":\"object\",\"properties\":{\"input\":{\"type\":\"string\"},\"algorithm\":{\"type\":\"string\"}},\"required\":[\"input\",\"algorithm\"]}",
        },
        .ctx = NULL,
        .execute = hash_execute,
        .free = NULL,
    };
}
