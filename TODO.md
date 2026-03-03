# T1a Internal Tools Roadmap

All planned tools have been implemented.

## Implementation Notes
- All tools follow the existing `nc_tool` struct pattern (see `src/tools.c`).
- Register new tools in `src/commands.c` and `src/commands_extra.c`.
- Declare constructors in `src/nc.h`.
- Keep each tool self-contained: no new external dependencies.
