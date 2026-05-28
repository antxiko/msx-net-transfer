// NetTransfer server core — libreria reutilizable.
//
// Expone `Server`, un servidor HTTP/1.0 minimo que sirve ficheros de un
// directorio y emite eventos (peticiones recibidas, errores) por un canal
// `mpsc::Receiver<ServerEvent>` para que un consumidor (CLI o GUI) los
// muestre como prefiera.
//
// Caracteristicas:
//   - HTTP/1.0 sin chunked encoding (Content-Length siempre)
//   - Soporte Range: bytes=N-  (206 Partial Content)
//   - Endpoint /_list con listado plano "name<TAB>size<LF>" (para clientes
//     muy simples como NHGET en MSX)
//   - Listado HTML al pedir "/"
//   - Sanitiza paths (rechaza "..", absolutos, salida de root)
//   - Sin dependencias externas

use std::collections::HashMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime};

const READ_TIMEOUT_SECS: u64 = 30;
const WRITE_TIMEOUT_SECS: u64 = 60;
const SEND_CHUNK: usize = 4096;
const ACCEPT_POLL_MS: u64 = 100;

/// Puerto UDP de descubrimiento. El server ANUNCIA su presencia por broadcast
/// a este puerto y el MSX solo tiene que ESCUCHAR. El protocolo y los magic
/// bytes estan documentados en `announce_loop`.
pub const DISCOVERY_PORT: u16 = 8089;

/// Intervalo entre anuncios de descubrimiento.
const ANNOUNCE_INTERVAL_MS: u64 = 1000;

//─────────────────────────────────────────────────────────────────
// API publica
//─────────────────────────────────────────────────────────────────

#[derive(Clone, Debug)]
pub struct ServerConfig {
    pub bind_addr: SocketAddr,
    pub root: PathBuf,
    /// Habilita uploads PUT. Default: false (read-only).
    pub writable: bool,
    /// Tamaño maximo aceptado en uploads. Default: 16 MiB.
    pub max_upload: u64,
    /// Si TRUE, un PUT a un fichero existente lo sobreescribe (200 OK).
    /// Si FALSE, devuelve 409 Conflict salvo que la peticion incluya
    /// "If-Match: *" para forzar la sobreescritura puntualmente.
    pub overwrite: bool,
    /// Habilita el anuncio UDP de descubrimiento por broadcast (a DISCOVERY_PORT).
    /// Default true. Util desactivar en entornos LAN con muchos services.
    pub discovery: bool,
    /// Nombre que se anuncia en el descubrimiento. Hasta 32 chars ASCII.
    /// Default: hostname del SO o "PC".
    pub name: String,
}

impl Default for ServerConfig {
    fn default() -> Self {
        ServerConfig {
            bind_addr: SocketAddr::from(([0, 0, 0, 0], 8088)),
            root: PathBuf::from("./files"),
            writable: false,
            max_upload: 16 * 1024 * 1024,
            overwrite: false,
            discovery: true,
            name: default_host_name(),
        }
    }
}

fn default_host_name() -> String {
    if let Ok(h) = std::env::var("COMPUTERNAME") {
        return h;
    }
    if let Ok(h) = std::env::var("HOSTNAME") {
        return h;
    }
    "PC".to_string()
}

/// Version publica de `default_host_name` para uso desde CLI/GUI.
pub fn default_host_name_string() -> String {
    default_host_name()
}

#[derive(Clone, Debug)]
pub enum ServerEvent {
    /// El servidor empezo a escuchar.
    Started { local_addr: SocketAddr, root: PathBuf },
    /// Una peticion HTTP fue procesada (puede ser exitosa o no).
    Request {
        time: SystemTime,
        peer: SocketAddr,
        method: String,
        path: String,
        status: u16,
        bytes_sent: u64,
        /// Bytes recibidos en el body (uploads). 0 para GET/HEAD.
        bytes_received: u64,
    },
    /// Una subida PUT fue completada con exito.
    UploadCompleted {
        time: SystemTime,
        peer: SocketAddr,
        path: String,
        bytes: u64,
        overwrote: bool,
    },
    /// Hemos emitido un anuncio de descubrimiento por broadcast.
    /// `count` es el numero acumulado de anuncios enviados desde el arranque.
    DiscoveryAnnounce {
        time: SystemTime,
        count: u64,
    },
    /// Algo no fatal — error de I/O, peticion malformada, etc.
    Warning(String),
    /// El servidor ha parado (limpio).
    Stopped,
}

/// Handle al servidor activo. Mientras existe, el servidor esta corriendo.
/// Cuando se hace drop o stop(), el servidor termina ordenadamente.
pub struct Server {
    stop_flag: Arc<AtomicBool>,
    rx: Receiver<ServerEvent>,
    join: Option<JoinHandle<()>>,
    local_addr: SocketAddr,
    root: PathBuf,
    writable: Arc<AtomicBool>,
    overwrite: Arc<AtomicBool>,
    discovery_name: Arc<std::sync::RwLock<String>>,
    discovery_join: Option<JoinHandle<()>>,
}

