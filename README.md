# MSX Net Transfer

**Share files from your PC to a real MSX over the network**, using the [MSX UNAPI TCP/IP](https://github.com/Konamiman/MSX-UNAPI-specification) standard.

Net Transfer is a tiny client/server pair that lets your MSX browse a folder on your modern PC and pull files down over plain HTTP, with no FTP, no SMB and no floppy gymnastics.

```
       PC (Windows/Linux/macOS)              MSX-DOS 2 with UNAPI TCP/IP
   ┌──────────────────────────────┐         ┌──────────────────────────────┐
   │  NetTransfer Server (Rust)   │         │      NT.COM  (client)        │
   │                              │         │                              │
   │  ┌────────────────────────┐  │         │  ┌────────────────────────┐  │
   │  │  /home/user/msx-share/ │  │ ──────► │  │  > big16k.bin          │  │
   │  │    big16k.bin          │  │  HTTP/  │  │    block1k.bin         │  │
   │  │    block1k.bin         │  │  1.0    │  │    hello.txt           │  │
   │  │    hello.txt           │  │ ◄────── │  │    rand4k.bin          │  │
   │  │    rand4k.bin          │  │         │  │    readme.txt          │  │
   │  └────────────────────────┘  │         │  │    test.txt            │  │
   │       Port 8088 (HTTP)       │         │  │  File 1 of 6           │  │
   └──────────────────────────────┘         │  └────────────────────────┘  │
                                            │   80-column browser, MSX-DOS 2│
                                            └──────────────────────────────┘
```

## Features

- **Two-process design**: a small GUI/CLI server on the PC and a single `NT.COM` on the MSX.
- **Cross-platform server**: native binaries for Windows, Linux and macOS, built in Rust (eframe / egui).
- **No protocol invention**: standard HTTP/1.0 — you can also browse the same folder from any web browser and `curl` works as a client too.
- **Bidirectional**: download server → MSX (`GET`) and upload MSX → server (`PUT`), with optional resume (`Content-Range`).
- **Dual-view MSX browser**: `S` shows the server's files, `L` shows the local MSX disk. `ENTER` downloads or uploads depending on the active view.
- **Extension filter**: press `F` in either view to cycle through `[ALL] → [ROM] → [DSK] → [ALL]` — useful for picking the right artefact when you have hundreds of files on the server or on the MSX.
- **Safe upload by default**: the server is read-only unless you explicitly enable uploads with `--writable` (CLI) or the *"Permitir uploads"* checkbox (GUI). Conflicting filenames return `409 Conflict` and the MSX prompts `(O)verwrite / (C)ancel`.
- **Safe download by default**: when a download would overwrite an existing file on the MSX disk, the client prompts `ENTER` to overwrite or `ESC` to skip. Use the `/F` flag to bypass the prompt (force overwrite).
- **Real binary transfer**: the server sends `Content-Length` and raw bytes; the MSX client streams straight to a file handle via MSX-DOS 2. The server writes uploads to a sidecar `.part` file and renames atomically on success.
- **Tiny client**: `NT.COM` is around 8.5 KiB of compiled Z80 code (padded into a 32 KiB COM by MSXgl).
- **Safe by default on paths**: the server rejects `..` path components, absolute paths, and anything that would escape the served folder. Uploads are restricted to direct children of the served folder (no subdirectories).

## How does it work?

1. The server listens on TCP port **8088** and serves files from a folder you choose with a GUI button (or a CLI argument).
2. The MSX runs `NT <ip>` (no port, no protocol — both are implicit). The client calls `GET /_list` to fetch a plain-text directory listing and shows it in an 80-column browser.
3. From the SERVER view (`S`), arrow keys + `ENTER` download the highlighted file to MSX disk. From the LOCAL view (`L`), the same gesture **uploads** the highlighted MSX file to the server with a `PUT` request.
4. Uploads land on a sidecar `<file>.part` and are renamed to their final name only when the body is fully received — so half-finished transfers never leave a corrupted file on the server. If `Content-Range` is supplied, the server will append at the requested offset (resume).

The MSX side talks to the network through any UNAPI TCP/IP 1.1 implementation: the [Obsonet](https://www.obsonet.com/) Ethernet card, GR8NET, an InterNestor stack on Caribbean, or the [openMSXnet](https://github.com/antxiko/openMSXnet) extension for the openMSX emulator (used to develop this project).

## Quick start

### 1. Run the server on your PC

Download the latest release for your platform from the *Releases* page and run the GUI:

| Platform | Binary               |
| -------- | -------------------- |
| Windows  | `nthttp-gui.exe`     |
| Linux    | `nthttp-gui`         |
| macOS    | `nthttp-gui`         |

The GUI lets you pick the folder to share, start/stop the server with a single button, and shows the exact command to type on the MSX side (e.g. `NT 192.168.0.102`).

If you prefer a command line server (e.g. for a Raspberry Pi without a desktop):

```bash
nthttp                                          # serves ./files on port 8088 (read-only)
nthttp 8088 /home/me/msx-share                  # custom port and folder
nthttp 8088 /home/me/msx-share --writable       # allow uploads (PUT) — default 16 MiB max
nthttp 8088 /home/me/msx-share --writable --overwrite   # allow PUT on existing files
nthttp --help                                   # all flags
```

### 2. Install `NT.COM` on the MSX

Copy `NT.COM` from the release onto any disk image or storage device your MSX can boot from with **MSX-DOS 2**. The disk must also have:

- A loaded **UNAPI TCP/IP implementation** (e.g. `UNAPINET.COM` for openMSX, or the TSR that ships with your Ethernet card).
- Standard MSX-DOS 2 files (`COMMAND2.COM`, `NEXTOR.SYS` if you use Nextor, etc.).

### 3. Run the client

From the MSX-DOS 2 prompt:

```
A:\>UNAPINET                    (or whatever loads your UNAPI TCP/IP stack)
A:\>NT 192.168.0.102            (the IP shown by the server GUI)
A:\>NT 192.168.0.102 /E:ROM     (start with the ROM-only extension filter active)
A:\>NT 192.168.0.102 /E:DSK     (start with [DSK] filter)
A:\>NT 192.168.0.102 /F         (force-overwrite on download, no prompt)
```

The screen switches to 80-column mode and the file browser appears.

## Client keyboard

| Key                  | Action                                                                |
| -------------------- | --------------------------------------------------------------------- |
| ↑ / ↓                | Move cursor by one item                                               |
| ← / →                | Jump to previous / next column                                        |
| `S`                  | Switch to **server** view (remote files; ENTER downloads)             |
| `L`                  | Switch to **local** view (MSX disk files; ENTER uploads)              |
| `F`                  | Cycle the extension filter: `[ALL]` → `[ROM]` → `[DSK]` → `[ALL]`     |
| `ENTER`              | Download the highlighted file (server view) **or** upload it (local view) |
| `R`                  | Refresh the current listing (server folder or local disk)             |
| `O` / `C`            | On a 409 conflict during upload: `O` overwrite, `C` cancel            |
| `ENTER` / `ESC`      | On a local-file overwrite prompt during download: `ENTER` overwrites, `ESC` skips |
| `ESC`                | Cancel the in-flight upload, or quit and return to MSX-DOS prompt     |

Filenames longer than 8.3 are converted to uppercase 8.3 when written to MSX disk on download — e.g. `dbg_startaddr.asc` is stored as `DBG_STAR.ASC`. Uploads send the local 8.3 name verbatim to the server.

## Server GUI

- **Large status badge**: green (`RUNNING`) or red (`STOPPED`) at a glance, with a small companion indicator 🔓 `UPLOADS ON` / 🔒 `READ-ONLY`.
- **Folder picker**: native file dialog on every platform.
- **MSX command line**: shows exactly what to type on the MSX (no need to "copy IP" — you can't paste into an MSX anyway). Only the real LAN IP is shown; virtual adapters (Docker, WSL, VirtualBox, Hyper-V) are filtered out.
- **Permissions**: two checkboxes — *Permitir uploads* and *Permitir sobreescritura* — that take effect **live** without restarting the server.
- **Upload log**: collapsible bottom panel with the last 20 successful uploads (time, file, size, source IP, whether it overwrote a previous file).
- **File browser**: list / columns / icons view, auto-refreshes after a successful upload so you can see new arrivals immediately.
- **Theme toggle**: light or dark.

The server is intentionally read-only. It exposes only `GET`/`HEAD` and never writes anything to disk.

## Network requirements

- The MSX and the PC must be reachable from each other on the LAN.
- TCP port **8088** must not be blocked by your firewall (the GUI prompts on the first connection on Windows).
- IPv4 only; the MSX UNAPI spec does not cover IPv6.

## Building from source

### Server (Rust)

```bash
cd server
cargo build --release
# binaries land in target/release/:
#   nthttp.exe / nthttp        (CLI)
#   nthttp-gui.exe / nthttp-gui (GUI)
```

Requires Rust 1.74+ (any recent stable will do).

### Client (MSX-DOS 2)

```bash
cd client
bash build.sh                 # produces ./nt.com
```

Requires:
- A working **MSXgl** checkout (set `MSXGL=/path/to/MSXgl` if it's not at `../../MSXonLIVE/MSXgl`).
- **SDCC 4.5+** (bundled inside MSXgl's `tools/sdcc/`).
- **Node.js** (bundled inside MSXgl's `tools/build/Node/`, or any system Node).

The build script copies the sources into `MSXgl/projects/nt/`, runs MSXgl's standard build, and brings the resulting `nt.com` back next to the script.

## Repository layout

```
NetTransfer/
├── README.md             # this file
├── client/               # MSX-DOS 2 client (C + Z80 asm via MSXgl)
│   ├── nt.c              # main source
│   ├── network.h         # UNAPI TCP wrapper
│   ├── msxgl_config.h
│   ├── project_config.js
│   └── build.sh
├── server/               # cross-platform server (Rust)
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs        # reusable server core (HTTP/1.0, range, /_list)
│       ├── main.rs       # CLI binary (nthttp)
│       └── bin/
│           └── gui.rs    # GUI binary (nthttp-gui, eframe/egui)
└── docs/                 # screenshots / extra docs
```

## HTTP endpoints exposed by the server

| Path / Method   | Purpose                                                              |
| --------------- | -------------------------------------------------------------------- |
| `GET /`         | HTML directory listing (for browsing from any web browser)           |
| `GET /<f>`      | Sends the file `<f>` with `Content-Length` and `application/octet-stream`. Replies `404 Not Found` with an extra `X-Resume-Offset: N` header when a partial `.part` of size N exists. |
| `GET /_list`    | Machine-readable plain text listing: one `name<TAB>size<LF>` per file. Used by `NT.COM`. |
| `PUT /<f>`      | (when `--writable`) Uploads the body as `<f>`. Returns `201 Created` on success, `200 OK` when overwriting (with `--overwrite` or `If-Match: *`), `409 Conflict` otherwise. |

`GET` supports `Range: bytes=N-` (replies `206 Partial Content`).
`PUT` supports `Content-Range: bytes N-M/total` for resumable uploads — the server appends at offset N if it matches the current `.part` size, otherwise replies `416 Range Not Satisfiable` with an `X-Resume-Offset` hint.

HTTP status codes used:

| Code | When |
| ---- | ---- |
| `200 OK` | Successful overwrite on `PUT`, or `GET` of a complete file |
| `201 Created` | New file successfully uploaded |
| `202 Accepted` | Partial `PUT` accepted; `.part` not yet complete |
| `206 Partial Content` | `GET` with `Range` |
| `400 Bad Request` | Malformed request or unsafe path |
| `403 Forbidden` | Path escapes root, or `PUT` when server is read-only |
| `404 Not Found` | File doesn't exist (may include `X-Resume-Offset` if a `.part` is around) |
| `405 Method Not Allowed` | Method other than `GET / HEAD / PUT` |
| `409 Conflict` | `PUT` to an existing file without overwrite permission |
| `411 Length Required` | `PUT` without `Content-Length` |
| `413 Payload Too Large` | `PUT` body exceeds `--max-upload` |
| `416 Range Not Satisfiable` | `PUT` `Content-Range` doesn't match current `.part` size |

### Example: upload from curl

```bash
# Server: nthttp 8088 ./files --writable
curl -X PUT --data-binary @local.bin -H "Content-Type: application/octet-stream" \
  http://192.168.0.102:8088/REMOTE.BIN
# → HTTP/1.0 201 Created

# Overwrite an existing file:
curl -X PUT --data-binary @local.bin -H "If-Match: *" \
  http://192.168.0.102:8088/REMOTE.BIN
# → HTTP/1.0 200 OK
```

## Credits

- **HGET** by [ducasp](https://github.com/ducasp/MSX-Development/tree/master/UNAPI) — the original inspiration. Net Transfer started as a clone of `HGET` and grew into a full client/server pair.
- **MSXgl** by Guillaume "Aoineko" Blanchard — the build system, SDCC integration, and BIOS abstractions used on the MSX client side.
- **MSX UNAPI** by Konamiman — the network standard that makes any of this possible.
- **openMSXnet** — the openMSX extension that provided a development-time UNAPI bridge so the client could be tested without real hardware.
- **eframe / egui** by Emil Ernerfeldt — the immediate-mode GUI toolkit used by the server's desktop UI.

## License

MSX Net Transfer is distributed under the **[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0)** — see [LICENSE](LICENSE).

In short: you may use, study, modify and redistribute the software **for any non-commercial purpose** — personal use, hobby projects, education, research, charities, and government bodies are all fine.

**Commercial use is not granted** by this license. If you need a commercial license (e.g. to bundle MSX Net Transfer with a paid product or service), please contact the author.
