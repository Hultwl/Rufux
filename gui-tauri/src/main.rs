// Rufux Tauri bridge: the WebView replica of Rufus's dialog talks to the real
// rufux CLI backend through these commands. Disk work runs in
// `pkexec rufux create ... --real --yes` (or directly as root), exactly like
// the GTK/Qt front ends; progress lines stream back as window events.
use std::collections::HashMap;
use std::path::PathBuf;
use std::process::Stdio;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;

// ---- backend resolution ----------------------------------------------------

fn candidate_bins() -> Vec<PathBuf> {
  let mut v = Vec::new();
  if let Ok(p) = std::env::var("RUFUX_BIN") {
    v.push(PathBuf::from(p));
  }
  if let Ok(exe) = std::env::current_exe() {
    if let Some(d) = exe.parent() {
      v.push(d.join("rufux"));
    }
  }
  for p in [
    "/usr/local/bin/rufux",
    "/usr/bin/rufux",
    "/app/bin/rufux",
  ] {
    v.push(PathBuf::from(p));
  }
  if let Some(home) = std::env::var_os("HOME") {
    let h = PathBuf::from(home);
    v.push(h.join("Projects/rufux/build/rufux"));
    v.push(h.join("Projects/rufux/build-qt6/rufux"));
  }
  v
}

fn rufux_bin() -> Result<PathBuf, String> {
  for p in candidate_bins() {
    if p.is_file() {
      return Ok(p);
    }
  }
  Err("rufux backend not found (set RUFUX_BIN or install rufux)".into())
}

fn is_root() -> bool {
  static CACHED: OnceLock<bool> = OnceLock::new();
  *CACHED.get_or_init(|| {
    std::process::Command::new("id")
      .arg("-u")
      .output()
      .map(|o| String::from_utf8_lossy(&o.stdout).trim() == "0")
      .unwrap_or(false)
  })
}

fn pkexec_path() -> Option<PathBuf> {
  for d in ["/usr/bin/pkexec", "/bin/pkexec"] {
    let p = PathBuf::from(d);
    if p.is_file() {
      return Some(p);
    }
  }
  None
}

// (program, argv) for a privileged-or-not backend invocation.
fn backend_cmd(sub: &str, args: &[String]) -> Result<(PathBuf, Vec<String>), String> {
  let bin = rufux_bin()?;
  let appimage = std::env::var_os("APPIMAGE").map(PathBuf::from);
  if is_root() {
    let prog = appimage.unwrap_or(bin);
    let mut a = vec![sub.to_string()];
    a.extend(args.iter().cloned());
    Ok((prog, a))
  } else {
    let pk = pkexec_path().ok_or("pkexec not found: install polkit or run as root")?;
    let target = match appimage {
      Some(ai) if !ai.as_os_str().is_empty() => ai,
      _ => bin,
    };
    let mut a = vec![target.to_string_lossy().into_owned(), sub.to_string()];
    a.extend(args.iter().cloned());
    Ok((pk, a))
  }
}

// ---- jobs (streaming children) ----------------------------------------------

struct Jobs {
  next: AtomicU32,
  kids: Mutex<HashMap<u32, Arc<tokio::sync::Mutex<tokio::process::Child>>>>,
}

#[derive(Clone, Serialize)]
struct JobLine {
  id: u32,
  line: String,
}

#[derive(Clone, Serialize)]
struct JobExit {
  id: u32,
  code: i32,
}

fn spawn_streaming(
  app: &AppHandle,
  jobs: &State<Jobs>,
  prog: PathBuf,
  args: Vec<String>,
) -> Result<u32, String> {
  let id = jobs.next.fetch_add(1, Ordering::SeqCst) + 1;
  let mut child = Command::new(prog)
    .args(&args)
    .stdout(Stdio::piped())
    .stderr(Stdio::piped())
    .spawn()
    .map_err(|e| format!("cannot start worker: {e}"))?;
  let stdout = child.stdout.take();
  let stderr = child.stderr.take();
  let kid = Arc::new(tokio::sync::Mutex::new(child));
  jobs.kids.lock().unwrap().insert(id, kid.clone());

  // Merge stdout+stderr lines into one event stream (worker merges them too).
  let app2 = app.clone();
  tauri::async_runtime::spawn(async move {
    if let Some(o) = stdout {
      let app3 = app2.clone();
      tauri::async_runtime::spawn(async move {
        let mut lines = BufReader::new(o).lines();
        while let Ok(Some(l)) = lines.next_line().await {
          // Worker may pack \r-separated progress updates in one line.
          for part in l.split('\r') {
            let t = part.trim();
            if t.is_empty() || t.starts_with('+') {
              continue;
            }
            let _ = app3.emit("job-line", JobLine { id, line: t.to_string() });
          }
        }
      });
    }
    if let Some(e) = stderr {
      let app3 = app2.clone();
      tauri::async_runtime::spawn(async move {
        let mut lines = BufReader::new(e).lines();
        while let Ok(Some(l)) = lines.next_line().await {
          let t = l.trim();
          if !t.is_empty() {
            let _ = app3.emit("job-line", JobLine { id, line: t.to_string() });
          }
        }
      });
    }
    let code = {
      let mut k = kid.lock().await;
      match k.wait().await {
        Ok(s) => s.code().unwrap_or(-1),
        Err(_) => -1,
      }
    };
    let _ = app2.emit("job-exit", JobExit { id, code });
  });
  Ok(id)
}

