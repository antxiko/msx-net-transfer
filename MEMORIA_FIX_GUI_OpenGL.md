# MEMORIA / HANDOFF — Fix de GUI eframe/egui que no arranca en Windows sin OpenGL 2.0+

**Fecha:** 2026-07-09
**Estado:** RESUELTO en `msx-net-transfer` (v0.3.6). Aplicable a cualquier GUI escrita con **eframe/egui** que use la configuracion por defecto (`glow`).
**Regla de oro:** un cambio de features en `Cargo.toml` + un flag en `NativeOptions` bastan. Codigo de la UI **no** se toca.

> Contexto: en Windows 10 Pro un usuario final ejecutaba nuestro `nthttp-gui.exe` y no aparecia ventana. Sin logging el proceso terminaba invisible: la GUI se compila con `#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]`, asi que ni STDOUT ni STDERR van a la consola, y un `Err` de arranque no deja rastro.

---

## 1. Sintoma (identifica el bug antes de tocar nada)

- Doble clic al `.exe` de la GUI → **no** aparece ventana; el proceso termina en <1 s.
- Ejecutar desde CMD/PowerShell → la consola vuelve al prompt sin imprimir nada (por `windows_subsystem="windows"`).
- No hay panic visible, no hay traza.
- Suele pasar en: escritorios remotos (RDP), maquinas virtuales sin GPU acelerada, drivers genericos de Microsoft, GPUs Intel viejas sin driver del fabricante.

---

## 2. Paso previo obligatorio: instrumentar la GUI para saber POR QUE muere

Si la GUI tampoco te da pista alguna en tu proyecto, replica el patron que usamos en `nthttp-gui`:

### 2.1 Log a fichero + panic hook

Anadir al principio de `main.rs` (o `bin/gui.rs`) del proyecto GUI:

```rust
use std::io::Write as _;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

fn log_file_path() -> PathBuf {
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            return parent.join("miapp-gui.log");   // <-- ajusta nombre
        }
    }
    PathBuf::from("miapp-gui.log")
}

fn log_line(msg: &str) {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    let m = LOCK.get_or_init(|| Mutex::new(()));
    let _g = m.lock();
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(log_file_path()) {
        let ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let _ = writeln!(f, "[{}] {}", ts, msg);
    }
}

fn install_panic_hook() {
    let default = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        log_line(&format!("PANIC: {}", info));
        default(info);
    }));
}
```

Luego, en `fn main()`, ANTES de cualquier cosa:

```rust
fn main() -> Result<(), eframe::Error> {
    install_panic_hook();
    log_line("=== miapp-gui startup ===");
    log_line(&format!("version: {}", env!("CARGO_PKG_VERSION")));
    if let Ok(p) = std::env::current_dir()  { log_line(&format!("cwd: {}", p.display())); }
    if let Ok(p) = std::env::current_exe() { log_line(&format!("exe: {}", p.display())); }
    log_line(&format!("log file: {}", log_file_path().display()));

    // ... resto del main tuyo (parse args, NativeOptions...)

    log_line("calling eframe::run_native");
    let result = eframe::run_native(/* args tuyos */);
    match &result {
        Ok(()) => log_line("eframe::run_native returned Ok"),
        Err(e) => log_line(&format!("eframe::run_native returned Err: {}", e)),
    }
    result
}
```

### 2.2 Que pedirle al usuario

- Ejecutar la GUI.
- Enviar el fichero `miapp-gui.log` (aparece **junto al .exe** o en el CWD si no hay permisos).

Si el log dice literalmente:

```
eframe::run_native returned Err: egui_glow: OpenGL: egui_glow requires opengl 2.0+
```

→ ES ESTE BUG. Pasa al fix directo abajo.

Si el log dice otra cosa (por ejemplo `WGPU: ... adapter not found`, o un `PANIC:` de otra cosa), **no** apliques el fix wgpu a ciegas: pasame el log.

---

## 3. Fix (aplicar cuando el log confirme "opengl 2.0+")

### 3.1 Cambia el backend en `Cargo.toml`

Sustituye la linea de `eframe` por:

```toml
# ANTES (defaults o glow explicito)
eframe = { version = "0.29", default-features = false, features = ["default_fonts", "glow", "wayland", "x11"] }

# DESPUES
eframe = { version = "0.29", default-features = false, features = ["default_fonts", "wgpu"] }
```

- Ajusta la version de `eframe` a la que uses tu proyecto (0.28/0.29/0.30 tienen el mismo esquema de features).
- `wgpu` usa **DirectX 12 → DirectX 11 → Vulkan → Metal** segun disponibilidad. Windows moderno (incluido RDP con RemoteFX / DX passthrough) trae DX11 → funciona.
- Quitar `wayland`/`x11` es OK: son features de **glow** en Linux; con **wgpu** no aplican.
- `default_fonts` se mantiene.

### 3.2 (Si eframe/egui `>= 0.24`) forzar el renderer explicitamente

Algunas versiones eligen renderer segun feature flag; otras esperan una senal en `NativeOptions`. Si tu `NativeOptions::default()` falla en tiempo de compilacion o eframe cree que sigue en glow, anadir:

```rust
let options = eframe::NativeOptions {
    renderer: eframe::Renderer::Wgpu,                // <-- si tu version lo requiere
    viewport: egui::ViewportBuilder::default()
        .with_inner_size([720.0, 600.0])
        .with_title("MiApp"),
    ..Default::default()
};
```

En `eframe = "0.29"` con la feature `wgpu` activada y `glow` desactivada, `renderer` por defecto ya es `Wgpu` y NO hace falta setearlo. Comprueba con `cargo build --release` que compila sin quejarse.

### 3.3 Verificacion en tu maquina

```bash
cargo build --release
./target/release/miapp-gui.exe    # Windows: debe abrir ventana
```

Y en el `miapp-gui.log` debe aparecer:

```
calling eframe::run_native
eframe::run_native returned Ok        # <-- NO Err
```

### 3.4 Verificacion en la maquina del usuario

Enviar el `.exe` nuevo (o el ZIP de release nuevo). Pedirle que ejecute y mande el log. Tiene que aparecer la ventana. El log debe cerrar en `returned Ok` o simplemente no cerrar (mientras la ventana este viva).

---

## 4. Efectos colaterales / trade-offs

- **Tamano binario**: sube en Windows ~1-2 MiB por incluir DirectX bindings de `wgpu`. Aceptable para una GUI.
- **Compilacion**: primera build baja de cache es notablemente mas lenta (wgpu depende de `naga`, `wgpu-core`, `wgpu-hal`). Warm build es rapida.
- **Linux**: `wgpu` usa Vulkan; en distros modernas con drivers Mesa reciente esta fino. Servidores headless siguen sin ir (esperado). El `wayland`/`x11` de glow no aplica.
- **macOS**: `wgpu` usa Metal — sin sorpresas.
- **Windows 7 / Server 2008 R2**: NO son objetivo de eframe/wgpu. Si necesitas eso, `glow` + Mesa opengl32sw.dll seria la ruta alternativa (bundlear ~10 MiB de Mesa junto al exe).

---

## 5. Alternativas (por si a alguien se le va la olla)

- **Mesa llvmpipe (opengl32sw.dll bundled)**: mantener `glow`, meter la DLL de Mesa junto al exe. Windows resuelve `opengl32.dll` desde el mismo dir primero, asi que el bundle gana al del sistema. Anade ~10-20 MiB. Solo hazlo si tu target incluye maquinas sin DirectX 11.
- **Detectar y fallar con MessageBox**: si no quieres cambiar el backend, capturar el `Err` y llamar `MessageBoxW` de Win32 para explicar. No arregla nada, solo informa. Peor UX que wgpu.

---

## 6. Referencia rapida

- Feature `wgpu` de eframe: https://docs.rs/eframe/latest/eframe/#feature-flags
- Compatibility matrix de wgpu (DX11/DX12 en Windows viejo): https://github.com/gfx-rs/wgpu/wiki
- Nuestro proyecto donde se resolvio: `msx-net-transfer` commit del bump a v0.3.6 (fecha 2026-07-09). El PR / commit toca **solo** `server/Cargo.toml` (features de eframe) y version.
