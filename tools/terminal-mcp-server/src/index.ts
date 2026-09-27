#!/usr/bin/env node
/**
 * Terminal MCP Server
 * A powerful and reliable MCP server for terminal interaction
 */

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
  Tool,
} from "@modelcontextprotocol/sdk/types.js";
import * as pty from "node-pty";
import { EventEmitter } from "events";
import stripAnsi from "strip-ansi";

// Terminal session management
interface TerminalSession {
  id: string;
  pty: pty.IPty;
  outputBuffer: string[];
  maxBufferLines: number;
  cwd: string;
  shell: string;
  createdAt: Date;
  lastActivity: Date;
  isAlive: boolean;
}

class TerminalManager extends EventEmitter {
  private sessions: Map<string, TerminalSession> = new Map();
  private defaultShell: string;
  private sessionCounter: number = 0;

  constructor() {
    super();
    // Detect default shell
    this.defaultShell = process.env.SHELL || "/bin/zsh";
  }

  createSession(cwd?: string, shell?: string): TerminalSession {
    const id = `term_${++this.sessionCounter}_${Date.now()}`;
    const sessionShell = shell || this.defaultShell;
    const sessionCwd = cwd || process.cwd();

    const ptyProcess = pty.spawn(sessionShell, [], {
      name: "xterm-256color",
      cols: 200,
      rows: 50,
      cwd: sessionCwd,
      env: {
        ...process.env,
        TERM: "xterm-256color",
        LANG: "en_US.UTF-8",
      },
    });

    const session: TerminalSession = {
      id,
      pty: ptyProcess,
      outputBuffer: [],
      maxBufferLines: 10000,
      cwd: sessionCwd,
      shell: sessionShell,
      createdAt: new Date(),
      lastActivity: new Date(),
      isAlive: true,
    };

    // Capture output
    ptyProcess.onData((data: string) => {
      session.lastActivity = new Date();
      const cleanData = stripAnsi(data);
      const lines = cleanData.split("\n");
      session.outputBuffer.push(...lines);

      // Trim buffer if too large
      if (session.outputBuffer.length > session.maxBufferLines) {
        session.outputBuffer = session.outputBuffer.slice(-session.maxBufferLines);
      }

      this.emit("output", { sessionId: id, data: cleanData });
    });

    ptyProcess.onExit(({ exitCode }) => {
      session.isAlive = false;
      this.emit("exit", { sessionId: id, exitCode });
    });

    this.sessions.set(id, session);
    return session;
  }

  getSession(id: string): TerminalSession | undefined {
    return this.sessions.get(id);
  }

  listSessions(): TerminalSession[] {
    return Array.from(this.sessions.values());
  }

  async runCommand(
    sessionId: string,
    command: string,
    options: { timeout?: number; waitForOutput?: boolean } = {}
  ): Promise<{ success: boolean; output: string; exitCode?: number }> {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return { success: false, output: `Session ${sessionId} not found` };
    }

    if (!session.isAlive) {
      return { success: false, output: `Session ${sessionId} is no longer alive` };
    }

    const timeout = options.timeout || 30000;
    const startIndex = session.outputBuffer.length;

