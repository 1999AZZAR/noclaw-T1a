# T1a Internal Tools Roadmap

## Priority: High

### 1. `http_fetch`
Expose the existing `nc_http_get` as a user-facing tool for direct URL fetching.
- **Why:** Eliminates Tavily dependency for simple page reads, API calls, and JSON endpoints.
- **Parameters:** `url` (required), `headers` (optional key-value pairs).
- **Returns:** Status code, response body (truncated to fit tool output buffer).
- **Notes:** Reuse existing TLS/BearSSL stack. Add a sensible max body size (e.g. 32KB) to prevent buffer overflow. Strip HTML tags for readability when content-type is text/html.

### 2. `list_dir`
Structured directory listing without shelling out.
- **Why:** Faster and safer than `shell("ls -la")`. No output parsing needed by the LLM.
- **Parameters:** `path` (required), `recursive` (optional bool, default false), `max_depth` (optional int, default 2).
- **Returns:** JSON array of entries: `{ name, type (file|dir|symlink), size, modified }`.
- **Implementation:** `opendir()` + `readdir()` + `stat()`. Keep it simple.

## Priority: Medium

### 3. `sys_info`
System health and resource monitoring.
- **Why:** Self-awareness for hardware-constrained deployments (Luckfox). Enables T1a to report its own health via `/status` or conversationally.
- **Returns:** JSON object with:
  - `hostname`
  - `uptime_seconds` (from `/proc/uptime`)
  - `memory_total_kb`, `memory_available_kb` (from `/proc/meminfo`)
  - `disk_total_kb`, `disk_available_kb` (from `statvfs()`)
  - `load_avg` (from `/proc/loadavg`)
  - `cpu_arch` (from `uname()`)
- **Parameters:** None.

### 4. `calc`
Lightweight math expression evaluator.
- **Why:** LLMs are unreliable for precise arithmetic. Tool call is more accurate than guessing.
- **Parameters:** `expression` (string, e.g. "127 * 389 + 42").
- **Supported:** `+`, `-`, `*`, `/`, `%`, parentheses, integers and floats.
- **Implementation:** Simple recursive descent parser (~80-120 lines of C). No external libs needed.
- **Returns:** Numeric result as string.

## Priority: Low (Future)

### 5. `env_get`
Read environment variables safely.
- **Why:** Useful for checking runtime config without shelling out.
- **Parameters:** `name` (required).
- **Returns:** Value string or "not set".
- **Security:** Whitelist allowed variable names to prevent leaking secrets.

### 6. `base64`
Encode/decode base64 strings.
- **Why:** Handy for quick data transformations, token inspection, and payload debugging.
- **Parameters:** `input` (required), `mode` ("encode" | "decode").
- **Implementation:** ~60 lines of C. Standard RFC 4648 table.

### 7. `hash`
Compute checksums for files or strings.
- **Why:** File integrity verification, quick dedup checks.
- **Parameters:** `input` (string or file path), `algorithm` ("md5" | "sha256").
- **Implementation:** Lightweight SHA-256 is ~200 lines of C. MD5 is even smaller.

---

## Implementation Notes
- All tools follow the existing `nc_tool` struct pattern (see `src/tools.c`).
- Register new tools in `src/commands.c` → `nc_cmd_agent()`.
- Declare constructors in `src/nc.h`.
- Keep each tool self-contained: no new external dependencies.
- Target: zero increase in binary size beyond ~8-16KB total for all new tools.
