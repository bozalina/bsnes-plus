# bsnes-mcp

MCP server for the bsnes-plus debug API. Exposes CPU registers, memory,
breakpoints, and execution control as Claude Code tools.

## Prerequisites

- bsnes-plus running with the debugger window open
- Node.js 18+

## Setup

```bash
npm install
npm run build
```

## Claude Code configuration

Add to your Claude Code MCP config (`.mcp.json` in the project root or
`~/.config/claude/mcp.json` globally):

```json
{
  "mcpServers": {
    "bsnes": {
      "command": "node",
      "args": ["/absolute/path/to/bsnes-plus/bsnes/mcp/dist/index.js"]
    }
  }
}
```

Or to run without building first:

```json
{
  "mcpServers": {
    "bsnes": {
      "command": "npx",
      "args": ["tsx", "/absolute/path/to/bsnes-plus/bsnes/mcp/src/index.ts"]
    }
  }
}
```

## Tools

| Tool | Description |
|---|---|
| `bsnes_get_status` | Current execution state |
| `bsnes_break` | Pause execution |
| `bsnes_resume` | Resume to next break |
| `bsnes_run` | Exit debug mode |
| `bsnes_step_into` | Step one instruction |
| `bsnes_step_over` | Step over subroutine calls |
| `bsnes_step_out` | Step out of current subroutine |
| `bsnes_step_to_vblank` | Run to next VBlank |
| `bsnes_step_to_hblank` | Run to next HBlank |
| `bsnes_step_to_nmi` | Run to next NMI |
| `bsnes_step_to_irq` | Run to next IRQ |
| `bsnes_get_registers` | Read all CPU registers and flags |
| `bsnes_set_registers` | Write registers and/or flags |
| `bsnes_disassemble` | Disassemble at a SNES address |
| `bsnes_get_usage` | Per-byte execution history |
| `bsnes_read_memory` | Read from a memory bus |
| `bsnes_write_memory` | Write to a memory bus |
| `bsnes_list_breakpoints` | List all breakpoints |
| `bsnes_add_breakpoint` | Add a breakpoint |
| `bsnes_delete_breakpoint` | Remove a breakpoint by index |
| `bsnes_clear_breakpoints` | Remove all breakpoints |
