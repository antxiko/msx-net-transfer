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
- **Cross-platform server**: native binaries for Windows, Linux and macOS, built in Rust.
- **No protocol invention**: standard HTTP/1.0 — you can also browse the same folder from any web browser.
- **Interactive MSX browser**: 80-column SCREEN 0 UI, arrow keys to navigate, `ENTER` to download, `R` to refresh, `ESC` to quit.
- **Real binary transfer**: the server sends `Content-Length` and raw bytes; the MSX client streams straight to a file handle via MSX-DOS 2.
- **Tiny client**: `NT.COM` fits comfortably under 16 KiB of resident code.
- **Safe by default**: the server rejects `..` path components, absolute paths, and anything that would escape the served folder.

## How does it work?

1. The server listens on TCP port **8088** and serves files from a folder you choose with a GUI button (or `cargo run` argument).
2. The MSX runs `NT <ip>` (no port, no protocol — both are implicit). The client calls `GET /_list` to fetch a plain-text directory listing.
3. You navigate the listing with the arrow keys and press `ENTER` to start the download. The client opens the target file with MSX-DOS 2's `_CREATE` function and streams the HTTP body straight to disk.

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
nthttp                              # serves ./files on port 8080
nthttp 8088                         # serves ./files on port 8088
nthttp 8088 /home/me/msx-share      # custom port and folder
```

### 2. Install `NT.COM` on the MSX

Copy `NT.COM` from the release onto any disk image or storage device your MSX can boot from with **MSX-DOS 2**. The disk must also have:

- A loaded **UNAPI TCP/IP implementation** (e.g. `UNAPINET.COM` for openMSX, or the TSR that ships with your Ethernet card).
- Standard MSX-DOS 2 files (`COMMAND2.COM`, `NEXTOR.SYS` if you use Nextor, etc.).

### 3. Run the client

From the MSX-DOS 2 prompt:

```
A:\>UNAPINET            (or whatever loads your UNAPI TCP/IP stack)
A:\>NT 192.168.0.102    (the IP shown by the server GUI)
```

The screen switches to 80-column mode and the file browser appears.

## Client keyboard

| Key                  | Action                                  |
| -------------------- | --------------------------------------- |
| ↑ / ↓                | Move cursor by one item                 |
| ← / →                | Jump to previous / next column          |
| `ENTER`              | Download the highlighted file           |
| `R`                  | Refresh the listing (server folder may have changed) |
| `ESC`                | Quit and return to MSX-DOS prompt       |

Filenames longer than 8.3 are converted to uppercase 8.3 when written to MSX disk — e.g. `dbg_startaddr.asc` is stored as `DBG_STAR.ASC`.

## Server GUI

- **Large status badge**: green (`RUNNING`) or red (`STOPPED`) at a glance.
- **Folder picker**: native file dialog on every platform.
- **MSX command line**: shows exactly what to type on the MSX (no need to "copy IP" — you can't paste into an MSX anyway).
- **File browser**: list / columns / icons view.
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

## Endpoints exposed by the server

| Path        | Purpose                                                              |
| ----------- | -------------------------------------------------------------------- |
| `GET /`     | HTML directory listing (for browsing from any web browser)           |
| `GET /<f>`  | Sends the file `<f>` with `Content-Length` and `application/octet-stream` |
| `GET /_list`| Machine-readable plain text listing: one `name<TAB>size<LF>` per file. Used by `NT.COM`. |

Range requests (`Range: bytes=N-`) are supported and reply with `206 Partial Content`, so you can also use Net Transfer with any HTTP client that supports resumable downloads.

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