/// Estado runtime mutable del servidor (writable, overwrite, etc.) compartido
/// entre el thread accept y los workers. Permite que la GUI cambie el toggle
/// "Allow uploads" sin reiniciar el server.
#[derive(Clone)]
struct SharedState {
    writable: Arc<AtomicBool>,
    overwrite: Arc<AtomicBool>,
    max_upload: u64,
}

impl Server {
    /// Arranca el servidor en un thread separado.
    pub fn start(cfg: ServerConfig) -> std::io::Result<Server> {
        let root = fs::canonicalize(&cfg.root)?;
        if !root.is_dir() {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                format!("'{}' no es un directorio", root.display()),
            ));
        }

        let listener = TcpListener::bind(cfg.bind_addr)?;
        listener.set_nonblocking(true)?;
        let local_addr = listener.local_addr()?;

        let stop_flag = Arc::new(AtomicBool::new(false));
        let (tx, rx) = channel::<ServerEvent>();

        let writable = Arc::new(AtomicBool::new(cfg.writable));
        let overwrite = Arc::new(AtomicBool::new(cfg.overwrite));
        let shared = SharedState {
            writable: Arc::clone(&writable),
            overwrite: Arc::clone(&overwrite),
            max_upload: cfg.max_upload,
        };

        let stop_for_thread = Arc::clone(&stop_flag);
        let root_for_thread = root.clone();
        let tx_for_thread = tx.clone();
        let shared_for_thread = shared.clone();

        let _ = tx.send(ServerEvent::Started {
            local_addr,
            root: root.clone(),
        });

        let join = thread::Builder::new()
            .name("nthttp-accept".to_string())
            .spawn(move || {
                accept_loop(
                    listener,
                    root_for_thread,
                    shared_for_thread,
                    stop_for_thread,
                    tx_for_thread,
                );
            })?;

        // ── Thread de descubrimiento UDP (si habilitado) ──
        let discovery_name = Arc::new(std::sync::RwLock::new(cfg.name.clone()));
        let discovery_join = if cfg.discovery {
            let stop_for_disc = Arc::clone(&stop_flag);
            let name_for_disc = Arc::clone(&discovery_name);
            let tx_for_disc = tx.clone();
            let server_port = local_addr.port();
            // Puerto local efimero (no DISCOVERY_PORT): asi el server no ocupa
            // el 8089 y un MSX que escuche en el mismo host (emulador) puede
            // bindearlo sin conflicto. set_broadcast es lo que permite el
            // sendto a 255.255.255.255.
            match UdpSocket::bind(("0.0.0.0", 0)) {
                Ok(sock) => {
                    sock.set_broadcast(true).ok();
                    let h = thread::Builder::new()
                        .name("nthttp-discovery".to_string())
                        .spawn(move || {
                            announce_loop(sock, server_port, name_for_disc, stop_for_disc, tx_for_disc);
                        })?;
                    Some(h)
                }
                Err(e) => {
                    let _ = tx.send(ServerEvent::Warning(format!(
                        "discovery deshabilitado: no puedo abrir UDP: {}", e
                    )));
                    None
                }
            }
        } else {
            None
        };

        Ok(Server {
            stop_flag,
            rx,
            join: Some(join),
            local_addr,
            root,
            writable,
            overwrite,
            discovery_name,
            discovery_join,
        })
    }

    /// Cambia el nombre que se anuncia en respuestas de descubrimiento.
    pub fn set_discovery_name(&self, name: &str) {
        if let Ok(mut n) = self.discovery_name.write() {
            *n = name.to_string();
        }
    }

    pub fn discovery_name(&self) -> String {
        self.discovery_name.read().map(|n| n.clone()).unwrap_or_default()
    }

    /// Activa / desactiva uploads en caliente (sin reiniciar el servidor).
    pub fn set_writable(&self, enabled: bool) {
        self.writable.store(enabled, Ordering::SeqCst);
    }

    /// Activa / desactiva sobreescritura automatica de uploads existentes.
    pub fn set_overwrite(&self, enabled: bool) {
        self.overwrite.store(enabled, Ordering::SeqCst);
    }

    pub fn is_writable(&self) -> bool {
        self.writable.load(Ordering::SeqCst)
    }

    pub fn is_overwrite(&self) -> bool {
        self.overwrite.load(Ordering::SeqCst)
    }

    pub fn local_addr(&self) -> SocketAddr {
        self.local_addr
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Devuelve el siguiente evento si lo hay (no bloquea).
    pub fn try_recv(&self) -> Option<ServerEvent> {
        self.rx.try_recv().ok()
    }

    /// Bloqueante: espera a que llegue el siguiente evento.
    pub fn recv(&self) -> Option<ServerEvent> {
        self.rx.recv().ok()
    }

    /// Para el servidor y espera a que terminen los threads (accept + discovery).
    pub fn stop(mut self) {
        self.stop_flag.store(true, Ordering::SeqCst);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
        if let Some(join) = self.discovery_join.take() {
            let _ = join.join();
        }
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        // Si el usuario olvido stop(), paramos por su cuenta.
        self.stop_flag.store(true, Ordering::SeqCst);
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
        if let Some(join) = self.discovery_join.take() {
            let _ = join.join();
        }
    }
}