// ---- commands ------------------------------------------------------------------

#[derive(Debug, Serialize, Deserialize)]
struct Device {
  node: String,
  sys: String,
  size_bytes: u64,
  removable: bool,
  usb: bool,
  mounted: bool,
  transport: String,
  vendor: String,
  model: String,
  serial: String,
}

#[tauri::command]
async fn list_devices(include_fixed: bool) -> Result<Vec<Device>, String> {
  let bin = rufux_bin()?;
  let mut args = vec!["list".to_string(), "--json".to_string()];
  if include_fixed {
    args.push("--allow-fixed".to_string());
  }
  let out = Command::new(bin)
    .args(&args)
    .output()
    .await
    .map_err(|e| format!("list failed: {e}"))?;
  if !out.status.success() {
    return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
  }
  let v: serde_json::Value =
    serde_json::from_slice(&out.stdout).map_err(|e| format!("bad list json: {e}"))?;
  let arr = v.as_array().cloned().unwrap_or_default();
  Ok(arr
    .iter()
    .map(|d| Device {
      node: d.get("node").and_then(|x| x.as_str()).unwrap_or("").into(),
      sys: d.get("sys").and_then(|x| x.as_str()).unwrap_or("").into(),
      size_bytes: d.get("size_bytes").and_then(|x| x.as_u64()).unwrap_or(0),
      removable: d.get("removable").and_then(|x| x.as_bool()).unwrap_or(false),
      usb: d.get("usb").and_then(|x| x.as_bool()).unwrap_or(false),
      mounted: d.get("mounted").and_then(|x| x.as_bool()).unwrap_or(false),
      transport: d.get("transport").and_then(|x| x.as_str()).unwrap_or("").into(),
      vendor: d.get("vendor").and_then(|x| x.as_str()).unwrap_or("").into(),
      model: d.get("model").and_then(|x| x.as_str()).unwrap_or("").into(),
      serial: d.get("serial").and_then(|x| x.as_str()).unwrap_or("").into(),
    })
    .collect())
}

#[derive(Debug, Serialize)]
struct IsoInfo {
  label: String,
  size_bytes: u64,
  valid: bool,
  bootable: bool,
  efi_hint: bool,
  windows: String,
  udf: String,
}

#[tauri::command]
async fn probe_iso(path: String) -> Result<IsoInfo, String> {
  let bin = rufux_bin()?;
  let out = Command::new(bin)
    .args(["probe", &path, "--detail"])
    .output()
    .await
    .map_err(|e| format!("probe failed: {e}"))?;
  let text = String::from_utf8_lossy(&out.stdout).into_owned();
  let get = |k: &str| -> String {
    for l in text.lines() {
      if let Some(v) = l.strip_prefix(&format!("{k}: ")) {
        return v.trim().to_string();
      }
    }
    String::new()
  };
  let yes = |k: &str| get(k) == "yes";
  Ok(IsoInfo {
    label: get("label"),
    size_bytes: get("size").split_whitespace().next().unwrap_or("0").parse().unwrap_or(0),
    valid: yes("valid_iso"),
    bootable: yes("bootable"),
    efi_hint: yes("efi_hint"),
    windows: get("windows"),
    udf: get("udf"),
  })
}

#[derive(Debug, Deserialize)]
struct CreateSpec {
  src: String,
  dst: String,
  mode: String,
  scheme: String,
  fs: String,
  label: String,
  persist_mb: u64,
  cluster_sectors: i32,
  badblock_passes: i32,
  quick: bool,
  extended_label: bool,
  uefi_validate: bool,
  ignore_smart: bool,
  allow_fixed: bool,
  wue: String,
  extra: Vec<String>,
}

