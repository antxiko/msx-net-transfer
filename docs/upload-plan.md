# MSX Net Transfer — Plan de evolución: subida de ficheros

> Documento de diseño para extender `msx-net-transfer` con soporte de subida
> (MSX → PC) y, opcionalmente, un servidor HTTP en MSX que acepte uploads,
> inspirado en NestorWeb.
>
> Autor del proyecto: antxiko. Documento pensado como input de trabajo
> (Claude Code, design review, agente de codificación, etc.).

---

## 0. Contexto del proyecto actual

`msx-net-transfer` v0.1.0 implementa:

- **Servidor Rust** (`nthttp` / `nthttp-gui`) en el PC, **read-only**, HTTP/1.0,
  sirve `GET` y `HEAD` desde una carpeta. Endpoints: `/`, `/<file>`, `/_list`.
  Soporta `Range` (206 Partial Content).
- **Cliente `NT.COM`** en MSX-DOS 2 (C + Z80 asm via MSXgl + SDCC), descarga
  por UNAPI TCP/IP. UI 80-col, navegación con flechas, `ENTER` para bajar.
- Validación de path: rechaza `..`, paths absolutos, escapes del directorio
  servido.

El servidor es deliberadamente read-only. Este documento detalla las dos
extensiones naturales:

- **Feature A** — subida MSX → PC (extiende los binarios actuales).
- **Feature B** — servidor HTTP en MSX con uploads (subproyecto nuevo).

---

## 1. Feature A — Subida MSX → PC

### 1.1 Diseño de protocolo

Reutilizar la misma topología y stack. El MSX inicia la conexión y manda un
`PUT` con el cuerpo crudo. Sin multipart, sin form encoding.

```
MSX (NT.COM)                       PC (nthttp)
─────────────                      ────────────
PUT /FILE.BIN HTTP/1.0
Content-Length: 12345
Content-Type: application/octet-stream
\r\n
<12345 bytes raw>           ──►
                                   201 Created\r\n
                                   Content-Length: 0\r\n
                                   \r\n               ◄──
```

Decisiones:

- **`PUT`** en vez de `POST`: semántica de "guardar este recurso aquí",
  sin tener que parsear multipart.
- **Nombre en la URL**, no en el body.
- **Sobrescritura configurable** en servidor; cliente no se entera.
- **`Content-Length` obligatorio** (sin `Transfer-Encoding: chunked`).
- **`Expect: 100-continue` no soportado** (innecesaria complicación en MSX).

### 1.2 Servidor Rust — cambios

#### 1.2.1 Routing por método (en `lib.rs`)

```rust
match method {
    "GET" | "HEAD" => handle_get(...),
    "PUT"          => handle_put(...),   // NUEVO
    _              => respond_405(stream, &["GET", "HEAD", "PUT"]),
}
```

#### 1.2.2 `handle_put` (pseudocódigo)

```rust
fn handle_put(
    stream: &mut TcpStream,
    raw_path: &str,
    headers: &HeaderMap,
    base_dir: &Path,
    cfg: &ServerConfig,
) -> Result<()> {
    if !cfg.writable {
        return respond_403(stream, "Server is read-only");
    }

    // 1. Validar y resolver path (reusar la función existente de GET)
    let dest = validate_path(raw_path, base_dir)?;

    // 2. Content-Length obligatorio
    let len: u64 = headers.get_int("content-length")
        .ok_or(Error::LengthRequired)?;
    if len > cfg.max_upload_size {
        return respond_413(stream);
    }

    // 3. Política de colisión
    if dest.exists() && !cfg.allow_overwrite {
        return respond_409(stream, "Already exists");
    }

    // 4. Escribir a fichero .part y renombrar al final (atomicidad)
    let tmp = dest.with_extension("part");
    let result = stream_to_file(stream, &tmp, len);

    match result {
        Ok(()) => {
            fs::rename(&tmp, &dest)?;
            cfg.event_log.push(UploadEvent::ok(&dest, len));
            respond_201(stream, raw_path)
        }
        Err(e) => {
            let _ = fs::remove_file(&tmp);  // limpieza
            cfg.event_log.push(UploadEvent::fail(&dest, &e));
            respond_400(stream, &e.to_string())
        }
    }
}

fn stream_to_file(s: &mut TcpStream, tmp: &Path, len: u64) -> Result<()> {
    let mut f = File::create(tmp)?;
    let mut buf = [0u8; 8192];
    let mut left = len;
    while left > 0 {
        let want = left.min(buf.len() as u64) as usize;
        let n = s.read(&mut buf[..want])?;
        if n == 0 { return Err(Error::Truncated); }
        f.write_all(&buf[..n])?;
        left -= n as u64;
    }
    f.sync_all()?;
    Ok(())
}
```