//─────────────────────────────────────────────────────────────────
// Announce loop — broadcast periodico de presencia a DISCOVERY_PORT
//─────────────────────────────────────────────────────────────────
//
// Modelo: el server ANUNCIA, el MSX solo ESCUCHA. Esto es lo que hace que
// funcione en cualquier pila UNAPI (GR8NET, BadCat, ESP-FPGA, openMSXnet):
// recibir un datagrama broadcast no requiere ningun flag especial, mientras
// que ENVIAR broadcast (el modelo inverso) exige SO_BROADCAST y no esta
// garantizado en todos los bridges/stacks.
//
// Cada ANNOUNCE_INTERVAL_MS enviamos a 255.255.255.255:8089:
//   "NT!"            (3 bytes ASCII magic)
//   port_lo, port_hi (u16 LE — puerto TCP donde escucha el server)
//   name_len         (u8 — longitud del nombre, 0..32)
//   name             (name_len bytes ASCII, p.ej. "MyMSX")
//   Total: 3 + 2 + 1 + N = 6..38 bytes
//
// El MSX escucha en 8089 unos segundos, deduplica por IP origen y se queda
// con la lista de servers. El puerto al que conectar va en el payload, no en
// el puerto origen del datagrama.
//─────────────────────────────────────────────────────────────────

fn announce_loop(
    socket: UdpSocket,
    server_port: u16,
    name: Arc<std::sync::RwLock<String>>,
    stop_flag: Arc<AtomicBool>,
    tx: Sender<ServerEvent>,
) {
    let bcast = SocketAddr::from((Ipv4Addr::BROADCAST, DISCOVERY_PORT));
    let mut count: u64 = 0;
    while !stop_flag.load(Ordering::SeqCst) {
        let name_str = name.read().map(|n| n.clone()).unwrap_or_default();
        let nb = name_str.as_bytes();
        let nlen = nb.len().min(32) as u8;
        let mut msg = Vec::with_capacity(6 + nlen as usize);
        msg.extend_from_slice(b"NT!");
        let p = server_port.to_le_bytes();
        msg.push(p[0]);
        msg.push(p[1]);
        msg.push(nlen);
        msg.extend_from_slice(&nb[..nlen as usize]);

        if socket.send_to(&msg, bcast).is_ok() {
            count += 1;
            let _ = tx.send(ServerEvent::DiscoveryAnnounce {
                time: SystemTime::now(),
                count,
            });
        }

        // Dormir en trozos para reaccionar rapido al stop_flag.
        let mut slept = 0u64;
        while slept < ANNOUNCE_INTERVAL_MS && !stop_flag.load(Ordering::SeqCst) {
            thread::sleep(Duration::from_millis(100));
            slept += 100;
        }
    }
}

//─────────────────────────────────────────────────────────────────
// Detector de IPs locales (sin dependencias)
//─────────────────────────────────────────────────────────────────

/// Devuelve LA IP local que el SO usaria para llegar a Internet (truco UDP
/// connect a 8.8.8.8). Es la que realmente esta en la LAN — no devolvemos
/// las de Docker/WSL/VirtualBox/Hyper-V que aparecen por hostname lookup y
/// confunden al usuario (no son alcanzables desde el MSX real).
pub fn local_ipv4s() -> Vec<Ipv4Addr> {
    let mut out: Vec<Ipv4Addr> = Vec::new();
    if let Ok(sock) = UdpSocket::bind("0.0.0.0:0") {
        if sock.connect("8.8.8.8:80").is_ok() {
            if let Ok(SocketAddr::V4(addr)) = sock.local_addr() {
                let ip = *addr.ip();
                if !ip.is_unspecified() && !ip.is_loopback() {
                    out.push(ip);
                }
            }
        }
    }
    out
}

//─────────────────────────────────────────────────────────────────
// Accept loop + dispatch a worker threads
//─────────────────────────────────────────────────────────────────

fn accept_loop(
    listener: TcpListener,
    root: PathBuf,
    shared: SharedState,
    stop_flag: Arc<AtomicBool>,
    tx: Sender<ServerEvent>,
) {
    let root = Arc::new(root);
    while !stop_flag.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let _ = stream.set_nonblocking(false);
                let root = Arc::clone(&root);
                let shared = shared.clone();
                let tx = tx.clone();
                let tx_for_upload = tx.clone();
                thread::spawn(move || {
                    let result = handle_connection(stream, peer, &root, &shared, &tx_for_upload);
                    match result {
                        Ok(req) => {
                            let _ = tx.send(ServerEvent::Request {
                                time: SystemTime::now(),
                                peer,
                                method: req.method,
                                path: req.path,
                                status: req.status,
                                bytes_sent: req.bytes_sent,
                                bytes_received: req.bytes_received,
                            });
                        }
                        Err(e) => {
                            let _ = tx.send(ServerEvent::Warning(format!(
                                "[{}] error: {}",
                                peer, e
                            )));
                        }
                    }
                });
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
            }
            Err(e) => {
                let _ = tx.send(ServerEvent::Warning(format!("accept: {}", e)));
                thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
            }
        }
    }
    let _ = tx.send(ServerEvent::Stopped);
}