#[tauri::command]
async fn start_create(app: AppHandle, jobs: State<'_, Jobs>, spec: CreateSpec) -> Result<u32, String> {
  let src = if spec.mode == "dos" || spec.mode == "format" {
    "none".to_string()
  } else {
    spec.src.clone()
  };
  let mut args = vec![
    src,
    spec.dst.clone(),
    "--mode".into(),
    spec.mode.clone(),
    "--scheme".into(),
    spec.scheme.clone(),
    "--fs".into(),
    spec.fs.clone(),
    "--label".into(),
    spec.label.clone(),
    "--persist-mb".into(),
    spec.persist_mb.to_string(),
    "--cluster-sectors".into(),
    spec.cluster_sectors.to_string(),
    "--badblock-passes".into(),
    spec.badblock_passes.to_string(),
    (if spec.quick { "--quick" } else { "--full" }).to_string(),
  ];
  if !spec.extended_label {
    args.push("--no-autorun".into());
  }
  if spec.uefi_validate {
    args.push("--uefi-validate".into());
  }
  if spec.ignore_smart {
    args.push("--ignore-smart".into());
  }
  if spec.mode == "windows" {
    args.push("--wue".into());
    args.push(if spec.wue.is_empty() { "none".into() } else { spec.wue.clone() });
    args.extend(spec.extra.clone());
  }
  args.push("--verify".into());
  if spec.allow_fixed {
    args.push("--allow-fixed".into());
  }
  args.push("--real".into());
  args.push("--yes".into());
  let (prog, full) = backend_cmd("create", &args)?;
  spawn_streaming(&app, &jobs, prog, full)
}

#[tauri::command]
async fn kill_job(jobs: State<'_, Jobs>, id: u32) -> Result<(), String> {
  let kid = jobs.kids.lock().unwrap().get(&id).cloned();
  match kid {
    Some(k) => {
      k.lock().await.start_kill().map_err(|e| format!("cannot stop job: {e}"))?;
      Ok(())
    }
    None => Err("unknown job".into()),
  }
}

#[derive(Debug, Serialize)]
struct DlProduct {
  name: String,
  release: String,
  editions: Vec<String>,
}

#[tauri::command]
async fn dl_products() -> Result<Vec<DlProduct>, String> {
  let bin = rufux_bin()?;
  let out = Command::new(bin)
    .args(["download-windows", "--list"])
    .output()
    .await
    .map_err(|e| format!("download list failed: {e}"))?;
  if !out.status.success() {
    return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
  }
  let mut prods: Vec<DlProduct> = Vec::new();
  for line in String::from_utf8_lossy(&out.stdout).lines() {
    if let Some(rest) = line.strip_prefix("  --edition ") {
      if let Some(cur) = prods.last_mut() {
        if let Some(sp) = rest.find("  ") {
          cur.editions.push(rest[sp + 2..].trim().to_string());
        }
      }
    } else if !line.trim().is_empty() {
      let mut it = line.splitn(2, "  ");
      let name = it.next().unwrap_or("").trim().to_string();
      let release = it.next().unwrap_or("").trim().to_string();
      if !name.is_empty() {
        prods.push(DlProduct { name, release, editions: Vec::new() });
      }
    }
  }
  Ok(prods)
}

#[derive(Debug, Serialize)]
struct DlLang {
  id: String,
  display: String,
}

#[tauri::command]
async fn dl_langs(version: String, edition: u32) -> Result<Vec<DlLang>, String> {
  let bin = rufux_bin()?;
  let out = Command::new(bin)
    .args(["download-windows", "--version", &version, "--edition", &edition.to_string(), "--list-langs"])
    .output()
    .await
    .map_err(|e| format!("language list failed: {e}"))?;
  if !out.status.success() {
    return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
  }
  Ok(String::from_utf8_lossy(&out.stdout)
    .lines()
    .filter_map(|l| {
      let (a, b) = l.split_once('|')?;
      Some(DlLang { id: a.trim().into(), display: b.trim().into() })
    })
    .collect())
}

#[derive(Debug, Deserialize)]
struct DlOpts {
  version: String,
  edition: u32,
  lang: String,
  arch: String,
  outdir: String,
}

#[tauri::command]
async fn start_download(app: AppHandle, jobs: State<'_, Jobs>, opts: DlOpts) -> Result<u32, String> {
  let bin = rufux_bin()?;
  let mut args = vec![
    "--version".to_string(),
    opts.version,
    "--edition".to_string(),
    opts.edition.to_string(),
    "--lang".to_string(),
    opts.lang,
    "--out".to_string(),
    opts.outdir,
  ];
  if !opts.arch.is_empty() {
    args.push("--arch".to_string());
    args.push(opts.arch);
  }
  // Downloads run unprivileged (dl drops no privileges needed).
  let mut full = vec!["download-windows".to_string()];
  full.extend(args);
  spawn_streaming(&app, &jobs, bin, full)
}