#### 1.2.3 Configuración nueva

CLI:

```
nthttp <port> <dir> [--writable] [--max-upload <bytes>] [--overwrite]
```

- `--writable`: habilita `PUT`. **Default: off** para no romper retrocompat.
- `--max-upload`: default `16 * 1024 * 1024` (16 MiB).
- `--overwrite`: default `false` → 409 si existe.

#### 1.2.4 GUI (`gui.rs`)

- Toggle **"Allow uploads"** con badge rojo cuando activo.
- Sección "Recent uploads" con timestamp, nombre, tamaño, resultado.
- Notificación de OS (opcional) al recibir un upload.
- Persistir el toggle en config local del usuario.

#### 1.2.5 Códigos HTTP usados

| Código | Cuándo |
|---|---|
| `201 Created` | Upload OK, fichero nuevo |
| `200 OK` | Upload OK, sobrescritura (si está habilitada) |
| `400 Bad Request` | Truncado, headers malos |
| `403 Forbidden` | Servidor no es writable |
| `405 Method Not Allowed` | Verbo desconocido |
| `409 Conflict` | Ya existe y `--no-overwrite` |
| `411 Length Required` | Falta `Content-Length` |
| `413 Payload Too Large` | Excede `--max-upload` |
| `500 Internal Server Error` | Fallo de disco |

### 1.3 Cliente `NT.COM` — cambios

#### 1.3.1 Nuevo flujo de UI

Añadir tecla **`U`** en el browser. Pulsarla dispara:

1. Línea inferior: `Upload file: _____`
2. Usuario teclea nombre 8.3 local (`MUSIC.MID`).
3. Cliente hace `_FFIRST` para validar existencia y leer tamaño.
4. Línea inferior: `Upload MUSIC.MID (12,345 bytes)? ENTER=ok ESC=cancel`
5. Si confirma → barra de progreso `[####....] 45%` en la línea inferior.
6. Al terminar → muestra `Upload OK` o `Error 4xx/5xx` durante 2 s, vuelve al
   listado y refresca (los nuevos ficheros aparecen en el directorio).

#### 1.3.2 Pseudo-código (C, estilo MSXgl)

```c
int do_upload(const char* local_name) {
    // 1. Existe?
    msxdos_fcb_t fcb;
    if (msxdos_ffirst(local_name, &fcb) != 0) {
        ui_error("File not found");
        return -1;
    }
    uint32_t size = fcb.size;

    // 2. Abrir lectura
    uint8_t fh;
    if (msxdos_open(local_name, O_RDONLY, &fh) != 0) {
        ui_error("Cannot open");
        return -1;
    }

    // 3. Conectar
    uint8_t conn;
    if (unapi_tcp_open(server_ip, 8088, &conn) != 0) {
        msxdos_close(fh);
        ui_error("Connection failed");
        return -1;
    }

    // 4. Cabecera
    char hdr[160];
    int hlen = sprintf(hdr,
        "PUT /%s HTTP/1.0\r\n"
        "Content-Length: %lu\r\n"
        "Content-Type: application/octet-stream\r\n"
        "User-Agent: NT.COM/0.2\r\n"
        "\r\n",
        local_name, size);

    if (tcp_send_all(conn, hdr, hlen) != 0) goto fail;

    // 5. Body en chunks
    uint8_t buf[1024];
    uint32_t sent = 0;
    while (sent < size) {
        uint16_t want = (size - sent > 1024) ? 1024 : (uint16_t)(size - sent);
        uint16_t got;
        if (msxdos_read(fh, buf, want, &got) != 0 || got == 0) goto fail;
        if (tcp_send_all(conn, buf, got) != 0) goto fail;
        sent += got;
        ui_progress(sent, size);

        // Polling de ESC
        if (kbd_pressed(KEY_ESC)) {
            unapi_tcp_close(conn);
            msxdos_close(fh);
            ui_error("Aborted");
            return -1;
        }
    }

    msxdos_close(fh);

    // 6. Respuesta
    char resp[160];
    uint16_t rn;
    if (tcp_recv_until(conn, resp, sizeof(resp), "\r\n", &rn) != 0) goto fail;
    int status = parse_status_line(resp);
    unapi_tcp_drain_and_close(conn);

    if (status == 201 || status == 200) {
        ui_ok("Upload OK");
        return 0;
    }
    ui_error_with_code(status);
    return -1;

fail:
    unapi_tcp_close(conn);
    msxdos_close(fh);
    ui_error("Transfer failed");
    return -1;
}

// tcp_send_all: loop hasta drenar `len` bytes, reintentando "buffer lleno".
static int tcp_send_all(uint8_t conn, const void* data, uint16_t len) {
    uint16_t sent = 0;
    while (sent < len) {
        uint16_t n;
        uint8_t r = unapi_tcp_send(conn, (uint8_t*)data + sent, len - sent, &n);
        if (r == UNAPI_ERR_NO_CONN) return -1;
        if (r == UNAPI_ERR_BUFFER_FULL || n == 0) {
            // ceder CPU brevemente
            unapi_yield();
            continue;
        }
        sent += n;
    }
    return 0;
}
```

