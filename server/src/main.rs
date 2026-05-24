// nthttp — CLI thin wrapper sobre nettransfer_server (lib.rs)
//
// Uso:
//   nthttp [<port>] [<dir>] [opciones]
//
//   port            default 8088
//   dir             default ./files
//
// Opciones (uploads — Feature A):
//   --writable           Acepta uploads PUT. Default: OFF (read-only).
//   --max-upload <N>     Tamaño maximo de upload en bytes. Default 16777216 (16 MiB).
//   --overwrite          Permite que un PUT sobreescriba un fichero existente.

use std::net::SocketAddr;
use std::path::PathBuf;

use nettransfer_server::{local_ipv4s, Server, ServerConfig, ServerEvent};

fn main() {
    let mut port: u16 = 8088;
    let mut root: PathBuf = PathBuf::from("./files");
    let mut writable = false;
    let mut overwrite = false;
    let mut max_upload: u64 = 16 * 1024 * 1024;

    let mut positionals: Vec<String> = Vec::new();
    let mut args = std::env::args().skip(1).peekable();
    while let Some(a) = args.next() {
        match a.as_str() {
            "--writable"   => writable = true,
            "--overwrite"  => overwrite = true,
            "--max-upload" => {
                match args.next().and_then(|s| s.parse::<u64>().ok()) {
                    Some(n) => max_upload = n,
                    None => {
                        eprintln!("[ERR] --max-upload requires a positive integer (bytes)");
                        std::process::exit(2);
                    }
                }
            }
            "-h" | "--help" => { print_help(); return; }
            other if other.starts_with('-') => {
                eprintln!("[ERR] Unknown option: {}", other);
                std::process::exit(2);
            }
            _ => positionals.push(a),
        }
    }
    if let Some(p) = positionals.first() {
        if let Ok(n) = p.parse::<u16>() { port = n; }
        else { eprintln!("[ERR] Bad port: {}", p); std::process::exit(2); }
    }
    if let Some(d) = positionals.get(1) {
        root = PathBuf::from(d);
    }

    let cfg = ServerConfig {
        bind_addr: SocketAddr::from(([0, 0, 0, 0], port)),
        root,
        writable,
        max_upload,
        overwrite,
    };

    let server = match Server::start(cfg) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[ERR] {}", e);
            std::process::exit(1);
        }
    };

    print_banner(server.root().display().to_string(), server.local_addr().port(), writable, overwrite, max_upload);

    // Bucle de eventos: imprime cada peticion.
    loop {
        match server.recv() {
            Some(ServerEvent::Started { local_addr, root }) => {
                eprintln!("[OK] Listening on {} (root: {})", local_addr, root.display());
            }
            Some(ServerEvent::Request { peer, method, path, status, bytes_sent, bytes_received, .. }) => {
                if bytes_received > 0 {
                    println!(
                        "[{}] {} {} -> {} (sent {}, received {})",
                        peer, method, path, status, bytes_sent, bytes_received
                    );
                } else {
                    println!(
                        "[{}] {} {} -> {} ({} bytes)",
                        peer, method, path, status, bytes_sent
                    );
                }
            }
            Some(ServerEvent::UploadCompleted { peer, path, bytes, overwrote, .. }) => {
                println!(
                    "[{}] ✓ UPLOAD {} ({} bytes){}",
                    peer, path, bytes,
                    if overwrote { " [overwrote]" } else { "" }
                );
            }
            Some(ServerEvent::Warning(msg)) => {
                eprintln!("[WARN] {}", msg);
            }
            Some(ServerEvent::Stopped) => {
                eprintln!("[OK] Stopped.");
                break;
            }
            None => break,
        }
    }
}

fn print_help() {
    println!("nthttp — MSX Net Transfer HTTP server");
    println!();
    println!("Uso: nthttp [<port>] [<dir>] [opciones]");
    println!();
    println!("Defaults: port=8088, dir=./files");
    println!();
    println!("Opciones:");
    println!("  --writable           Acepta uploads PUT");
    println!("  --max-upload <N>     Tamano maximo de upload en bytes (default 16 MiB)");
    println!("  --overwrite          Permite sobrescritura en uploads (default: 409)");
    println!("  -h, --help           Esta ayuda");
}

fn print_banner(root: String, port: u16, writable: bool, overwrite: bool, max_upload: u64) {
    println!("=========================================================");
    println!(" MSX Net Transfer HTTP server");
    println!(" Raiz: {}", root);
    println!(" Puerto: {}", port);
    println!(" Uploads: {}{}",
        if writable { "ON" } else { "off (read-only)" },
        if writable {
            format!("  (max {} MiB, overwrite={})",
                max_upload / (1024 * 1024),
                if overwrite { "yes" } else { "no" })
        } else { String::new() }
    );
    println!("---------------------------------------------------------");
    println!(" URLs para usar desde MSX:");
    let ips = local_ipv4s();
    if ips.is_empty() {
        println!("   http://127.0.0.1:{}/", port);
    } else {
        for ip in ips {
            println!("   http://{}:{}/", ip, port);
        }
    }
    println!("=========================================================");
}
