// nthttp-gui — GUI eframe/egui para arrancar/parar el servidor NetTransfer
//
// Cross-platform (Windows / Linux / macOS). Permite:
//   - Elegir la carpeta a compartir (con selector nativo)
//   - Cambiar el puerto (8088 por defecto)
//   - Start / Stop del servidor
//   - Ver las IPs locales para conectar desde MSX (con boton Copy)
//   - Listar los ficheros de la carpeta con su tamaño
//   - Tema claro / oscuro

#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

use std::fs;
use std::net::{Ipv4Addr, SocketAddr};
use std::path::PathBuf;
use std::time::Duration;

use eframe::egui;
use nettransfer_server::{local_ipv4s, Server, ServerConfig, ServerEvent};

const DEFAULT_PORT: u16 = 8088;
const DEFAULT_FOLDER: &str = "./files";

#[derive(PartialEq, Eq, Clone, Copy)]
enum Theme {
    Light,
    Dark,
}

#[derive(PartialEq, Eq, Clone, Copy)]
enum ViewMode {
    List,     // nombre + tamaño, vertical
    Columns,  // solo nombres, multi-columna (newspaper)
    Icons,    // grid con iconos grandes
}

struct App {
    folder: PathBuf,
    server: Option<Server>,
    last_error: Option<String>,
    files: Vec<(String, u64)>,   // (nombre, tamaño)
    ips: Vec<Ipv4Addr>,
    theme: Theme,
    view_mode: ViewMode,
    request_count: u64,
    last_request: Option<String>,
}

impl App {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let theme = Theme::Dark;
        apply_theme(&cc.egui_ctx, theme);

        let folder = fs::canonicalize(PathBuf::from(DEFAULT_FOLDER))
            .unwrap_or_else(|_| PathBuf::from(DEFAULT_FOLDER));

