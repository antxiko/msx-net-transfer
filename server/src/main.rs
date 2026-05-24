// nthttp — CLI thin wrapper sobre nettransfer_server (lib.rs)
//
// Uso:
//   nthttp                          (puerto 8080, raiz ./files)
//   nthttp 8080                     (puerto, raiz ./files)
//   nthttp 8080 C:\msx\share        (puerto y raiz)

use std::net::SocketAddr;
use std::path::PathBuf;

use nettransfer_server::{local_ipv4s, Server, ServerConfig, ServerEvent};

fn main() {
    let mut args = std::env::args().skip(1);
    let port: u16 = args
        .next()
        .as_deref()
        .map(|s| s.parse().unwrap_or(8080))
        .unwrap_or(8080);
    let root: PathBuf = args
        .next()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("./files"));

    let cfg = ServerConfig {
        bind_addr: SocketAddr::from(([0, 0, 0, 0], port)),
        root,
    };

    let server = match Server::start(cfg) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[ERR] {}", e);
            std::process::exit(1);
        }
    };

    print_banner(server.root().display().to_string(), server.local_addr().port());

    // Bucle de eventos: imprime cada peticion.
    loop {
        match server.recv() {
            Some(ServerEvent::Started { local_addr, root }) => {
                eprintln!("[OK] Listening on {} (root: {})", local_addr, root.display());
            }
            Some(ServerEvent::Request { peer, method, path, status, bytes_sent, .. }) => {
                println!(
                    "[{}] {} {} -> {} ({} bytes)",
                    peer, method, path, status, bytes_sent
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

fn print_banner(root: String, port: u16) {
    println!("=========================================================");
    println!(" NetTransfer HTTP server");
    println!(" Raiz: {}", root);
    println!(" Puerto: {}", port);
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
