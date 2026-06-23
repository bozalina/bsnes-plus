import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { spawn } from "child_process";

const BASE_URL = process.env.BSNES_API_URL ?? "http://127.0.0.1:5744";

async function api(
  method: "GET" | "POST" | "PUT" | "DELETE",
  path: string,
  body?: unknown
): Promise<unknown> {
  const url = `${BASE_URL}${path}`;
  const init: RequestInit = {
    method,
    headers: { "Content-Type": "application/json" },
  };
  if (body !== undefined) init.body = JSON.stringify(body);

  let res: Response;
  try {
    res = await fetch(url, init);
  } catch (e) {
    throw new Error(
      `Cannot reach bsnes at ${BASE_URL}. ` +
      "Ensure bsnes-plus is running with the debugger window open."
    );
  }

  const json = await res.json();
  if (!res.ok) {
    const msg = (json as any).message ?? (json as any).error ?? res.statusText;
    throw new Error(`bsnes API error ${res.status}: ${msg}`);
  }
  return json;
}

const server = new McpServer({
  name: "bsnes-mcp",
  version: "1.0.0",
});

async function isBsnesRunning(): Promise<boolean> {
  try {
    const res = await fetch(`${BASE_URL}/status`,
      { signal: AbortSignal.timeout(1000) });
    return res.status !== 0;
  } catch {
    return false;
  }
}

server.tool(
  "start_bsnes",
  "Launch bsnes-plus if it is not already running. " +
  "Waits up to 15 seconds for the debug server to become reachable. " +
  "Requires the BSNES_PATH environment variable to be set to the path " +
  "of the bsnes-plus executable. Launches with --show-debugger so the " +
  "debugger window is visible immediately.",
  {},
  async () => {
    if (await isBsnesRunning())
      return { content: [{ type: "text",
        text: "bsnes-plus is already running." }] };

    const bsnesPath = process.env.BSNES_PATH;
    if (!bsnesPath)
      throw new Error(
        "BSNES_PATH environment variable is not set. " +
        "Set it to the path of the bsnes-plus executable."
      );

    spawn(bsnesPath, ["--show-debugger"], { detached: true, stdio: "ignore" })
      .unref();

    for (let i = 0; i < 30; i++) {
      await new Promise(r => setTimeout(r, 500));
      if (await isBsnesRunning())
        return { content: [{ type: "text",
          text: "bsnes-plus launched and debug server is reachable." }] };
    }

    throw new Error(
      "bsnes-plus was launched but the debug server did not become " +
      "reachable within 15 seconds. Check that the executable path is " +
      "correct and that this is a debugger build."
    );
  }
);

// Status and execution control

server.tool(
  "bsnes_get_status",
  "Get current emulator state: whether it is paused or running, what caused " +
  "the last break, and CPU registers if paused. Safe to call at any time.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("GET", "/status"), null, 2) }]
  })
);

server.tool(
  "bsnes_break",
  "Pause the emulator immediately. Returns CPU registers, the current " +
  "instruction, and what triggered the break. Use before reading registers " +
  "or memory.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/break"), null, 2) }]
  })
);

server.tool(
  "bsnes_resume",
  "Resume execution until the next breakpoint or step boundary. Returns " +
  "immediately — use bsnes_get_status to poll for the next pause.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/resume"), null, 2) }]
  })
);

server.tool(
  "bsnes_run",
  "Exit debug mode and run the emulator at full speed with no breakpoints " +
  "active. Use when analysis is complete.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/run"), null, 2) }]
  })
);

// Stepping

server.tool(
  "bsnes_step_into",
  "Execute exactly one 65C816 instruction and return the resulting CPU state. " +
  "Enters subroutines (JSR/JSL). Blocks until the step completes.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/into"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_over",
  "Execute one instruction, stepping over subroutine calls (JSR/JSL) without " +
  "entering them. Blocks until the step completes.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/over"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_out",
  "Run until the current subroutine returns (RTS/RTL/RTI) and return the " +
  "resulting CPU state. Blocks until complete.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/out"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_to_vblank",
  "Run until the start of the next VBlank period and return CPU state. " +
  "Useful for analysing frame-boundary code. Blocks up to 30 seconds.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/vblank"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_to_hblank",
  "Run until the start of the next HBlank period and return CPU state. " +
  "Blocks up to 30 seconds.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/hblank"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_to_nmi",
  "Run until the next NMI interrupt fires and return CPU state. " +
  "Blocks up to 30 seconds.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/nmi"), null, 2) }]
  })
);

server.tool(
  "bsnes_step_to_irq",
  "Run until the next IRQ interrupt fires and return CPU state. " +
  "Blocks up to 30 seconds.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/step/irq"), null, 2) }]
  })
);

// System control

server.tool(
  "bsnes_reset",
  "Send a soft reset to the SNES. The emulator continues running after " +
  "reset — call bsnes_break afterward to pause at the reset vector. " +
  "Requires a loaded cartridge with power on.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/reset"), null, 2) }]
  })
);