        let mut app = App {
            folder,
            server: None,
            last_error: None,
            files: Vec::new(),
            ips: local_ipv4s(),
            theme,
            view_mode: ViewMode::Columns,
            request_count: 0,
            last_request: None,
        };
        app.refresh_files();
        app
    }

    fn refresh_files(&mut self) {
        self.files.clear();
        if let Ok(rd) = fs::read_dir(&self.folder) {
            let mut entries: Vec<(String, u64)> = rd
                .filter_map(|e| e.ok())
                .filter(|e| e.file_type().map(|t| t.is_file()).unwrap_or(false))
                .map(|e| {
                    let n = e.file_name().to_string_lossy().to_string();
                    let s = e.metadata().map(|m| m.len()).unwrap_or(0);
                    (n, s)
                })
                .filter(|(n, _)| !n.starts_with('.'))
                .collect();
            entries.sort_by(|a, b| a.0.to_lowercase().cmp(&b.0.to_lowercase()));
            self.files = entries;
        }
    }

    fn try_start(&mut self) {
        self.last_error = None;

        let cfg = ServerConfig {
            bind_addr: SocketAddr::from(([0, 0, 0, 0], DEFAULT_PORT)),
            root: self.folder.clone(),
        };
        match Server::start(cfg) {
            Ok(s) => {
                self.request_count = 0;
                self.last_request = None;
                self.server = Some(s);
                self.refresh_files();
            }
            Err(e) => {
                self.last_error = Some(format!("No se puede arrancar: {}", e));
            }
        }
    }

    fn stop(&mut self) {
        if let Some(s) = self.server.take() {
            s.stop();
        }
    }

    fn drain_events(&mut self) {
        let Some(server) = &self.server else { return; };
        while let Some(ev) = server.try_recv() {
            match ev {
                ServerEvent::Started { .. } => {}
                ServerEvent::Request {
                    peer, method, path, status, bytes_sent, ..
                } => {
                    self.request_count += 1;
                    self.last_request = Some(format!(
                        "{} {} {} -> {} ({} B)",
                        peer, method, path, status, bytes_sent
                    ));
                }
                ServerEvent::Warning(msg) => {
                    self.last_error = Some(msg);
                }
                ServerEvent::Stopped => {
                    // El thread interno termino — limpia handle.
                    // (drop ya se hizo)
                }
            }
        }
    }

    fn is_running(&self) -> bool {
        self.server.is_some()
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Drena eventos del servidor (no bloqueante).
        self.drain_events();

        // Si esta corriendo, pide repintar pronto para refrescar contadores.
        if self.is_running() {
            ctx.request_repaint_after(Duration::from_millis(300));
        }

        egui::TopBottomPanel::top("top").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("NetTransfer Server");
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    let label = match self.theme {
                        Theme::Dark => "☀ Claro",
                        Theme::Light => "🌙 Oscuro",
                    };
                    if ui.button(label).clicked() {
                        self.theme = match self.theme {
                            Theme::Dark => Theme::Light,
                            Theme::Light => Theme::Dark,
                        };
                        apply_theme(ctx, self.theme);
                    }
                });
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            // ── Status badge (verde / rojo, grande) ──
            let running = self.is_running();
            ui.horizontal(|ui| {
                let (bg, fg, label) = if running {
                    (
                        egui::Color32::from_rgb(40, 160, 60),
                        egui::Color32::WHITE,
                        format!("●  RUNNING  ·  :{}", DEFAULT_PORT),
                    )
                } else {
                    (
                        egui::Color32::from_rgb(190, 60, 60),
                        egui::Color32::WHITE,
                        "■  STOPPED".to_string(),
                    )
                };

                // Badge: rectangulo coloreado con texto en negrita
                let text = egui::RichText::new(label).color(fg).size(20.0).strong();
                let frame = egui::Frame::none()
                    .fill(bg)
                    .inner_margin(egui::Margin::symmetric(14.0, 8.0))
                    .rounding(8.0);
                frame.show(ui, |ui| {
                    ui.label(text);
                });

                ui.add_space(12.0);
                if running {
                    ui.vertical(|ui| {
                        ui.label(format!("Peticiones servidas: {}", self.request_count));
                        if let Some(last) = &self.last_request {
                            ui.weak(format!("Ultima: {}", last));
                        }
                    });
                }
            });

            if let Some(err) = &self.last_error {
                ui.add_space(4.0);
                ui.colored_label(egui::Color32::from_rgb(220, 120, 60), err);
            }

            ui.add_space(8.0);

            // ── Carpeta a compartir ──
            // Muestra la ruta como label (no editable a mano) y ofrece un boton
            // para elegir/cambiar carpeta. El prefijo Windows "\\?\" se quita.
            ui.horizontal(|ui| {
                ui.label("Carpeta:");
                ui.monospace(pretty_path(&self.folder));
            });
            ui.horizontal(|ui| {
                ui.add_space(56.0);
                if ui
                    .add_enabled(
                        !self.is_running(),
                        egui::Button::new("🗁  Elegir carpeta a compartir..."),
                    )
                    .clicked()
                {
                    if let Some(p) = rfd::FileDialog::new()
                        .set_directory(&self.folder)
                        .pick_folder()
                    {
                        self.folder = p;
                        self.refresh_files();
                    }
                }
            });

            ui.add_space(8.0);

            // ── Botones Start/Stop ──
            ui.horizontal(|ui| {
                if ui
                    .add_enabled(!self.is_running(), egui::Button::new("▶  Start"))
                    .clicked()
                {
                    self.try_start();
                }
                if ui
                    .add_enabled(self.is_running(), egui::Button::new("■  Stop"))
                    .clicked()
                {
                    self.stop();
                }
                ui.separator();
                if ui.button("⟳ Refrescar ficheros").clicked() {
                    self.refresh_files();
                }
            });

            ui.add_space(12.0);
            ui.separator();
            ui.add_space(6.0);

            // ── Conexion desde MSX ──
            // No tiene sentido un boton "Copiar" — el usuario no puede pegar en
            // el MSX. Lo mejor es mostrar la instruccion exacta a teclear.
            ui.label(egui::RichText::new("Desde el MSX, ejecuta:").strong());
            ui.add_space(2.0);

            let lan_ips: Vec<String> = self.ips.iter().map(|ip| ip.to_string()).collect();
            if lan_ips.is_empty() {
                ui.weak("(no se detectan IPs de LAN)");
            } else {
                for ip in &lan_ips {
                    ui.horizontal(|ui| {
                        ui.add_space(12.0);
                        ui.monospace(
                            egui::RichText::new(format!("NT {}", ip))
                                .size(22.0)
                                .strong()
                                .color(egui::Color32::from_rgb(80, 180, 240)),
                        );
                    });
                }
            }

            ui.add_space(12.0);
            ui.separator();
            ui.add_space(6.0);

            // ── Lista de ficheros con selector de vista ──
            ui.horizontal(|ui| {
                ui.label(
                    egui::RichText::new(format!(
                        "Ficheros en la carpeta ({}):",
                        self.files.len()
                    ))
                    .strong(),
                );
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.label("Vista:");
                    ui.selectable_value(&mut self.view_mode, ViewMode::Icons, "🇮 Iconos");
                    ui.selectable_value(&mut self.view_mode, ViewMode::Columns, "🇨 Columnas");
                    ui.selectable_value(&mut self.view_mode, ViewMode::List, "🇱 Lista");
                });
            });

            ui.add_space(4.0);

            egui::ScrollArea::vertical()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    if self.files.is_empty() {
                        ui.weak("(carpeta vacia)");
                        return;
                    }
                    match self.view_mode {
                        ViewMode::List => render_list_view(ui, &self.files),
                        ViewMode::Columns => render_columns_view(ui, &self.files),
                        ViewMode::Icons => render_icons_view(ui, &self.files),
                    }
                });
        });
    }
}

