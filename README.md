# T1a: The Minimalist Command Unit

T1a is an ultra-lightweight, autonomous AI agent built in pure C11. Based on the `noclaw` architecture, it is designed for maximum efficiency with a near-zero resource footprint. T1a serves as a specialized command unit alongside Mema to provide focused, high-performance assistance within the Azzar Budiyanto ecosystem.

## Key Features

- **Minimalist Footprint**: ~80KB binary size with sub-1MB peak RSS.
- **Pure C11**: Zero runtime dependencies (compiled against musl/libc and BearSSL).
- **Native MCP Support**: Integrated Model Context Protocol (MCP) client for extensible tool capabilities.
- **Dynamic Context**: Loads identity and soul from `SOUL.md`, `USER.md`, and `IDENTITY.md` at runtime.
- **Telegram Native**: Optimized for Telegram Bot API with real-time typing status and Markdown support.
- **Knowledge Graph Memory**: Persistent, structured memory management via the MCP Memory server.

## Architecture

T1a utilizes a function-pointer vtable architecture for providers, channels, and tools, allowing for extreme modularity without the overhead of heavy abstractions.

- **Providers**: Supports OpenAI-compatible APIs (OpenRouter, etc.) and Anthropic.
- **Channels**: Native Telegram and CLI support.
- **Tools**: Built-in shell execution, filesystem I/O, and MCP proxying.

## Quick Start

### Build
```bash
make release
```

### Configuration
Configuration is managed via `~/.noclaw/config.json` and `~/.noclaw/mcp.json`.

### Execution
```bash
# Start in Telegram mode
./noclaw agent --channel telegram

# Interactive CLI mode
./noclaw agent
```

## Standards & Philosophy

- **Efficiency First**: No wasted cycles, no unnecessary bloat.
- **Pragmatic Design**: Built to solve problems with the least amount of code possible.
- **Security**: Strict workspace scoping and input validation.

---
"Wong edan mah ajaib."