//─────────────────────────────────────────────────────────────────
// Procesamiento HTTP por conexion
//─────────────────────────────────────────────────────────────────

struct HandledRequest {
    method: String,
    path: String,
    status: u16,
    bytes_sent: u64,
    bytes_received: u64,
}

fn handle_connection(
    mut stream: TcpStream,
    peer: SocketAddr,
    root: &Path,
    shared: &SharedState,
    tx: &Sender<ServerEvent>,
) -> std::io::Result<HandledRequest> {
    stream.set_read_timeout(Some(Duration::from_secs(READ_TIMEOUT_SECS)))?;
    stream.set_write_timeout(Some(Duration::from_secs(WRITE_TIMEOUT_SECS)))?;

    let req = match read_request(&mut stream) {
        Ok(r) => r,
        Err(e) => {
            let sent = write_error(&mut stream, 400, "Bad Request", "Peticion mal formada\n")?;
            return Ok(HandledRequest {
                method: "?".to_string(),
                path: format!("(bad: {})", e),
                status: 400,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };

    // Dispatch por metodo
    match req.method.as_str() {
        "GET" | "HEAD" => handle_get(&mut stream, &req, root),
        "PUT" => handle_put(&mut stream, &req, root, shared, peer, tx),
        _ => {
            let sent = write_error(
                &mut stream,
                405,
                "Method Not Allowed",
                "Allowed: GET, HEAD, PUT\n",
            )?;
            Ok(HandledRequest {
                method: req.method,
                path: req.path,
                status: 405,
                bytes_sent: sent,
                bytes_received: 0,
            })
        }
    }
}

fn handle_get(
    stream: &mut TcpStream,
    req: &Request,
    root: &Path,
) -> std::io::Result<HandledRequest> {
    let decoded = match url_decode(&req.path_only()) {
        Some(d) => d,
        None => {
            let sent = write_error(stream, 400, "Bad Request", "URL invalida\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 400,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };

    // Endpoint especial maquina-friendly
    if decoded == "/_list" || decoded == "/_list/" {
        // Paginacion: ?from=N&limit=N. Default: from=0, limit=256 (igual que
        // el cap del cliente MSX). El cliente recibe X-Total-Count en la
        // cabecera y puede pedir paginas siguientes.
        let from = req
            .query_param("from")
            .and_then(|s| s.parse::<usize>().ok())
            .unwrap_or(0);
        let limit = req
            .query_param("limit")
            .and_then(|s| s.parse::<usize>().ok())
            .filter(|n| *n > 0)
            .unwrap_or(256);
        let sent = serve_machine_list(stream, root, req.method == "HEAD", from, limit)?;
        return Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 200,
            bytes_sent: sent,
            bytes_received: 0,
        });
    }

    let rel = match safe_rel_path(&decoded) {
        Some(p) => p,
        None => {
            let sent = write_error(stream, 403, "Forbidden", "Ruta no permitida\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 403,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };

    let full = root.join(&rel);

    let canon = match fs::canonicalize(&full) {
        Ok(c) => c,
        Err(_) => {
            // Sin fichero — pero comprobamos si hay un .part en curso, para
            // que el cliente pueda decidir si reanudar.
            let part_path = with_part_extension(&full);
            if let Ok(meta) = fs::metadata(&part_path) {
                if meta.is_file() {
                    // 404 + cabecera X-Resume-Offset para que el cliente sepa
                    // cuantos bytes ya tiene el server.
                    let header = format!(
                        "HTTP/1.0 404 Not Found\r\n\
                         Content-Type: text/plain; charset=utf-8\r\n\
                         Content-Length: 19\r\n\
                         X-Resume-Offset: {}\r\n\
                         Connection: close\r\n\r\n\
                         Partial upload\n   ",
                        meta.len()
                    );
                    let bytes = header.as_bytes();
                    stream.write_all(bytes)?;
                    return Ok(HandledRequest {
                        method: req.method.clone(),
                        path: req.path.clone(),
                        status: 404,
                        bytes_sent: bytes.len() as u64,
                        bytes_received: 0,
                    });
                }
            }
            let sent = write_error(stream, 404, "Not Found", "No existe\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 404,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };
    if !canon.starts_with(root) {
        let sent = write_error(stream, 403, "Forbidden", "Fuera de raiz\n")?;
        return Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 403,
            bytes_sent: sent,
            bytes_received: 0,
        });
    }

    let meta = fs::metadata(&canon)?;

    if meta.is_dir() {
        let sent = serve_listing(stream, root, &canon, req.method == "HEAD")?;
        return Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 200,
            bytes_sent: sent,
            bytes_received: 0,
        });
    }

    let (status, sent) = serve_file(
        stream,
        &canon,
        meta.len(),
        req.range(),
        req.method == "HEAD",
    )?;
    Ok(HandledRequest {
        method: req.method.clone(),
        path: req.path.clone(),
        status,
        bytes_sent: sent,
        bytes_received: 0,
    })
}

//─────────────────────────────────────────────────────────────────
// PUT — subida de ficheros
//
// Soporte:
//   - PUT plano:  cuerpo entero -> .part -> rename a final
//   - PUT con Content-Range "bytes N-M/total": resume (escribir en offset N)
//   - If-Match: * fuerza sobreescritura aunque overwrite=false
//─────────────────────────────────────────────────────────────────

fn handle_put(
    stream: &mut TcpStream,
    req: &Request,
    root: &Path,
    shared: &SharedState,
    peer: SocketAddr,
    tx: &Sender<ServerEvent>,
) -> std::io::Result<HandledRequest> {
    // 1. Writable?
    if !shared.writable.load(Ordering::SeqCst) {
        let sent = write_error(stream, 403, "Forbidden", "Server is read-only\n")?;
        return Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 403,
            bytes_sent: sent,
            bytes_received: 0,
        });
    }

    // 2. Content-Length obligatorio
    let body_len = match req.content_length() {
        Some(n) => n,
        None => {
            let sent = write_error(stream, 411, "Length Required", "Content-Length required\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 411,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };

    if body_len > shared.max_upload {
        let sent = write_error(stream, 413, "Payload Too Large", "Upload exceeds limit\n")?;
        // Tenemos que drenar el body, pero como vamos a cerrar, simplemente
        // cerramos la conexion sin leerlo (la otra parte vera el RST).
        return Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 413,
            bytes_sent: sent,
            bytes_received: 0,
        });
    }

    // 3. Path validation
    let decoded = match url_decode(&req.path_only()) {
        Some(d) => d,
        None => {
            let sent = write_error(stream, 400, "Bad Request", "URL invalida\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 400,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };
    let rel = match safe_rel_path_for_put(&decoded) {
        Some(p) => p,
        None => {
            let sent = write_error(stream, 400, "Bad Request", "Bad path\n")?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 400,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    };
    let dest = root.join(&rel);
    let part = with_part_extension(&dest);

    // 4. Content-Range (resume) — opcional
    let (write_offset, expected_total) = match req.content_range() {
        Some((start, end, total)) => {
            // Validaciones basicas
            let span = end.checked_sub(start).and_then(|x| x.checked_add(1));
            if span != Some(body_len) {
                let sent = write_error(
                    stream,
                    400,
                    "Bad Request",
                    "Content-Range/Content-Length mismatch\n",
                )?;
                return Ok(HandledRequest {
                    method: req.method.clone(),
                    path: req.path.clone(),
                    status: 400,
                    bytes_sent: sent,
                    bytes_received: 0,
                });
            }
            // El offset de escritura debe casar con la longitud actual del .part
            let current = fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
            if start != current {
                let header = format!(
                    "HTTP/1.0 416 Range Not Satisfiable\r\n\
                     Content-Range: bytes */{}\r\n\
                     X-Resume-Offset: {}\r\n\
                     Content-Length: 0\r\n\
                     Connection: close\r\n\r\n",
                    total, current
                );
                stream.write_all(header.as_bytes())?;
                return Ok(HandledRequest {
                    method: req.method.clone(),
                    path: req.path.clone(),
                    status: 416,
                    bytes_sent: header.len() as u64,
                    bytes_received: 0,
                });
            }
            (start, total)
        }
        None => {
            // Sin Content-Range: PUT entero desde 0. Si existe un .part de un
            // intento previo, lo borramos para empezar limpio.
            let _ = fs::remove_file(&part);
            (0u64, body_len)
        }
    };

    // 5. Politica de colision (solo en el momento del PUT inicial completo)
    let overwriting = dest.exists();
    if overwriting && write_offset == 0 {
        let force = req.force_overwrite();
        let allow_overwrite = shared.overwrite.load(Ordering::SeqCst) || force;
        if !allow_overwrite {
            let sent = write_error(
                stream,
                409,
                "Conflict",
                "File exists; use If-Match: * to overwrite or rename\n",
            )?;
            return Ok(HandledRequest {
                method: req.method.clone(),
                path: req.path.clone(),
                status: 409,
                bytes_sent: sent,
                bytes_received: 0,
            });
        }
    }

    // 6. Stream body -> .part (append-mode si offset>0)
    let mut f = if write_offset == 0 {
        fs::File::create(&part)?
    } else {
        let mut f = fs::OpenOptions::new().write(true).append(false).open(&part)?;
        f.seek(SeekFrom::Start(write_offset))?;
        f
    };

    let mut buf = [0u8; 8192];
    let mut left = body_len;
    let mut received: u64 = 0;
    while left > 0 {
        let want = left.min(buf.len() as u64) as usize;
        let n = match stream.read(&mut buf[..want]) {
            Ok(0) => {
                // Cliente cerró antes de tiempo — .part queda; quizas reanude
                drop(f);
                let sent = write_error(stream, 400, "Bad Request", "Truncated body\n")?;
                let _ = tx.send(ServerEvent::Warning(format!(
                    "[{}] upload truncated at {}/{} bytes for {:?}",
                    peer, received, body_len, dest
                )));
                return Ok(HandledRequest {
                    method: req.method.clone(),
                    path: req.path.clone(),
                    status: 400,
                    bytes_sent: sent,
                    bytes_received: received,
                });
            }
            Ok(n) => n,
            Err(e) => {
                drop(f);
                let _ = tx.send(ServerEvent::Warning(format!(
                    "[{}] upload read error after {} bytes: {}",
                    peer, received, e
                )));
                return Err(e);
            }
        };
        f.write_all(&buf[..n])?;
        received += n as u64;
        left -= n as u64;
    }
    f.sync_all()?;
    drop(f);

    // 7. Rename .part -> final si esta completo
    let part_size = fs::metadata(&part)?.len();
    if part_size >= expected_total {
        // Completo. Renombramos atomicamente.
        if dest.exists() {
            let _ = fs::remove_file(&dest);
        }
        fs::rename(&part, &dest)?;
        let status = if overwriting { 200 } else { 201 };
        let status_text = if overwriting { "OK" } else { "Created" };
        let body = format!("{}\n", req.path_only());
        let header = format!(
            "HTTP/1.0 {} {}\r\n\
             Content-Type: text/plain; charset=utf-8\r\n\
             Content-Length: {}\r\n\
             Connection: close\r\n\r\n",
            status,
            status_text,
            body.len()
        );
        stream.write_all(header.as_bytes())?;
        stream.write_all(body.as_bytes())?;

        let _ = tx.send(ServerEvent::UploadCompleted {
            time: SystemTime::now(),
            peer,
            path: req.path_only(),
            bytes: part_size,
            overwrote: overwriting,
        });

        Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status,
            bytes_sent: (header.len() + body.len()) as u64,
            bytes_received: received,
        })
    } else {
        // Parcial: respondemos 202 Accepted indicando cuantos bytes tenemos.
        let body = format!("Partial: {} of {}\n", part_size, expected_total);
        let header = format!(
            "HTTP/1.0 202 Accepted\r\n\
             Content-Type: text/plain; charset=utf-8\r\n\
             Content-Length: {}\r\n\
             X-Resume-Offset: {}\r\n\
             Connection: close\r\n\r\n",
            body.len(),
            part_size
        );
        stream.write_all(header.as_bytes())?;
        stream.write_all(body.as_bytes())?;
        Ok(HandledRequest {
            method: req.method.clone(),
            path: req.path.clone(),
            status: 202,
            bytes_sent: (header.len() + body.len()) as u64,
            bytes_received: received,
        })
    }
}

/// Path validation especifica para PUT — solo permite ficheros sin
/// subdirectorios (sin slashes). Mas restrictivo que GET pero mas seguro.
fn safe_rel_path_for_put(url_path: &str) -> Option<PathBuf> {
    let trimmed = url_path.trim_start_matches('/');
    if trimmed.is_empty() {
        return None;
    }
    if trimmed.contains('/') || trimmed.contains('\\') {
        return None;
    }
    if trimmed.contains("..") {
        return None;
    }
    // Nombres reservados / vacios
    if trimmed == "." || trimmed.starts_with('.') {
        return None;
    }
    Some(PathBuf::from(trimmed))
}

/// Devuelve `<path>.part` para un fichero. Usado durante uploads.
fn with_part_extension(path: &Path) -> PathBuf {
    let mut s = path.as_os_str().to_owned();
    s.push(".part");
    PathBuf::from(s)
}

//─────────────────────────────────────────────────────────────────
// Parser de peticion
//─────────────────────────────────────────────────────────────────

struct Request {
    method: String,
    path: String,
    headers: HashMap<String, String>,  // claves lowercased
}

impl Request {
    fn path_only(&self) -> String {
        match self.path.find('?') {
            Some(q) => self.path[..q].to_string(),
            None => self.path.clone(),
        }
    }

    /// Devuelve el valor del query param `name` si esta en la URL. No decodifica
    /// percent-encoding (los parametros que usamos son numericos).
    fn query_param(&self, name: &str) -> Option<&str> {
        let (_, query) = self.path.split_once('?')?;
        for kv in query.split('&') {
            if let Some((k, v)) = kv.split_once('=') {
                if k == name {
                    return Some(v);
                }
            }
        }
        None
    }

    fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(&name.to_ascii_lowercase()).map(|s| s.as_str())
    }

    /// Range header (para GET): (start, Option<end>)
    fn range(&self) -> Option<(u64, Option<u64>)> {
        parse_range(self.header("range")?)
    }

    fn content_length(&self) -> Option<u64> {
        self.header("content-length")?.trim().parse().ok()
    }

    /// Content-Range header (para PUT): (start, end, total)
    fn content_range(&self) -> Option<(u64, u64, u64)> {
        parse_content_range(self.header("content-range")?)
    }

    /// If-Match: * fuerza sobreescritura
    fn force_overwrite(&self) -> bool {
        self.header("if-match").map(|v| v.trim() == "*").unwrap_or(false)
    }
}

fn read_request(stream: &mut TcpStream) -> std::io::Result<Request> {
    let mut buf = Vec::with_capacity(1024);
    let mut byte = [0u8; 1];
    loop {
        let n = stream.read(&mut byte)?;
        if n == 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::UnexpectedEof,
                "cliente cerro antes de enviar headers",
            ));
        }
        buf.push(byte[0]);
        if buf.len() >= 4 && &buf[buf.len() - 4..] == b"\r\n\r\n" {
            break;
        }
        if buf.len() > 8192 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidData,
                "headers demasiado grandes",
            ));
        }
    }

    let text = std::str::from_utf8(&buf)
        .map_err(|_| std::io::Error::new(std::io::ErrorKind::InvalidData, "headers no ASCII"))?;
    let mut lines = text.split("\r\n");
    let start = lines.next().unwrap_or("");
    let mut parts = start.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let path = parts.next().unwrap_or("/").to_string();
    let _version = parts.next().unwrap_or("HTTP/1.0");

    if method.is_empty() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidData,
            "metodo vacio",
        ));
    }

    let mut headers: HashMap<String, String> = HashMap::new();
    for line in lines {
        if line.is_empty() {
            continue;
        }
        if let Some(colon) = line.find(':') {
            let name = line[..colon].trim().to_ascii_lowercase();
            let value = line[colon + 1..].trim().to_string();
            headers.insert(name, value);
        }
    }

    Ok(Request {
        method,
        path,
        headers,
    })
}

