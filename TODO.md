# T1a Internal Tools Roadmap

## Priority: Low (Future)

### 1. `env_get`
Read environment variables safely.
- **Why:** Useful for checking runtime config without shelling out.
- **Parameters:** `name` (required).
- **Returns:** Value string or "not set".
- **Security:** Whitelist allowed variable names to prevent leaking secrets.

### 2. `base64`
Encode/decode base64 strings.
- **Why:** Handy for quick data transformations, token inspection, and payload debugging.
- **Parameters:** `input` (required), `mode` ("encode" | "decode").
- **Implementation:** ~60 lines of C. Standard RFC 4648 table.

### 3. `hash`
Compute checksums for files or strings.
- **Why:** File integrity verification, quick dedup checks.
- **Parameters:** `input` (string or file path), `algorithm` ("md5" | "sha256").
- **Implementation:** Lightweight SHA-256 is ~200 lines of C. MD5 is even smaller.

---

## Implementation Notes
- All tools follow the existing `nc_tool` struct pattern (see `src/tools.c`).
- Register new tools in `src/commands.c` and `src/commands_extra.c`.
- Declare constructors in `src/nc.h`.
- Keep each tool self-contained: no new external dependencies.