server.tool(
  "bsnes_reload",
  "Reload the currently loaded ROM from disk, resetting all emulator state. " +
  "Useful after the ROM file has been modified externally (e.g. after a new " +
  "assembly build).",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/reload"), null, 2) }]
  })
);

server.tool(
  "bsnes_power_cycle",
  "Hard reset the SNES (power off then power on). Clears all CPU and PPU " +
  "state. More thorough than bsnes_reset.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("POST", "/power-cycle"), null, 2) }]
  })
);

server.tool(
  "bsnes_load_cartridge",
  "Load a ROM file by absolute filesystem path. Unloads any currently loaded " +
  "game first (saving SRAM). Applies BPS/UPS/IPS patches automatically if " +
  "present alongside the ROM file.",
  {
    path: z.string()
      .describe("Absolute filesystem path to the ROM file, e.g. '/home/user/roms/som.sfc'"),
  },
  async ({ path }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(await api("POST", "/cartridge/load", { path }), null, 2)
    }]
  })
);

// CPU registers and disassembly

server.tool(
  "bsnes_get_registers",
  "Read all 65C816 registers (PC, A, X, Y, S, D, DB, P) and processor flags " +
  "(E, N, V, M, X, D, I, Z, C). Requires the emulator to be paused.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("GET", "/cpu/registers"), null, 2) }]
  })
);

server.tool(
  "bsnes_set_registers",
  "Write one or more CPU registers or flags. Omit fields to leave them " +
  "unchanged. Pass registers as hex strings (e.g. { pc: 'C0A3F2' }) and " +
  "flags nested under a 'flags' object (e.g. { flags: { m: true } }). " +
  "Requires the emulator to be paused.",
  {
    registers: z.record(z.string()).optional()
      .describe("Register values as hex strings: pc, a, x, y, s, d, db, p"),
    flags: z.record(z.boolean()).optional()
      .describe("Flag values as booleans: e, n, v, m, x, d, i, z, c"),
  },
  async ({ registers, flags }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("PUT", "/cpu/registers", { ...registers, flags }),
        null, 2
      )
    }]
  })
);

server.tool(
  "bsnes_disassemble_current",
  "Disassemble ONLY the current instruction (at the current PC), using the " +
  "live M/X state at that PC. There is no look-ahead — to inspect the next " +
  "instruction, step first, then call this again. Reading ahead and decoding " +
  "future instructions statically is not supported because it is unreliable " +
  "across M/X changes. Requires the emulator to be paused.",
  {},
  async () => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("GET", "/cpu/disassemble"),
        null, 2
      )
    }]
  })
);

server.tool(
  "bsnes_get_usage",
  "Read per-byte execution history for a range of SNES addresses. Each entry " +
  "shows whether the byte has been read, written, or executed, and the M/X " +
  "flag state at last execution. Useful for confirming code vs data " +
  "boundaries before disassembling.",
  {
    addr: z.string().describe("Start SNES address in hex, e.g. 'C0A3F2'"),
    count: z.number().int().min(1).describe("Number of bytes to return"),
  },
  async ({ addr, count }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("GET", `/cpu/usage?addr=${addr}&count=${count}`),
        null, 2
      )
    }]
  })
);

server.tool(
  "bsnes_get_wram_provenance",
  "Look up the ROM source address for each byte in a WRAM range. " +
  "Returns a 'provenance' array where each entry is either a 6-digit hex " +
  "ROM address (where the byte was originally copied from) or null (byte " +
  "has no ROM origin — written by game code or never set). " +
  "Use this whenever the CPU is executing from WRAM: call this with the " +
  "current PC area to find the corresponding ROM addresses, then use " +
  "diz_get_byte_by_snes_address on those ROM addresses to annotate the " +
  "correct location in Diz. Provenance chains (WRAM-to-WRAM copies) are " +
  "resolved automatically to the ultimate ROM origin. " +
  "Requires the emulator to be paused.",
  {
    addr: z.string()
      .describe("Start WRAM address in hex, e.g. '7E8000'"),
    count: z.number().int().min(1).max(4096).default(64)
      .describe("Number of bytes to query (default 64, max 4096)"),
  },
  async ({ addr, count }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("GET", `/wram/provenance?addr=${addr}&count=${count}`),
        null, 2
      )
    }]
  })
);

// Memory

server.tool(
  "bsnes_read_memory",
  "Read bytes from a SNES memory bus. " +
  "Sources: cpu (full 24-bit bus, use for WRAM/ROM/registers), " +
  "vram, oam, cgram, cartrom (raw ROM file), cartram (save RAM). " +
  "Count is capped at 4096. Requires the emulator to be paused.",
  {
    source: z.enum(["cpu", "vram", "oam", "cgram", "cartrom", "cartram"])
      .describe("Memory bus to read from"),
    addr: z.string().describe("Start address in hex"),
    count: z.number().int().min(1).max(4096).default(256)
      .describe("Number of bytes to read (default 256, max 4096)"),
  },
  async ({ source, addr, count }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("GET", `/memory/${source}?addr=${addr}&count=${count}`),
        null, 2
      )
    }]
  })
);

