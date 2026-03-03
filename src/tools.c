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