#### 1.3.3 Consideraciones MSX-específicas

- **Tamaño máximo del send UNAPI:** consultar con `TCP_GETSTATE`. En la
  práctica suele aceptar 1-2 KB. Mantener buffer en 1 KB.
- **`tcp_send` puede devolver "buffer lleno"** → reintentar tras yield.
  No tratarlo como error.
- **Sin malloc.** Todos los buffers estáticos en BSS.
- **Cabecera limitada a 160-256 bytes** — suficiente con nombre 8.3.
- **ESC abort:** cierra TCP inmediatamente, sin esperar respuesta. El server
  detectará truncado y borrará el `.part`.
- **Refresh post-upload:** llamar a `GET /_list` y redibujar el browser.

### 1.4 Plan de tests Feature A

| # | Caso | Cliente | Servidor | Esperado |
|---|---|---|---|---|
| 1 | Fichero 1 KB | NT.COM | `--writable` | 201, MD5 idéntico |
| 2 | Fichero 1 MB | NT.COM | `--writable` | 201, MD5 idéntico |
| 3 | Server read-only | NT.COM | (default) | 403, NT muestra error |
| 4 | Path con `..` | `curl` manual | `--writable` | 400, no escape |
| 5 | Overwrite OFF | NT.COM x2 | `--writable` | 1ª: 201, 2ª: 409 |
| 6 | Overwrite ON | NT.COM x2 | `--writable --overwrite` | Ambos 200/201 |
| 7 | Abort en mitad | NT.COM (ESC) | `--writable` | Sin `.part` residual |
| 8 | Exceso tamaño | `curl` | `--writable --max-upload 1024` | 413 |
| 9 | Sin Content-Length | `curl -H ""` | `--writable` | 411 |
| 10 | Fichero binario con 0x00 | NT.COM | `--writable` | MD5 idéntico (no corrupción de zeros) |

### 1.5 Estimación Feature A

| Bloque | Esfuerzo |
|---|---|
| Servidor Rust (`handle_put`, config, CLI) | 3-5 h |
| GUI (toggle, log, notificaciones) | 2-3 h |
| Cliente NT.COM (UI, upload routine) | 1-2 días |
| Tests manuales + ajustes UNAPI | medio día |
| **Total** | **~1 fin de semana** |

---

## 2. Feature B — Servidor HTTP en MSX con uploads

### 2.1 Estrategia

**Recomendación: fork de NestorWeb**, no empezar de cero.

Por qué:

- Licencia MIT, redistribuible y modificable sin fricciones.
- Listener UNAPI passive ya resuelto (es la parte más delicada).
- Parser HTTP, lectura de headers, gestión de conexión: hecho.
- Soporte CGI 1.1 ya implementado → la primitiva "drenar body desde socket"
  **ya existe** en alguna forma dentro del código.

Naming sugerido: `nestorweb-rw` (read-write) como repo separado, o
`server-msx/` dentro de `msx-net-transfer` con submodule de NestorWeb.

### 2.2 Dos caminos de implementación

#### Camino B1 — CGI upload script (rápido, mínimo invasivo)

Aprovechar que NestorWeb ya pasa el body de POST a stdin del CGI.

Crear `UPLOAD.COM` en `CGI-BIN/`:

```c
// UPLOAD.COM — recibe body crudo en stdin y lo guarda a disco
#include <stdio.h>
#include "msxdos.h"

int main(void) {
    const char* path_info = getenv("PATH_INFO");      // p.ej. "/file.bin"
    const char* clen_str  = getenv("CONTENT_LENGTH");
    const char* upload_dir = getenv("NWEB_UPLOAD_DIR"); // p.ej. "A:\UPLOADS"

    if (!path_info || !*path_info || !upload_dir) {
        printf("Status: 400\r\nContent-Length: 9\r\n\r\nBad path\n");
        return 1;
    }

    // Sanitize: rechazar '..', '/', '\\'
    char fname[16];
    if (!sanitize_83(path_info + 1, fname)) {
        printf("Status: 400\r\n\r\nBad name\n");
        return 1;
    }

    char full[64];
    sprintf(full, "%s\\%s", upload_dir, fname);

    // Crear destino
    uint8_t fh;
    if (msxdos_create(full, &fh) != 0) {
        printf("Status: 500\r\n\r\nCreate failed\n");
        return 1;
    }

    // Stream stdin → fichero, leyendo CONTENT_LENGTH bytes exactos
    uint32_t remaining = atol(clen_str ? clen_str : "0");
    uint8_t buf[1024];
    while (remaining > 0) {
        uint16_t want = remaining > 1024 ? 1024 : (uint16_t)remaining;
        uint16_t got = fread(buf, 1, want, stdin);
        if (got == 0) break;
        msxdos_write(fh, buf, got);
        remaining -= got;
    }
    msxdos_close(fh);

    printf("Status: 201 Created\r\n");
    printf("Content-Type: text/plain\r\n\r\n");
    printf("OK %s\n", fname);
    return 0;
}
```

Uso desde un cliente:

```bash
curl --data-binary @local.bin \
  -X POST \
  -H "Content-Type: application/octet-stream" \
  http://<msx-ip>/CGI-BIN/UPLOAD.COM/destino.bin
```

**Ventajas:**

- No toca el core de NestorWeb. Aislado.
- Si falla, el resto del server sigue funcionando.
- Iteración rápida: recompilas solo el `.COM`.

**Desventajas / riesgos:**

- NestorWeb **recarga `NWEB.COM` desde disco** tras cada CGI → overhead.
  Mitigación: `NWEB.COM` y `UPLOAD.COM` en RAM disk.
- NestorWeb **cachea request y response en fichero temporal** (lo dice el
  README). Para uploads grandes esto significa que necesitas espacio temporal
  igual al tamaño del upload. **Puede ser un blocker para uploads > unos
  cientos de KB en sistemas con poco RAM disk.** Verificar inspeccionando
  `cgi.c` antes de comprometerse a este camino.

#### Camino B2 — `PUT` nativo en `http.c` (más esfuerzo, mejor resultado)

Modificar el parser de NestorWeb para reconocer `PUT` (y `POST` no-CGI) y
escribir directo a disco sin intermediación.

Cambios principales:

```c
// En el enum de métodos en http.h
typedef enum { HTTP_GET, HTTP_HEAD, HTTP_PUT, HTTP_POST } http_method_t;

// En el parser de request line en http.c, añadir reconocimiento

// Nueva función en http.c
void handle_put(const char* raw_path, uint32_t content_length) {
    if (!upload_dir_configured()) {
        send_simple_response(403, "Uploads disabled");
        return;
    }

    char dest[64];
    if (!resolve_upload_path(raw_path, dest)) {  // valida '..', concatena con NWEB_UPLOAD_DIR
        send_simple_response(400, "Bad path");
        return;
    }

    if (content_length > max_upload_size) {
        send_simple_response(413, "Too large");
        return;
    }

    if (file_exists(dest) && !overwrite_enabled) {
        send_simple_response(409, "Exists");
        return;
    }

    uint8_t fh;
    if (msxdos_create(dest, &fh) != 0) {
        send_simple_response(500, "Create failed");
        return;
    }

    uint8_t buf[1024];
    uint32_t remaining = content_length;
    while (remaining > 0) {
        uint16_t want = remaining > 1024 ? 1024 : (uint16_t)remaining;
        uint16_t got;
        if (tcp_recv(buf, want, &got) != 0 || got == 0) {
            msxdos_close(fh);
            msxdos_delete(dest);
            send_simple_response(400, "Truncated");
            return;
        }
        msxdos_write(fh, buf, got);
        remaining -= got;
    }
    msxdos_close(fh);

    send_simple_response(201, "Created");
}
```

