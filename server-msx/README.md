# MSX Net Transfer — MSX server (`NTS.COM`)

A counterpart to `NT.COM`: this is **a Net Transfer HTTP server that runs on the MSX itself**. It exposes a folder on the MSX disk over HTTP/1.0 so any other client (a browser on a PC, `curl`, the Rust GUI, or a second MSX running `NT.COM`) can download files from it — and optionally upload to it.

> **Status: work in progress (v0.3 target).** This document is the design spec; the actual implementation is incremental, starting with the UI shell.

## Why?

`NT.COM` (client) + `nthttp` / `nthttp-gui` (PC server) covers PC ↔ MSX in both directions. But there's a third axis: **MSX ↔ MSX** and **MSX → browser**. The MSX-side server unlocks:

- Drag a file straight off your MSX from a web browser on your phone.
- One MSX serving ROMs/DSKs to another MSX over the LAN with no PC in the middle.
- A loose "remote disk" pattern using `curl PUT` to push artefacts during dev.

## How it differs from NestorWeb

Konamiman's [NestorWeb](https://github.com/Konamiman/NestorWeb) is the canonical HTTP server for MSX. It's MIT-licensed, well-engineered, and supports CGI scripts. It was the obvious starting point for this project.

`NTS.COM` shares the same **architectural pattern** as NestorWeb — a foreground MSX-DOS 2 program with a polling main loop that yields to the UNAPI stack via `TCPIP_WAIT`. After studying NestorWeb in detail (`server-msx/nestorweb-reference/`, gitignored), we decided to **write `NTS.COM` fresh** instead of forking, for three reasons:

1. **Different toolchain.** NestorWeb uses raw SDCC + a custom makefile + a custom crt0. `NTS.COM` reuses our MSXgl + `network.h` + `bios`-wrapper infrastructure that already powers `NT.COM`. Mixing the two would mean maintaining a parallel build for one binary.
2. **Different UX.** NestorWeb is a headless CLI server (`printf` status lines). `NTS.COM` has an interactive 80-column SCREEN 0 UI with toggles, folder picker and file listing — same shell as `NT.COM`. Forking NestorWeb just to delete its `printf`s and bolt on a UI is more work than starting fresh.
3. **Different feature set.** NestorWeb has CGI, basic auth, request body caching. `NTS.COM` doesn't need any of that — just `GET /<file>`, `GET /_list`, and `PUT /<file>` to match the Rust server's protocol. Smaller scope.