server.tool(
  "bsnes_dump_memory",
  "Dump a range of memory directly to a binary file on the machine running " +
  "bsnes. Use this instead of bsnes_read_memory for large regions (more than " +
  "a few hundred bytes) — it writes raw bytes to disk with no size cap and " +
  "no data passing through the conversation. Requires the emulator to be " +
  "paused. The file is written on the bsnes host; provide an absolute path.",
  {
    source: z.enum(["cpu","apu","apuram","dsp","vram","oam","cgram","cartrom","cartram"])
      .describe("Memory bus to read from (use 'cpu' for WRAM/ROM/registers)"),
    addr: z.string()
      .describe("Start address as a hex string, e.g. '7E8000'"),
    count: z.number().int().min(1).max(0x1000000)
      .describe("Number of bytes to dump"),
    path: z.string()
      .describe("Absolute file path on the bsnes host to write the raw bytes to"),
  },
  async ({ source, addr, count, path }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("POST", `/memory/${source}/dump`, { addr, count, path }),
        null, 2)
    }]
  })
);

server.tool(
  "bsnes_dump_screen",
  "Write the most recently rendered screen to a PNG file on the bsnes host, " +
  "so you can SEE what is on screen instead of inferring the game's state from " +
  "registers and memory. Use this when you need to know what phase the game is " +
  "in (title, menu, loading, gameplay, a specific screen). Works while paused — " +
  "it captures the last rendered frame. After dumping, read the PNG to view it. " +
  "Provide an absolute host path, conventionally under /render or /tmp.",
  {
    path: z.string()
      .describe("Absolute host path for the PNG, e.g. '/render/screen.png'"),
  },
  async ({ path }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(await api("POST", "/screen/dump", { path }), null, 2)
    }]
  })
);

server.tool(
  "bsnes_write_memory",
  "Write bytes to a SNES memory bus. Limited to 4096 bytes per call. " +
  "Requires the emulator to be paused. Use with care — writes are live " +
  "and immediately affect emulator state.",
  {
    source: z.enum(["cpu", "vram", "oam", "cgram", "cartrom", "cartram"])
      .describe("Memory bus to write to"),
    addr: z.string().describe("Start address in hex"),
    data: z.array(z.number().int().min(0).max(255))
      .describe("Byte values to write (0–255 each, max 4096 entries)"),
  },
  async ({ source, addr, data }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("PUT", `/memory/${source}?addr=${addr}`, { data }),
        null, 2
      )
    }]
  })
);

// Breakpoints

server.tool(
  "bsnes_list_breakpoints",
  "List all currently set breakpoints with their indices, addresses, modes, " +
  "and hit counts. Use indices with bsnes_delete_breakpoint.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("GET", "/breakpoints"), null, 2) }]
  })
);

server.tool(
  "bsnes_add_breakpoint",
  "Add a breakpoint. Set mode to one or more of Exec, Read, Write. " +
  "Source is typically CPUBus for code breakpoints. " +
  "Optionally specify a data value and comparison operator to break only " +
  "when a specific value is read or written.",
  {
    addr: z.string().describe("SNES address in hex, e.g. 'C0A3F2'"),
    mode: z.array(z.enum(["Exec", "Read", "Write"])).min(1)
      .describe("When to trigger: Exec, Read, and/or Write"),
    source: z.enum(["CPUBus", "VRAM", "OAM", "CGRAM"]).default("CPUBus")
      .describe("Memory bus to watch (default CPUBus)"),
    addrEnd: z.string().optional()
      .describe("End of address range for range breakpoints (optional)"),
    data: z.number().int().min(-1).max(255).default(-1)
      .describe("Data value to match (-1 = any value)"),
    compare: z.enum(["Equal", "NotEqual", "Less", "LessEqual", "Greater", "GreaterEqual"])
      .default("Equal")
      .describe("Comparison operator for data condition (default Equal)"),
  },
  async ({ addr, mode, source, addrEnd, data, compare }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(
        await api("POST", "/breakpoints", { addr, mode, source, addrEnd, data, compare }),
        null, 2
      )
    }]
  })
);

server.tool(
  "bsnes_delete_breakpoint",
  "Remove a single breakpoint by its index. Use bsnes_list_breakpoints to " +
  "find indices.",
  {
    index: z.number().int().min(0).describe("Breakpoint index to remove"),
  },
  async ({ index }) => ({
    content: [{
      type: "text",
      text: JSON.stringify(await api("DELETE", `/breakpoints/${index}`), null, 2)
    }]
  })
);

server.tool(
  "bsnes_clear_breakpoints",
  "Remove all breakpoints at once.",
  {},
  async () => ({
    content: [{ type: "text", text: JSON.stringify(await api("DELETE", "/breakpoints"), null, 2) }]
  })
);

const transport = new StdioServerTransport();
await server.connect(transport);