**Ventajas:**

- Sin overhead de CGI (no recargas `NWEB.COM`).
- Sin caché en disco temporal → uploads grandes posibles aunque no haya RAM
  disk grande.
- Streaming puro: solo necesitas el buffer de 1 KB.

**Desventajas:**

- Tocas el core de NestorWeb, requiere más testing.
- Hay que respetar el estilo del código de Konamiman y mandarle PR (o
  mantener el fork).

### 2.3 Configuración (variables de entorno, estilo NestorWeb)

| Variable | Significado | Default |
|---|---|---|
| `NWEB_UPLOAD_DIR` | Directorio donde aceptar uploads. **Si no se define, uploads OFF.** | (vacío) |
| `NWEB_MAX_UPLOAD` | Tamaño máximo en bytes. | `1048576` (1 MiB) |
| `NWEB_OVERWRITE` | `0` o `1` | `0` |
| `NWEB_UPLOAD_AUTH` | `0` o `1`. Si `1`, basic auth obligatorio para `PUT`/`POST` aunque el resto sea anónimo. | `1` |

Flag CLI opcional: `u=<path>` equivalente a `NWEB_UPLOAD_DIR`.

### 2.4 UI web mínima

`UPLOADS\INDEX.HTM` (servido por NestorWeb):

```html
<!DOCTYPE html>
<html><head><meta charset="ascii"><title>MSX Upload</title></head>
<body style="font-family:monospace">
<h1>MSX Upload</h1>
<input type="file" id="f">
<button onclick="up()">Upload</button>
<div id="s"></div>
<script>
async function up() {
  const f = document.getElementById('f').files[0];
  if (!f) return;
  const name = f.name.toUpperCase().slice(0, 12);  // forzar 8.3-ish
  const r = await fetch('/UPLOADS/' + name, {
    method: 'PUT',
    headers: {'Content-Type': 'application/octet-stream'},
    body: f
  });
  document.getElementById('s').textContent =
    r.ok ? 'Uploaded ' + name : 'Error ' + r.status;
}
</script>
</body></html>
```

Subir desde el navegador con `PUT` raw evita tener que parsear
`multipart/form-data` en el MSX. **Esta es la clave para mantener el código
del servidor sano.**

### 2.5 Seguridad

- **Path traversal:** rechazar `..`, paths absolutos, separadores raros.
  Reusar la lógica existente de NestorWeb.
- **Whitelist de directorios:** uploads sólo van a `NWEB_UPLOAD_DIR`, nunca
  a la raíz del server ni a `CGI-BIN`.
- **Tamaño máximo:** chequear `Content-Length` antes de leer ni un byte.
- **Auth:** forzar basic auth en `PUT`/`POST` (reusar `auth.c` de NestorWeb).
  Default ON.
- **Extensiones peligrosas:** opcional, rechazar `.COM`, `.CGI`, `.BAT` en
  uploads → evita que un upload se convierta en ejecutable accesible.
- **HTTPS:** no es viable en MSX. Si vas a exponer fuera de la LAN, monta
  un Raspberry Pi con nginx + TLS por delante.

### 2.6 Limitaciones inherentes (asumirlas, no luchar contra ellas)

- **Una conexión a la vez.** Durante un upload grande el server está
  bloqueado. Aceptable para hobby.
- **Throughput.** Obsonet ~50-100 KB/s, GR8NET algo menos. 1 MB ≈ 10-20 s.
- **Espacio en disco MSX.** Uploads van directos a la FAT del MSX, que
  puede ser pequeña (720 KB floppy, varios MB en SD/CF).
- **Sin chunked transfer encoding.** `Content-Length` obligatorio.
- **Sin resume.** Un upload interrumpido se borra y se reintenta entero
  (implementar `Range`/`Content-Range` para PUT sería v3).

### 2.7 Estimación Feature B

| Bloque | Esfuerzo |
|---|---|
| B1 — CGI upload raw (sin multipart) | 1 fin de semana |
| B1 — añadir multipart parser | +1-2 fines de semana (boundaries son dolor) |
| B2 — `PUT` nativo en `http.c` | 2-3 fines de semana, incluye tests |
| UI web mínima | medio día |
| Documentación y release | medio día |

---

## 3. Roadmap propuesto

