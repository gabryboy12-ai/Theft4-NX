# Terminal MCP Server

A powerful and reliable MCP (Model Context Protocol) server for terminal interaction.

## Features

- **Create persistent terminal sessions** - Manage multiple terminal sessions
- **Run commands** - Execute commands and reliably capture output
- **Output buffering** - Maintains a rolling buffer of terminal output (up to 10,000 lines)
- **Interactive support** - Send raw input for interactive programs
- **ANSI stripping** - Clean output without escape codes
- **Session management** - List, create, and destroy sessions

## Installation

```bash
cd tools/terminal-mcp-server
npm install
npm run build
```

## Tools

### terminal_create
Create a new terminal session.
- `cwd` (optional): Working directory
- `shell` (optional): Shell to use (defaults to system shell)

### terminal_run
Run a command in an existing session and wait for output.
- `sessionId`: The terminal session ID
- `command`: The command to run
- `timeout` (optional): Timeout in ms (default: 30000)

### terminal_run_simple
Run a command in a new temporary terminal. Best for one-off commands.
- `command`: The command to run
- `cwd` (optional): Working directory
- `timeout` (optional): Timeout in ms (default: 60000)

### terminal_get_output
Get the output buffer from a terminal session.
- `sessionId`: The terminal session ID
- `lines` (optional): Number of lines to retrieve from end

### terminal_write
Write raw input to a terminal (for interactive programs).
- `sessionId`: The terminal session ID
- `input`: The input to write (supports `\r`, `\n`, `\t`, `\x03` for Ctrl+C)

### terminal_list
List all active terminal sessions.

### terminal_destroy
Destroy a terminal session.
- `sessionId`: The terminal session ID to destroy

### terminal_clear
Clear the output buffer of a terminal session.
- `sessionId`: The terminal session ID

## VS Code MCP Configuration

Add to your VS Code MCP settings (`~/Library/Application Support/Code/User/settings.json`):

```json
{
  "mcp": {
    "servers": {
      "terminal": {
        "command": "node",
        "args": ["/path/to/terminal-mcp-server/dist/index.js"]
      }
    }
  }
}
```

Or in `mcp.json`:

```json
{
  "servers": {
    "terminal": {
      "command": "node",
      "args": ["/path/to/terminal-mcp-server/dist/index.js"]
    }
  }
}
```

## License

MIT