    return new Promise((resolve) => {
      const marker = `__CMD_DONE_${Date.now()}__`;
      let timeoutHandle: NodeJS.Timeout;
      let resolved = false;

      const checkOutput = () => {
        if (resolved) return;

        const newOutput = session.outputBuffer.slice(startIndex).join("\n");
        if (newOutput.includes(marker)) {
          resolved = true;
          clearTimeout(timeoutHandle);
          // Remove the marker from output
          const cleanOutput = newOutput.replace(new RegExp(`echo ${marker}.*`), "").replace(marker, "").trim();
          resolve({ success: true, output: cleanOutput });
        }
      };

      // Set up output listener
      const outputHandler = () => checkOutput();
      this.on("output", outputHandler);

      // Set timeout
      timeoutHandle = setTimeout(() => {
        if (!resolved) {
          resolved = true;
          this.off("output", outputHandler);
          const newOutput = session.outputBuffer.slice(startIndex).join("\n");
          resolve({ success: true, output: newOutput.trim() + "\n[Command timed out]" });
        }
      }, timeout);

      // Write command with marker
      session.pty.write(`${command}; echo ${marker}\r`);

      // Also check periodically
      const interval = setInterval(() => {
        checkOutput();
        if (resolved) {
          clearInterval(interval);
          this.off("output", outputHandler);
        }
      }, 100);
    });
  }

  async runCommandSimple(
    command: string,
    cwd?: string,
    timeout?: number
  ): Promise<{ success: boolean; output: string; exitCode?: number }> {
    // Create a temporary session for this command
    const session = this.createSession(cwd);
    
    // Wait a bit for shell to initialize
    await new Promise(resolve => setTimeout(resolve, 500));
    
    const result = await this.runCommand(session.id, command, { timeout: timeout || 60000 });
    
    // Clean up session after command
    this.destroySession(session.id);
    
    return result;
  }

  getOutput(sessionId: string, lines?: number): string {
    const session = this.sessions.get(sessionId);
    if (!session) {
      return `Session ${sessionId} not found`;
    }

    const outputLines = lines
      ? session.outputBuffer.slice(-lines)
      : session.outputBuffer;

    return outputLines.join("\n");
  }

  clearOutput(sessionId: string): boolean {
    const session = this.sessions.get(sessionId);
    if (!session) return false;
    session.outputBuffer = [];
    return true;
  }

  writeInput(sessionId: string, input: string): boolean {
    const session = this.sessions.get(sessionId);
    if (!session || !session.isAlive) return false;
    session.pty.write(input);
    return true;
  }

  resize(sessionId: string, cols: number, rows: number): boolean {
    const session = this.sessions.get(sessionId);
    if (!session || !session.isAlive) return false;
    session.pty.resize(cols, rows);
    return true;
  }

  destroySession(sessionId: string): boolean {
    const session = this.sessions.get(sessionId);
    if (!session) return false;
    
    try {
      session.pty.kill();
    } catch (e) {
      // Ignore errors during kill
    }
    
    this.sessions.delete(sessionId);
    return true;
  }

  destroyAllSessions(): void {
    for (const [id] of this.sessions) {
      this.destroySession(id);
    }
  }
}

// MCP Server Setup
const terminalManager = new TerminalManager();

const tools: Tool[] = [
  {
    name: "terminal_create",
    description: "Create a new terminal session. Returns a session ID for subsequent operations.",
    inputSchema: {
      type: "object",
      properties: {
        cwd: {
          type: "string",
          description: "Working directory for the terminal (optional)",
        },
        shell: {
          type: "string",
          description: "Shell to use (e.g., /bin/zsh, /bin/bash). Defaults to system shell.",
        },
      },
    },
  },
  {
    name: "terminal_run",
    description: "Run a command in an existing terminal session and wait for output.",
    inputSchema: {
      type: "object",
      properties: {
        sessionId: {
          type: "string",
          description: "The terminal session ID",
        },
        command: {
          type: "string",
          description: "The command to run",
        },
        timeout: {
          type: "number",
          description: "Timeout in milliseconds (default: 30000)",
        },
      },
      required: ["sessionId", "command"],
    },
  },
  {
    name: "terminal_run_simple",
    description: "Run a command in a new temporary terminal and return the output. Best for one-off commands.",
    inputSchema: {
      type: "object",
      properties: {
        command: {
          type: "string",
          description: "The command to run",
        },
        cwd: {
          type: "string",
          description: "Working directory (optional)",
        },
        timeout: {
          type: "number",
          description: "Timeout in milliseconds (default: 60000)",
        },
      },
      required: ["command"],
    },
  },
  {
    name: "terminal_get_output",
    description: "Get the output buffer from a terminal session.",
    inputSchema: {
      type: "object",
      properties: {
        sessionId: {
          type: "string",
          description: "The terminal session ID",
        },
        lines: {
          type: "number",
          description: "Number of lines to retrieve (from end). Omit for all output.",
        },
      },
      required: ["sessionId"],
    },
  },
  {
    name: "terminal_write",
    description: "Write raw input to a terminal (for interactive programs).",
    inputSchema: {
      type: "object",
      properties: {
        sessionId: {
          type: "string",
          description: "The terminal session ID",
        },
        input: {
          type: "string",
          description: "The input to write (use \\r for Enter, \\x03 for Ctrl+C)",
        },
      },
      required: ["sessionId", "input"],
    },
  },
  {
    name: "terminal_list",
    description: "List all active terminal sessions.",
    inputSchema: {
      type: "object",
      properties: {},
    },
  },
  {
    name: "terminal_destroy",
    description: "Destroy a terminal session.",
    inputSchema: {
      type: "object",
      properties: {
        sessionId: {
          type: "string",
          description: "The terminal session ID to destroy",
        },
      },
      required: ["sessionId"],
    },
  },
  {
    name: "terminal_clear",
    description: "Clear the output buffer of a terminal session.",
    inputSchema: {
      type: "object",
      properties: {
        sessionId: {
          type: "string",
          description: "The terminal session ID",
        },
      },
      required: ["sessionId"],
    },
  },
];