//─────────────────────────────────────────────────────────────────
// Renderizado de la lista de ficheros segun ViewMode
//─────────────────────────────────────────────────────────────────

fn render_list_view(ui: &mut egui::Ui, files: &[(String, u64)]) {
    egui::Grid::new("files-list")
        .num_columns(2)
        .striped(true)
        .spacing([12.0, 4.0])
        .show(ui, |ui| {
            for (name, size) in files {
                ui.monospace(name);
                ui.with_layout(
                    egui::Layout::right_to_left(egui::Align::Center),
                    |ui| {
                        ui.monospace(human_size(*size));
                    },
                );
                ui.end_row();
            }
        });
}

fn render_columns_view(ui: &mut egui::Ui, files: &[(String, u64)]) {
    // Calcula cuantas columnas caben en el ancho disponible
    const COL_WIDTH: f32 = 180.0;
    let avail = ui.available_width().max(COL_WIDTH);
    let cols = ((avail / COL_WIDTH).floor() as usize).max(1);

    // Distribuye por columnas en orden de lectura (col-major newspaper)
    let rows = (files.len() + cols - 1) / cols;
    egui::Grid::new("files-cols")
        .num_columns(cols)
        .spacing([12.0, 2.0])
        .show(ui, |ui| {
            for r in 0..rows {
                for c in 0..cols {
                    let idx = c * rows + r;
                    if idx < files.len() {
                        ui.monospace(&files[idx].0);
                    } else {
                        ui.label("");
                    }
                }
                ui.end_row();
            }
        });
}

fn render_icons_view(ui: &mut egui::Ui, files: &[(String, u64)]) {
    // Grid de iconos: 📄 grande + nombre debajo
    const TILE_WIDTH: f32 = 110.0;
    let avail = ui.available_width().max(TILE_WIDTH);
    let cols = ((avail / TILE_WIDTH).floor() as usize).max(1);

    let rows = (files.len() + cols - 1) / cols;
    egui::Grid::new("files-icons")
        .num_columns(cols)
        .spacing([8.0, 12.0])
        .show(ui, |ui| {
            for r in 0..rows {
                for c in 0..cols {
                    let idx = r * cols + c;
                    if idx < files.len() {
                        ui.allocate_ui(egui::vec2(TILE_WIDTH - 8.0, 70.0), |ui| {
                            ui.vertical_centered(|ui| {
                                let icon = icon_for(&files[idx].0);
                                ui.label(egui::RichText::new(icon).size(32.0));
                                ui.monospace(
                                    egui::RichText::new(elide(&files[idx].0, 14)).size(11.0),
                                );
                            });
                        });
                    } else {
                        ui.label("");
                    }
                }
                ui.end_row();
            }
        });
}

// Selecciona un emoji segun la extension del fichero
fn icon_for(name: &str) -> &'static str {
    let ext = name.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    match ext.as_str() {
        "txt" | "md" | "log" | "csv" => "📄",
        "bin" | "rom" | "com" | "exe" | "sys" => "💾",
        "dsk" | "img" | "iso" => "💿",
        "zip" | "tar" | "gz" | "7z" | "rar" => "🗜",
        "asc" | "s" | "asm" => "⚙",
        "bas" => "📜",
        "wav" | "mp3" | "vgm" | "vgz" => "🎵",
        "png" | "jpg" | "jpeg" | "gif" | "bmp" => "🖼",
        _ => "📁",
    }
}

fn elide(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        s.to_string()
    } else {
        let mut out: String = s.chars().take(max - 1).collect();
        out.push('…');
        out
    }
}

fn apply_theme(ctx: &egui::Context, theme: Theme) {
    let visuals = match theme {
        Theme::Light => egui::Visuals::light(),
        Theme::Dark => egui::Visuals::dark(),
    };
    ctx.set_visuals(visuals);
}

// Limpia un PathBuf para mostrar: en Windows quita el prefijo "\\?\" que
// canonicalize() inserta y que es visualmente feo.
fn pretty_path(p: &std::path::Path) -> String {
    let s = p.to_string_lossy().to_string();
    s.strip_prefix(r"\\?\").map(|x| x.to_string()).unwrap_or(s)
}

fn human_size(n: u64) -> String {
    if n < 1024 {
        format!("{} B", n)
    } else if n < 1024 * 1024 {
        format!("{:.1} KB", n as f64 / 1024.0)
    } else if n < 1024 * 1024 * 1024 {
        format!("{:.1} MB", n as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", n as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}

fn main() -> Result<(), eframe::Error> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([720.0, 600.0])
            .with_min_inner_size([520.0, 420.0])
            .with_title("NetTransfer Server"),
        ..Default::default()
    };
    eframe::run_native(
        "NetTransfer Server",
        options,
        Box::new(|cc| Ok(Box::new(App::new(cc)))),
    )
}
