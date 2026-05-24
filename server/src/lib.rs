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

//─────────────────────────────────────────────────────────────────
// API publica
//─────────────────────────────────────────────────────────────────

#[derive(Clone, Debug)]
pub struct ServerConfig {
    pub bind_addr: SocketAddr,
    pub root: PathBuf,
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

        let stop_for_thread = Arc::clone(&stop_flag);
        let root_for_thread = root.clone();
        let tx_for_thread = tx.clone();

        let _ = tx.send(ServerEvent::Started {
            local_addr,
            root: root.clone(),
        });

        let join = thread::Builder::new()
            .name("nthttp-accept".to_string())
            .spawn(move || {
                accept_loop(listener, root_for_thread, stop_for_thread, tx_for_thread);
            })?;

        Ok(Server {
            stop_flag,
            rx,
            join: Some(join),
            local_addr,
            root,
        })
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

    /// Para el servidor y espera a que termine el thread de accept.
    pub fn stop(mut self) {
        self.stop_flag.store(true, Ordering::SeqCst);
        if let Some(join) = self.join.take() {
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
    }
}

//─────────────────────────────────────────────────────────────────
// Detector de IPs locales (sin dependencias)
//─────────────────────────────────────────────────────────────────

pub fn local_ipv4s() -> Vec<Ipv4Addr> {
    let mut out: Vec<Ipv4Addr> = Vec::new();

    // 1. La saliente — UDP socket "connect" a 8.8.8.8 sin enviar datos.
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

    // 2. Todas las del host por hostname lookup.
    if let Ok(hostname) = hostname_lookup() {
        if let Ok(iter) = (hostname.as_str(), 0u16).to_socket_addrs_safe() {
            for sa in iter {
                if let SocketAddr::V4(a) = sa {
                    let ip = *a.ip();
                    if !ip.is_unspecified() && !ip.is_loopback() && !out.contains(&ip) {
                        out.push(ip);
                    }
                }
            }
        }
    }

    out
}

fn hostname_lookup() -> std::io::Result<String> {
    if let Ok(h) = std::env::var("COMPUTERNAME") {
        return Ok(h);
    }
    if let Ok(h) = std::env::var("HOSTNAME") {
        return Ok(h);
    }
    fs::read_to_string("/etc/hostname").map(|s| s.trim().to_string())
}

trait ToSocketAddrsSafe {
    fn to_socket_addrs_safe(self) -> std::io::Result<std::vec::IntoIter<SocketAddr>>;
}

impl ToSocketAddrsSafe for (&str, u16) {
    fn to_socket_addrs_safe(self) -> std::io::Result<std::vec::IntoIter<SocketAddr>> {
        use std::net::ToSocketAddrs;
        let v: Vec<SocketAddr> = self.to_socket_addrs()?.collect();
        Ok(v.into_iter())
    }
}

//─────────────────────────────────────────────────────────────────
// Accept loop + dispatch a worker threads
//─────────────────────────────────────────────────────────────────

fn accept_loop(
    listener: TcpListener,
    root: PathBuf,
    stop_flag: Arc<AtomicBool>,
    tx: Sender<ServerEvent>,
) {
    let root = Arc::new(root);
    while !stop_flag.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let _ = stream.set_nonblocking(false);
                let root = Arc::clone(&root);
                let tx = tx.clone();
                thread::spawn(move || {
                    let result = handle_connection(stream, peer, &root);
                    match result {
                        Ok(req) => {
                            let _ = tx.send(ServerEvent::Request {
                                time: SystemTime::now(),
                                peer,
                                method: req.method,
                                path: req.path,
                                status: req.status,
                                bytes_sent: req.bytes_sent,
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
}

fn handle_connection(
    mut stream: TcpStream,
    _peer: SocketAddr,
    root: &Path,
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
            });
        }
    };

    if req.method != "GET" && req.method != "HEAD" {
        let sent = write_error(&mut stream, 405, "Method Not Allowed", "Solo GET/HEAD\n")?;
        return Ok(HandledRequest {
            method: req.method,
            path: req.path,
            status: 405,
            bytes_sent: sent,
        });
    }

    let decoded = match url_decode(&req.path_only()) {
        Some(d) => d,
        None => {
            let sent = write_error(&mut stream, 400, "Bad Request", "URL invalida\n")?;
            return Ok(HandledRequest {
                method: req.method,
                path: req.path,
                status: 400,
                bytes_sent: sent,
            });
        }
    };

    // Endpoint especial maquina-friendly
    if decoded == "/_list" || decoded == "/_list/" {
        let sent = serve_machine_list(&mut stream, root, req.method == "HEAD")?;
        return Ok(HandledRequest {
            method: req.method,
            path: req.path,
            status: 200,
            bytes_sent: sent,
        });
    }

    let rel = match safe_rel_path(&decoded) {
        Some(p) => p,
        None => {
            let sent = write_error(&mut stream, 403, "Forbidden", "Ruta no permitida\n")?;
            return Ok(HandledRequest {
                method: req.method,
                path: req.path,
                status: 403,
                bytes_sent: sent,
            });
        }
    };

    let full = root.join(&rel);

    let canon = match fs::canonicalize(&full) {
        Ok(c) => c,
        Err(_) => {
            let sent = write_error(&mut stream, 404, "Not Found", "No existe\n")?;
            return Ok(HandledRequest {
                method: req.method,
                path: req.path,
                status: 404,
                bytes_sent: sent,
            });
        }
    };
    if !canon.starts_with(root) {
        let sent = write_error(&mut stream, 403, "Forbidden", "Fuera de raiz\n")?;
        return Ok(HandledRequest {
            method: req.method,
            path: req.path,
            status: 403,
            bytes_sent: sent,
        });
    }

    let meta = fs::metadata(&canon)?;

    if meta.is_dir() {
        let sent = serve_listing(&mut stream, root, &canon, req.method == "HEAD")?;
        return Ok(HandledRequest {
            method: req.method,
            path: req.path,
            status: 200,
            bytes_sent: sent,
        });
    }

    let (status, sent) = serve_file(
        &mut stream,
        &canon,
        meta.len(),
        req.range,
        req.method == "HEAD",
    )?;
    Ok(HandledRequest {
        method: req.method,
        path: req.path,
        status,
        bytes_sent: sent,
    })
}

//─────────────────────────────────────────────────────────────────
// Parser de peticion
//─────────────────────────────────────────────────────────────────

struct Request {
    method: String,
    path: String,
    range: Option<(u64, Option<u64>)>,
}

impl Request {
    fn path_only(&self) -> String {
        match self.path.find('?') {
            Some(q) => self.path[..q].to_string(),
            None => self.path.clone(),
        }
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

    let mut range = None;
    for line in lines {
        if line.is_empty() {
            continue;
        }
        if let Some(colon) = line.find(':') {
            let name = line[..colon].trim();
            let value = line[colon + 1..].trim();
            if name.eq_ignore_ascii_case("Range") {
                range = parse_range(value);
            }
        }
    }

    Ok(Request {
        method,
        path,
        range,
    })
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

    let mut body = String::new();
    for e in &entries {
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