#[tauri::command]
async fn checksum(path: String, algo: String) -> Result<String, String> {
  let bin = rufux_bin()?;
  let out = Command::new(bin)
    .args(["checksum", &path, "--algo", &algo])
    .output()
    .await
    .map_err(|e| format!("checksum failed: {e}"))?;
  if !out.status.success() {
    let err = String::from_utf8_lossy(&out.stderr);
    return Err(err.lines().last().unwrap_or("checksum failed").trim().to_string());
  }
  let text = String::from_utf8_lossy(&out.stdout);
  let last = text.split(['\n', '\r']).filter(|l| !l.trim().is_empty()).last().unwrap_or("");
  Ok(last.split_whitespace().next().unwrap_or("").to_string())
}

#[tauri::command]
fn backend_info() -> Result<serde_json::Value, String> {
  let bin = rufux_bin()?;
  Ok(serde_json::json!({ "bin": bin.to_string_lossy(), "root": is_root() }))
}

#[tauri::command]
fn metrics(dpr: f64, vw: u32, vh: u32, dw: u32, dh: u32) {
  eprintln!("page: dpr={} viewport={}x{} dlg={}x{}", dpr, vw, vh, dw, dh);
}

#[tauri::command]
fn download_dir() -> String {
  std::env::var("XDG_DOWNLOAD_DIR")
    .ok()
    .filter(|s| !s.is_empty())
    .or_else(|| {
      std::env::var_os("HOME").map(|h| {
        let mut p = PathBuf::from(h);
        p.push("Downloads");
        p.to_string_lossy().into_owned()
      })
    })
    .unwrap_or_else(|| ".".into())
}

#[tauri::command]
fn save_file(path: String, content: String) -> Result<(), String> {
  std::fs::write(&path, content).map_err(|e| format!("cannot write {path}: {e}"))
}

#[tauri::command]
fn close_window(app: AppHandle) {
  if let Some(w) = app.get_webview_window("main") {
    let _ = w.close();
  }
}

#[tauri::command]
fn fit_window(app: AppHandle, height: u32) -> Result<(), String> {
  use tauri::PhysicalSize;
  let w = app.get_webview_window("main").ok_or("no main window")?;
  let outer = w.outer_size().map_err(|e| e.to_string())?;
  let inner = w.inner_size().map_err(|e| e.to_string())?;
  let chrome = outer.height.saturating_sub(inner.height);
  // No window manager (nested/Xvfb): outer size is 0x0, fall back to inner.
  let wpx = if outer.width > 0 { outer.width } else { inner.width };
  let scale = w.scale_factor().unwrap_or(1.0);
  // Never grow past the monitor (minus a margin): beyond that the page
  // scrolls inside the window instead.
  let mut want = (height as f64 * scale) as u32;
  let mut mon_h = 0u32;
  if let Ok(Some(mon)) = w.current_monitor() {
    mon_h = mon.size().height;
    let cap = mon_h.saturating_sub((90.0 * scale) as u32);
    if cap > 200 {
      want = want.min(cap);
    }
  }
  let size_h = want.saturating_add(chrome).max(200);
  let size = tauri::Size::Physical(PhysicalSize {
    width: wpx,
    height: size_h,
  });
  eprintln!("fit: want_css_h={} inner={}x{} outer={}x{} mon_h={} scale={} -> set {}x{}",
    height, inner.width, inner.height, outer.width, outer.height, mon_h, scale,
    wpx, want.saturating_add(chrome).max(200));
  // Fixed dialog (like real Rufus): no min/max games, a single set_size.
  // Non-resizable windows float instead of tiling, and floating windows
  // accept client resizes.
  let _ = w.set_size(size);
  std::thread::sleep(std::time::Duration::from_millis(300));
  match w.inner_size() {
    Ok(after) => eprintln!("fit: verify inner={}x{}", after.width, after.height),
    Err(e) => eprintln!("fit: verify failed: {}", e),
  }
  Ok(())
}

fn main() {
  tauri::Builder::default()
    .plugin(tauri_plugin_dialog::init())
    .manage(Jobs { next: AtomicU32::new(0), kids: Mutex::new(HashMap::new()) })
    .invoke_handler(tauri::generate_handler![
      list_devices,
      probe_iso,
      start_create,
      kill_job,
      dl_products,
      dl_langs,
      start_download,
      checksum,
      backend_info,
      download_dir,
      save_file,
      close_window,
      fit_window,
      metrics
    ])
    .run(tauri::generate_context!())
    .expect("failed to run rufux-gui");
}