So `NTS.COM` is **inspired by NestorWeb** (especially `tcpip.c`'s passive-listen pattern and the `LetTcpipBreathe()` yield idiom) but does not include its code. NestorWeb is referenced locally for design questions only.

## UX (mirrors `NT.COM`)

```
NTS v0.3.0 - Net Transfer server   [SHARING ON] [UPLOADS ON] [ALL]   IP: 192.168.0.150:8088
--------------------------------------------------------------------------------
  FILE                  SIZE             FILE                  SIZE
> game.rom              16384            readme.txt              520
  notes.txt              1024            sample.wav            32000
  ...
--------------------------------------------------------------------------------
File 1 of 8                                              Requests served: 3
Arrows nav   S sharing   U uploads   F filter   D folder   R refresh   ESC exit
```

- **`S`** — toggle the TCP listener on/off. When OFF, no new connections are accepted; existing connections are aborted.
- **`U`** — toggle whether `PUT` is allowed. Mirrors the server's `--writable` flag.
- **`F`** — cycle the extension filter: `[ALL]` → `[ROM]` → `[DSK]` → `[ALL]` (same as the client).
- **`D`** — change the served folder (prompts for path; defaults to current drive's root).
- **`R`** — refresh the file listing from disk.
- **Arrow keys + ENTER** — for now, just navigate the list (selection has no action; the file is served on HTTP request).
- **`ESC`** — quit cleanly. Existing connections are aborted, TCP listener closed.

Status line shows:
- Active sharing state (green `SHARING ON` / red `SHARING OFF`).
- Active upload permission.
- Active extension filter.
- Local IP and listening port — this is what the PC user types in their browser or in `NT.COM`.
- Number of requests served since the program started (and last request summary if there's room).

## Endpoints

Identical to the Rust server, so `NT.COM` and any HTTP client work against either interchangeably:

| Path / Method   | Behaviour |
| --------------- | --------- |
| `GET /_list`    | `name<TAB>size<LF>` per file in served folder |
| `GET /<f>`      | Sends file `<f>` with `Content-Length`. `Range: bytes=N-` → `206 Partial Content`. |
| `GET /`         | HTML listing for browser access |
| `PUT /<f>`      | (when uploads enabled) Streams body to `<f>.part`, renames on completion. `If-Match: *` overrides 409. |
| `HEAD /<f>`     | Headers only |

Unsupported: `POST`, CGI, basic auth, virtual hosts. Same as the Rust server v0.2.

## Architectural notes (for next-session implementation)

### Main loop

Single-threaded event loop. Each tick:

1. **Render dirty UI regions** (only what changed — keep the flicker-free policy from `NT.COM`).
2. **Poll keyboard** via `My_Snsmat` (the same CALSLT-wrapped helper used by `NT.COM`).
3. **If sharing is ON and no active connection**: check `Net_GetConnState(passive_conn)`. If `TCP_STATE_ESTABLISHED`, a client has connected → process the request in-line.
4. **If sharing is ON and a request is in flight**: read more headers / stream body / send response (state machine, to keep the UI responsive between chunks).
5. **`tcpip_wait()`** — yields to the UNAPI ISR. Equivalent to NestorWeb's `LetTcpipBreathe()`.

### UNAPI passive listener

New territory for us (`NT.COM` only uses active connections). Reference: `nestorweb-reference/tcpip.c::_OpenPassiveTcpConnection`. Pattern:

```c
g_TcpParms.dest_ip[0..3] = 0;   // 0.0.0.0 = accept from anywhere
g_TcpParms.dest_port = 0;
g_TcpParms.local_port = 8088;
g_TcpParms.flags = CONNTYPE_PASSIVE;   // <-- key difference
tcpip_tcp_open(&g_TcpParms, &handle);  // returns immediately; conn is in LISTEN
```

When `tcpip_tcp_state(handle)` returns `conn_state = TCP_STATE_ESTABLISHED`, a client has connected on that handle. After we close the connection, we re-open a fresh passive socket for the next client. **One concurrent connection at a time** — acceptable for the use case.

### HTTP request parser

Reuse `NT.COM`'s `AppendHeader` + `ParseHeaders` idea: accumulate bytes into a 512-byte header buffer until `\r\n\r\n` is found, then walk the buffer for the start line and `Content-Length` / `Range` / `Content-Range` / `If-Match`. Same defensive style.

For `PUT`: stream-to-file pattern identical to the Rust server (`<file>.part`, atomic rename, `If-Match: *` for force overwrite, 409 on conflict without it).

### Path safety

Same rule as the Rust server: `safe_rel_path_for_put` accepts single-component filenames only. No `..`, no `/`, no `\`. The served folder is the only one ever touched.

### Memory layout

`ForceRamAddr = 0x8000` (page 2) as in `NT.COM` — UNAPINET's mapper segment lives in page 1, and our globals must not overlap.

### Files to create (next session)

```
server-msx/
├── README.md           (this file)
├── nts.c               main program (UI + HTTP state machine)
├── network_passive.h   passive-listener helpers (extends network.h pattern)
├── msxgl_config.h      MSXgl module config (copy from client/)
├── project_config.js   MSXgl project config (based on client/)
└── build.sh            compile wrapper (based on client/)
```

The `nestorweb-reference/` folder is gitignored — clone it yourself if you want to consult it:

```bash
git clone https://github.com/Konamiman/NestorWeb.git server-msx/nestorweb-reference
```

## License

Same as the rest of the project — [PolyForm Noncommercial 1.0.0](../LICENSE).

NestorWeb (the upstream reference) is MIT-licensed by Néstor Soriano (Konamiman). We do not include or modify its source code.