const server = new Server(
  {
    name: "terminal-mcp-server",
    version: "1.0.0",
  },
  {
    capabilities: {
      tools: {},
    },
  }
);

// Handle tool listing
server.setRequestHandler(ListToolsRequestSchema, async () => {
  return { tools };
});

// Handle tool execution
server.setRequestHandler(CallToolRequestSchema, async (request) => {
  const { name, arguments: args } = request.params;

  try {
    switch (name) {
      case "terminal_create": {
        const session = terminalManager.createSession(
          args?.cwd as string | undefined,
          args?.shell as string | undefined
        );
        // Wait for shell to initialize
        await new Promise(resolve => setTimeout(resolve, 300));
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                success: true,
                sessionId: session.id,
                cwd: session.cwd,
                shell: session.shell,
                message: "Terminal session created successfully",
              }, null, 2),
            },
          ],
        };
      }

      case "terminal_run": {
        const sessionId = args?.sessionId as string;
        const command = args?.command as string;
        const timeout = args?.timeout as number | undefined;

        const result = await terminalManager.runCommand(sessionId, command, { timeout });
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                success: result.success,
                output: result.output,
                exitCode: result.exitCode,
              }, null, 2),
            },
          ],
        };
      }

      case "terminal_run_simple": {
        const command = args?.command as string;
        const cwd = args?.cwd as string | undefined;
        const timeout = args?.timeout as number | undefined;

        const result = await terminalManager.runCommandSimple(command, cwd, timeout);
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                success: result.success,
                output: result.output,
              }, null, 2),
            },
          ],
        };
      }

      case "terminal_get_output": {
        const sessionId = args?.sessionId as string;
        const lines = args?.lines as number | undefined;

        const output = terminalManager.getOutput(sessionId, lines);
        return {
          content: [
            {
              type: "text",
              text: output,
            },
          ],
        };
      }

      case "terminal_write": {
        const sessionId = args?.sessionId as string;
        const input = args?.input as string;

        // Handle escape sequences
        const processedInput = input
          .replace(/\\r/g, "\r")
          .replace(/\\n/g, "\n")
          .replace(/\\t/g, "\t")
          .replace(/\\x03/g, "\x03"); // Ctrl+C

        const success = terminalManager.writeInput(sessionId, processedInput);
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({ success }, null, 2),
            },
          ],
        };
      }

      case "terminal_list": {
        const sessions = terminalManager.listSessions();
        const sessionInfo = sessions.map(s => ({
          id: s.id,
          cwd: s.cwd,
          shell: s.shell,
          isAlive: s.isAlive,
          createdAt: s.createdAt.toISOString(),
          lastActivity: s.lastActivity.toISOString(),
          outputLines: s.outputBuffer.length,
        }));
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({ sessions: sessionInfo }, null, 2),
            },
          ],
        };
      }

      case "terminal_destroy": {
        const sessionId = args?.sessionId as string;
        const success = terminalManager.destroySession(sessionId);
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                success,
                message: success ? "Session destroyed" : "Session not found",
              }, null, 2),
            },
          ],
        };
      }

      case "terminal_clear": {
        const sessionId = args?.sessionId as string;
        const success = terminalManager.clearOutput(sessionId);
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({
                success,
                message: success ? "Output buffer cleared" : "Session not found",
              }, null, 2),
            },
          ],
        };
      }

      default:
        return {
          content: [
            {
              type: "text",
              text: JSON.stringify({ error: `Unknown tool: ${name}` }),
            },
          ],
          isError: true,
        };
    }
  } catch (error) {
    const errorMessage = error instanceof Error ? error.message : String(error);
    return {
      content: [
        {
          type: "text",
          text: JSON.stringify({ error: errorMessage }),
        },
      ],
      isError: true,
    };
  }
});

// Cleanup on exit
process.on("SIGINT", () => {
  terminalManager.destroyAllSessions();
  process.exit(0);
});

process.on("SIGTERM", () => {
  terminalManager.destroyAllSessions();
  process.exit(0);
});

// Start server
async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
  console.error("Terminal MCP Server running on stdio");
}

main().catch((error) => {
  console.error("Fatal error:", error);
  process.exit(1);
});