fn parse_content_range(v: &str) -> Option<(u64, u64, u64)> {
    let rest = v.trim().strip_prefix("bytes ")?;
    let (range_part, total_part) = rest.split_once('/')?;
    let (start_s, end_s) = range_part.split_once('-')?;
    Some((
        start_s.trim().parse().ok()?,
        end_s.trim().parse().ok()?,
        total_part.trim().parse().ok()?,
    ))
}

fn parse_range(v: &str) -> Option<(u64, Option<u64>)> {
    let v = v.trim();
    let rest = v.strip_prefix("bytes=")?;
    let first = rest.split(',').next()?.trim();
    let (a, b) = first.split_once('-')?;
    let start: u64 = a.parse().ok()?;
    let end = if b.is_empty() {
        None
    } else {
        Some(b.parse::<u64>().ok()?)
    };
    Some((start, end))
}

//─────────────────────────────────────────────────────────────────
// Decode + sanitizado de path
//─────────────────────────────────────────────────────────────────

fn url_decode(s: &str) -> Option<String> {
    let mut out = Vec::with_capacity(s.len());
    let bytes = s.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        if c == b'%' {
            if i + 2 >= bytes.len() {
                return None;
            }
            let hi = hex(bytes[i + 1])?;
            let lo = hex(bytes[i + 2])?;
            out.push((hi << 4) | lo);
            i += 3;
        } else {
            out.push(c);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

fn hex(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

fn safe_rel_path(url_path: &str) -> Option<PathBuf> {
    let trimmed = url_path.trim_start_matches('/');
    if trimmed.is_empty() {
        return Some(PathBuf::new());
    }
    let candidate = Path::new(trimmed);
    let mut out = PathBuf::new();
    for comp in candidate.components() {
        match comp {
            Component::Normal(s) => out.push(s),
            Component::CurDir => {}
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => return None,
        }
    }
    Some(out)
}

//─────────────────────────────────────────────────────────────────
// Respuestas
//─────────────────────────────────────────────────────────────────

fn write_error(
    stream: &mut TcpStream,
    code: u16,
    reason: &str,
    body: &str,
) -> std::io::Result<u64> {
    let body_bytes = body.as_bytes();
    let header = format!(
        "HTTP/1.0 {} {}\r\n\
         Content-Type: text/plain; charset=utf-8\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\
         \r\n",
        code,
        reason,
        body_bytes.len()
    );
    stream.write_all(header.as_bytes())?;
    stream.write_all(body_bytes)?;
    Ok((header.len() + body_bytes.len()) as u64)
}

fn serve_file(
    stream: &mut TcpStream,
    path: &Path,
    total_len: u64,
    range: Option<(u64, Option<u64>)>,
    head_only: bool,
) -> std::io::Result<(u16, u64)> {
    let (status, status_text, start, length) = match range {
        Some((s, e)) => {
            if s >= total_len {
                let header = format!(
                    "HTTP/1.0 416 Range Not Satisfiable\r\n\
                     Content-Range: bytes */{}\r\n\
                     Content-Length: 0\r\n\
                     Connection: close\r\n\r\n",
                    total_len
                );
                stream.write_all(header.as_bytes())?;
                return Ok((416, header.len() as u64));
            }
            let end = e.unwrap_or(total_len - 1).min(total_len - 1);
            (206u16, "Partial Content", s, end - s + 1)
        }
        None => (200u16, "OK", 0u64, total_len),
    };

    let mut headers = format!(
        "HTTP/1.0 {} {}\r\n\
         Content-Type: application/octet-stream\r\n\
         Content-Length: {}\r\n\
         Accept-Ranges: bytes\r\n\
         Connection: close\r\n",
        status, status_text, length
    );
    if status == 206 {
        headers.push_str(&format!(
            "Content-Range: bytes {}-{}/{}\r\n",
            start,
            start + length - 1,
            total_len
        ));
    }
    headers.push_str("\r\n");
    stream.write_all(headers.as_bytes())?;

    let mut sent = headers.len() as u64;
    if head_only {
        return Ok((status, sent));
    }

    let mut f = fs::File::open(path)?;
    if start > 0 {
        f.seek(SeekFrom::Start(start))?;
    }
    let mut remaining = length;
    let mut buf = [0u8; SEND_CHUNK];
    while remaining > 0 {
        let to_read = remaining.min(SEND_CHUNK as u64) as usize;
        let n = f.read(&mut buf[..to_read])?;
        if n == 0 {
            break;
        }
        stream.write_all(&buf[..n])?;
        sent += n as u64;
        remaining -= n as u64;
    }
    Ok((status, sent))
}

fn serve_machine_list(
    stream: &mut TcpStream,
    root: &Path,
    head_only: bool,
    from: usize,
    limit: usize,
) -> std::io::Result<u64> {
    let mut entries: Vec<_> = fs::read_dir(root)?
        .filter_map(|e| e.ok())
        .filter(|e| {
            let n = e.file_name();
            let name = n.to_string_lossy();
            if name.starts_with('.') {
                return false;
            }
            e.file_type().map(|t| t.is_file()).unwrap_or(false)
        })
        .collect();
    entries.sort_by_key(|e| e.file_name());

    let total = entries.len();

    // Recorta a la ventana solicitada
    let end = from.saturating_add(limit).min(total);
    let window: &[_] = if from >= total { &[] } else { &entries[from..end] };

    let mut body = String::new();
    for e in window {
        let n = e.file_name().to_string_lossy().to_string();
        let size = e.metadata().map(|m| m.len()).unwrap_or(0);
        if n.contains('\t') || n.contains('\n') || n.contains('\r') {
            continue;
        }
        body.push_str(&format!("{}\t{}\n", n, size));
    }

    let body_bytes = body.as_bytes();
    let header = format!(
        "HTTP/1.0 200 OK\r\n\
         Content-Type: text/plain; charset=ascii\r\n\
         Content-Length: {}\r\n\
         X-Total-Count: {}\r\n\
         X-Page-Start: {}\r\n\
         Connection: close\r\n\r\n",
        body_bytes.len(),
        total,
        from
    );
    stream.write_all(header.as_bytes())?;
    let mut sent = header.len() as u64;
    if !head_only {
        stream.write_all(body_bytes)?;
        sent += body_bytes.len() as u64;
    }
    Ok(sent)
}

fn serve_listing(
    stream: &mut TcpStream,
    root: &Path,
    dir: &Path,
    head_only: bool,
) -> std::io::Result<u64> {
    let rel = dir.strip_prefix(root).unwrap_or(Path::new(""));
    let rel_str = rel.to_string_lossy().replace('\\', "/");

    let mut entries: Vec<_> = fs::read_dir(dir)?.filter_map(|e| e.ok()).collect();
    entries.sort_by_key(|e| e.file_name());

    let mut body = String::new();
    body.push_str("<!doctype html><html><head><meta charset=\"utf-8\">");
    body.push_str("<title>NetTransfer</title></head><body>");
    body.push_str(&format!("<h2>/{}</h2><pre>\n", rel_str));
    if !rel_str.is_empty() {
        body.push_str("<a href=\"../\">../</a>\n");
    }
    for e in entries {
        let name = e.file_name().to_string_lossy().to_string();
        let is_dir = e.file_type().map(|t| t.is_dir()).unwrap_or(false);
        let size = e.metadata().map(|m| m.len()).unwrap_or(0);
        let display = if is_dir {
            format!("{}/", name)
        } else {
            name.clone()
        };
        let href = if is_dir { format!("{}/", name) } else { name };
        if is_dir {
            body.push_str(&format!("<a href=\"{}\">{}</a>\n", href, display));
        } else {
            body.push_str(&format!("<a href=\"{}\">{}</a>  {} bytes\n", href, display, size));
        }
    }
    body.push_str("</pre></body></html>\n");

    let body_bytes = body.as_bytes();
    let header = format!(
        "HTTP/1.0 200 OK\r\n\
         Content-Type: text/html; charset=utf-8\r\n\
         Content-Length: {}\r\n\
         Connection: close\r\n\r\n",
        body_bytes.len()
    );
    stream.write_all(header.as_bytes())?;
    let mut sent = header.len() as u64;
    if !head_only {
        stream.write_all(body_bytes)?;
        sent += body_bytes.len() as u64;
    }
    Ok(sent)
}