1. **Sprint 1 — Feature A.** Valor inmediato, extensión natural del proyecto
   actual. Mantiene retrocompat porque `--writable` es opt-in.
2. **Sprint 2 — Polish A.** Progress bar bonita en `NT.COM`, log de uploads
   en la GUI, retries y reconexión.
3. **Sprint 3 — Feature B1 (CGI upload).** Subproyecto nuevo. Cliente
   probado con curl + navegador + `NT.COM` modificado.
4. **Sprint 4 — Feature B2 (`PUT` nativo).** Sustituye o complementa B1 si
   el overhead/caché de CGI resulta limitante en la práctica.
5. **Sprint 5 — Cross-pollination.** Que `NT.COM` sepa hablar tanto con el
   server Rust como con el server MSX. Negociación de capabilities vía
   `GET /_caps` o `OPTIONS *`.

---

## 4. Decisiones abiertas (necesitan tu input)

- ¿**Overwrite** silencioso (200) o `409 Conflict` por defecto?
- ¿**Progress bar** sólo en upload, o también retrofit en download?
- ¿**Resume** con `Range` para uploads grandes? Útil en líneas lentas, pero
  complica cliente y servidor.
- ¿**Distribuir el server MSX** como fork público de NestorWeb (PR upstream
  a Konamiman) o como código original tuyo?
- ¿**`PUT` estricto** o aceptar también `POST` a una URL fija
  (`/upload`) más amigable para forms HTML legacy?
- ¿**Naming colisión** del fichero: timestamp suffix, contador, o sólo 409?

---

## 5. Esqueleto de PR / commits propuesto

### Para Feature A

```
feat(server): add PUT endpoint for uploads
  - lib.rs: handle_put with .part + atomic rename
  - main.rs: --writable, --max-upload, --overwrite flags
  - tests: integration tests with reqwest

feat(gui): upload toggle and recent uploads list
  - gui.rs: read-write badge, event log panel
  - persist preference

feat(client): U key uploads local file to server
  - nt.c: upload routine, progress UI, ESC abort
  - network.h: tcp_send_all helper

docs: update README with upload usage and protocol
```

### Para Feature B

```
chore(server-msx): vendor NestorWeb as fork base
feat(server-msx/cgi): UPLOAD.COM CGI script for raw uploads
feat(server-msx/http): native PUT handler in http.c
feat(server-msx/web): minimal upload UI in INDEX.HTM
docs(server-msx): README with build, env vars, security notes
```

---

## 6. Apéndices

### A. Referencias

- MSX UNAPI TCP/IP spec — https://github.com/Konamiman/MSX-UNAPI-specification
- NestorWeb — https://github.com/Konamiman/NestorWeb
- MSX-DOS 2 function calls — https://www.konamiman.com/msx/msx-e.html
- HTTP/1.0 — RFC 1945
- CGI 1.1 — RFC 3875
- MSXgl — https://github.com/aoineko-fr/MSXgl

### B. Snippets de validación rápida

#### Test PUT contra el server Rust

```bash
# Subir
curl -X PUT \
  -H "Content-Type: application/octet-stream" \
  --data-binary @local.bin \
  http://192.168.0.102:8088/REMOTE.BIN
# Esperado: HTTP/1.0 201 Created

# Verificar
curl -s http://192.168.0.102:8088/REMOTE.BIN | md5sum
md5sum local.bin
# Ambos hashes deben coincidir
```

#### Test CGI upload en MSX

```bash
curl --data-binary @local.bin \
  -X POST \
  -H "Content-Type: application/octet-stream" \
  http://<msx-ip>/CGI-BIN/UPLOAD.COM/REMOTE.BIN
```

#### Detección de capabilities (idea v3)

```
GET /_caps HTTP/1.0

→ 200 OK
  Content-Type: text/plain

  version=0.2
  upload=1
  max-upload=16777216
  overwrite=0
  resume=0
```

### C. Glosario rápido

- **UNAPI:** especificación de Konamiman para extender MSX-DOS con APIs
  hardware/software (aquí, TCP/IP).
- **Passive connection:** socket en modo listen (server-side). El MSX la
  necesita en Feature B.
- **Active connection:** socket conectando a un peer (client-side). El MSX
  la usa en Feature A y en el proyecto actual.
- **`.part`:** convención de nombre de fichero parcial durante upload, se
  renombra al definitivo al terminar OK.

---

*Documento v1.0 — generado como input de trabajo para
`docs/upload-plan.md` del repo.*
