// Rufux macOS Tauri v2 bridge: the WebView replica of Rufus's dialog talks to
// the native macOS `rufux` CLI backend through these commands. Unprivileged
// operations (`list`, `probe`, `download-windows`, `checksum`) run directly;
// disk creation (`rufux create ... --real --yes`) streams real-time progress
// through a per-job 0700 FIFO session directory, elevating via native macOS
// `osascript` administrator privileges when not already running as root.
use std::collections::HashMap;
use std::io::Read;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use tauri::{AppHandle, Emitter, Manager, State};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command;

// ---- backend resolution ----------------------------------------------------

fn candidate_bins_for_exe(exe: Option<&Path>) -> Vec<PathBuf> {
  let mut v = Vec::new();
  if let Ok(p) = std::env::var("RUFUX_BIN") {
    if !p.is_empty() {
      v.push(PathBuf::from(p));
    }
  }
  if let Some(exe_path) = exe {
    if let Some(d) = exe_path.parent() {
      // 1. Inside Rufux.app/Contents/MacOS/rufux or alongside rufux-gui in build dir
      v.push(d.join("rufux"));
      // 2. Inside Rufux.app/Contents/Resources/bin/rufux
      v.push(d.join("../Resources/bin/rufux"));
      // 3. Local workspace build directories relative to target/release or target/debug
      v.push(d.join("../../build/rufux"));
      v.push(d.join("../../../build/rufux"));
    }
  }
  for p in [
    "/opt/homebrew/bin/rufux",
    "/usr/local/bin/rufux",
    "/usr/bin/rufux",
  ] {
    v.push(PathBuf::from(p));
  }
  if let Some(home) = std::env::var_os("HOME") {
    let h = PathBuf::from(home);
    v.push(h.join("Projects/rufux/macos/build/rufux"));
    v.push(h.join("Projects/rufux/build/rufux"));
  }
  v
}

fn candidate_bins() -> Vec<PathBuf> {
  let exe = std::env::current_exe().ok();
  candidate_bins_for_exe(exe.as_deref())
}

fn rufux_bin() -> Result<PathBuf, String> {
  for p in candidate_bins() {
    if p.is_file() {
      return Ok(p);
    }
  }
  Err("rufux backend not found (set RUFUX_BIN or bundle rufux in Rufux.app/Contents/MacOS)".into())
}

fn is_root() -> bool {
  static CACHED: OnceLock<bool> = OnceLock::new();
  *CACHED.get_or_init(|| unsafe { libc::geteuid() == 0 })
}

// ---- POSIX shell & AppleScript escaping ------------------------------------

pub fn shell_single_quote(s: &str) -> String {
  let mut out = String::with_capacity(s.len() + 2);
  out.push('\'');
  for ch in s.chars() {
    if ch == '\'' {
      out.push_str("'\\''");
    } else {
      out.push(ch);
    }
  }
  out.push('\'');
  out
}

pub fn applescript_escape(s: &str) -> String {
  let mut out = String::with_capacity(s.len() + 8);
  for ch in s.chars() {
    match ch {
      '\\' => out.push_str("\\\\"),
      '"' => out.push_str("\\\""),
      '\n' => out.push_str("\\n"),
      '\r' => out.push_str("\\r"),
      '\t' => out.push_str("\\t"),
      _ => out.push(ch),
    }
  }
  out
}

fn build_shell_command(prog: &Path, args: &[String]) -> String {
  let mut parts = Vec::with_capacity(args.len() + 1);
  parts.push(shell_single_quote(&prog.to_string_lossy()));
  for a in args {
    parts.push(shell_single_quote(a));
  }
  parts.join(" ")
}

// ---- jobs (streaming children + privileged FIFO runner) --------------------

struct JobEntry {
  child: Arc<tokio::sync::Mutex<tokio::process::Child>>,
  session_dir: Option<PathBuf>,
  privileged: bool,
}

struct Jobs {
  next: AtomicU32,
  kids: Mutex<HashMap<u32, JobEntry>>,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
struct JobLine {
  id: u32,
  line: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
struct JobExit {
  id: u32,
  code: i32,
}

struct FifoSession {
  dir: PathBuf,
  fifo_path: PathBuf,
  pid_path: PathBuf,
  rc_path: PathBuf,
}

impl FifoSession {
  fn create(job_id: u32) -> Result<Self, String> {
    let pid = std::process::id();
    let nonce = std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .map(|d| d.as_nanos())
      .unwrap_or(0);
    let dir = PathBuf::from(format!("/tmp/rufux-gui-{pid}-{job_id}-{nonce}"));
    std::fs::create_dir_all(&dir).map_err(|e| format!("cannot create session dir: {e}"))?;
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700))
      .map_err(|e| format!("cannot chmod 0700 session dir: {e}"))?;

    let fifo_path = dir.join("stream.fifo");
    let c_fifo = std::ffi::CString::new(fifo_path.as_os_str().as_bytes())
      .map_err(|e| format!("invalid fifo path: {e}"))?;
    let rc = unsafe { libc::mkfifo(c_fifo.as_ptr(), 0o600) };
    if rc != 0 {
      let err = std::io::Error::last_os_error();
      let _ = std::fs::remove_dir_all(&dir);
      return Err(format!("mkfifo failed: {err}"));
    }

    Ok(Self {
      pid_path: dir.join("pid"),
      rc_path: dir.join("rc"),
      fifo_path,
      dir,
    })
  }

  fn build_wrapper_script(&self, cmd_line: &str) -> String {
    let fifo_q = shell_single_quote(&self.fifo_path.to_string_lossy());
    let pid_q = shell_single_quote(&self.pid_path.to_string_lossy());
    let rc_q = shell_single_quote(&self.rc_path.to_string_lossy());
    format!(
      "{cmd_line} > {fifo_q} 2>&1 & wpid=$!; printf '%s\\n' \"$wpid\" > {pid_q}; wait \"$wpid\"; rc=$?; printf '%s\\n' \"$rc\" > {rc_q}; exit \"$rc\""
    )
  }
}

fn unblock_fifo_reader(fifo_path: &Path) {
  if let Ok(c_path) = std::ffi::CString::new(fifo_path.as_os_str().as_bytes()) {
    unsafe {
      let fd = libc::open(c_path.as_ptr(), libc::O_RDWR | libc::O_NONBLOCK);
      if fd >= 0 {
        libc::close(fd);
      }
    }
  }
}

fn read_stream_cr_lf<R: Read, F: FnMut(String)>(mut reader: R, mut on_line: F) {
  let mut buf = [0u8; 4096];
  let mut pending = Vec::with_capacity(256);
  loop {
    match reader.read(&mut buf) {
      Ok(0) => break,
      Ok(n) => {
        for &b in &buf[..n] {
          if b == b'\n' || b == b'\r' {
            if !pending.is_empty() {
              let s = String::from_utf8_lossy(&pending);
              let t = s.trim();
              if !t.is_empty() && !t.starts_with('+') {
                on_line(t.to_string());
              }
              pending.clear();
            }
          } else {
            pending.push(b);
          }
        }
      }
      Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
      Err(_) => break,
    }
  }
  if !pending.is_empty() {
    let s = String::from_utf8_lossy(&pending);
    let t = s.trim();
    if !t.is_empty() && !t.starts_with('+') {
      on_line(t.to_string());
    }
  }
}

fn spawn_fifo_job(
  app: &AppHandle,
  jobs: &State<Jobs>,
  prog: PathBuf,
  args: Vec<String>,
  elevate: bool,
) -> Result<u32, String> {
  let id = jobs.next.fetch_add(1, Ordering::SeqCst) + 1;
  let session = FifoSession::create(id)?;
  let cmd_line = build_shell_command(&prog, &args);
  let wrapper = session.build_wrapper_script(&cmd_line);

  let need_admin = elevate && !is_root();
  let mut cmd = if need_admin {
    let osa_script = format!(
      "do shell script \"{}\" with administrator privileges",
      applescript_escape(&wrapper)
    );
    let mut c = Command::new("/usr/bin/osascript");
    c.arg("-e").arg(osa_script);
    c
  } else {
    let mut c = Command::new("/bin/sh");
    c.arg("-c").arg(wrapper);
    c
  };
  cmd.stdout(Stdio::null()).stderr(Stdio::piped());

  let mut child = cmd.spawn().map_err(|e| {
    let _ = std::fs::remove_dir_all(&session.dir);
    format!("cannot start worker: {e}")
  })?;
  let stderr = child.stderr.take();
  let kid = Arc::new(tokio::sync::Mutex::new(child));
  jobs.kids.lock().unwrap().insert(
    id,
    JobEntry {
      child: kid.clone(),
      session_dir: Some(session.dir.clone()),
      privileged: need_admin,
    },
  );

  let fifo_for_reader = session.fifo_path.clone();
  let app_for_reader = app.clone();
  let reader_handle = std::thread::spawn(move || {
    if let Ok(file) = std::fs::File::open(&fifo_for_reader) {
      read_stream_cr_lf(file, |line| {
        let _ = app_for_reader.emit("job-line", JobLine { id, line });
      });
    }
  });

  let app2 = app.clone();
  tauri::async_runtime::spawn(async move {
    let stderr_task = stderr.map(|e| {
      let app3 = app2.clone();
      tauri::async_runtime::spawn(async move {
        let mut lines = BufReader::new(e).lines();
        while let Ok(Some(l)) = lines.next_line().await {
          let t = l.trim();
          if !t.is_empty() {
            let _ = app3.emit("job-line", JobLine { id, line: t.to_string() });
          }
        }
      })
    });

    let outer_code = {
      let mut k = kid.lock().await;
      match k.wait().await {
        Ok(s) => s.code().unwrap_or(-1),
        Err(_) => -1,
      }
    };

    // Ensure reader thread unblocks if elevation dialog was canceled before FIFO opened.
    while !reader_handle.is_finished() {
      unblock_fifo_reader(&session.fifo_path);
      tokio::time::sleep(Duration::from_millis(10)).await;
    }
    let _ = reader_handle.join();
    if let Some(t) = stderr_task {
      let _ = t.await;
    }

    let code = std::fs::read_to_string(&session.rc_path)
      .ok()
      .and_then(|s| s.trim().parse::<i32>().ok())
      .unwrap_or(outer_code);

    let _ = std::fs::remove_dir_all(&session.dir);
    let _ = app2.emit("job-exit", JobExit { id, code });
  });

  Ok(id)
}

fn spawn_streaming(
  app: &AppHandle,
  jobs: &State<Jobs>,
  prog: PathBuf,
  args: Vec<String>,
) -> Result<u32, String> {
  spawn_fifo_job(app, jobs, prog, args, false)
}

// ---- commands --------------------------------------------------------------

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
#[serde(rename_all = "camelCase")]
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
  let bin = rufux_bin()?;
  let src = if spec.mode == "dos" || spec.mode == "format" {
    "none".to_string()
  } else {
    spec.src.clone()
  };
  let mut args = vec![
    "create".into(),
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
  spawn_fifo_job(&app, &jobs, bin, args, true)
}

#[tauri::command]
async fn kill_job(jobs: State<'_, Jobs>, id: u32) -> Result<(), String> {
  let entry = {
    let guard = jobs.kids.lock().unwrap();
    guard.get(&id).map(|e| (e.child.clone(), e.session_dir.clone(), e.privileged))
  };
  match entry {
    Some((k, session_dir, privileged)) => {
      if let Some(dir) = session_dir {
        if let Ok(pid_str) = std::fs::read_to_string(dir.join("pid")) {
          if let Ok(wpid) = pid_str.trim().parse::<i32>() {
            if wpid > 0 {
              let killed = unsafe { libc::kill(wpid, libc::SIGTERM) == 0 };
              if !killed && privileged && !is_root() {
                let kill_cmd = format!(
                  "do shell script \"kill -TERM {wpid} >/dev/null 2>&1 || true\" with administrator privileges"
                );
                let _ = Command::new("/usr/bin/osascript")
                  .arg("-e")
                  .arg(kill_cmd)
                  .status()
                  .await;
              }
            }
          }
        }
      }
      let _ = k.lock().await.start_kill();
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
  let mut full = vec!["download-windows".to_string()];
  full.extend(args);
  spawn_streaming(&app, &jobs, bin, full)
}

#[tauri::command]
async fn dl_url(opts: DlOpts) -> Result<String, String> {
  let bin = rufux_bin()?;
  let mut args = vec![
    "download-windows".to_string(),
    "--version".to_string(),
    opts.version,
    "--edition".to_string(),
    opts.edition.to_string(),
    "--lang".to_string(),
    opts.lang,
    "--url-only".to_string(),
  ];
  if !opts.arch.is_empty() {
    args.push("--arch".to_string());
    args.push(opts.arch);
  }
  let out = Command::new(bin)
    .args(&args)
    .output()
    .await
    .map_err(|e| format!("link lookup failed: {e}"))?;
  if !out.status.success() {
    return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
  }
  Ok(String::from_utf8_lossy(&out.stdout).lines().last().unwrap_or("").trim().to_string())
}

#[tauri::command]
fn open_url(url: String) -> Result<(), String> {
  if !url.starts_with("https://") {
    return Err("refusing to open non-HTTPS URL".into());
  }
  std::process::Command::new("/usr/bin/open")
    .arg(&url)
    .spawn()
    .map_err(|e| format!("cannot open browser: {e}"))?;
  Ok(())
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
fn startup_file() -> Option<String> {
  for a in std::env::args().skip(1) {
    if a == "gui" || a == "--gui" || a.starts_with('-') {
      continue;
    }
    if std::path::Path::new(&a).is_file() {
      return Some(a);
    }
  }
  None
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
      dl_url,
      open_url,
      checksum,
      backend_info,
      download_dir,
      startup_file,
      save_file,
      close_window
    ])
    .run(tauri::generate_context!())
    .expect("failed to run rufux-gui");
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn test_shell_quote_metacharacters() {
    let pwn_marker = format!("/tmp/rufux-pwn-test-{}", std::process::id());
    let _ = std::fs::remove_file(&pwn_marker);

    let tricky_args = vec![
      "simple".to_string(),
      "with spaces and \t tabs".to_string(),
      "it's 'single' quoted".to_string(),
      "\"double\" quotes".to_string(),
      format!("$(touch {pwn_marker})"),
      format!("`touch {pwn_marker}`"),
      r"back\slash\\path\$HOME".to_string(),
      "line1\nline2".to_string(),
    ];

    let quoted: Vec<String> = tricky_args.iter().map(|a| shell_single_quote(a)).collect();
    let shell_cmd = format!("printf '%s\\0' {}", quoted.join(" "));

    // 1. Round-trip directly through /bin/sh -c
    let sh_out = std::process::Command::new("/bin/sh")
      .arg("-c")
      .arg(&shell_cmd)
      .output()
      .expect("sh failed");
    assert!(sh_out.status.success());
    let mut parsed: Vec<String> = sh_out
      .stdout
      .split(|&b| b == 0)
      .map(|s| String::from_utf8_lossy(s).into_owned())
      .collect();
    if parsed.last().map(|s| s.is_empty()).unwrap_or(false) {
      parsed.pop();
    }
    assert_eq!(parsed, tricky_args);
    assert!(!Path::new(&pwn_marker).exists(), "command injection must not execute");

    // 2. Round-trip through AppleScript string escaping via osascript
    let out_file = format!("/tmp/rufux-osa-test-{}", std::process::id());
    let _ = std::fs::remove_file(&out_file);
    let shell_to_file = format!("{shell_cmd} > {}", shell_single_quote(&out_file));
    let osa_expr = format!("do shell script \"{}\"", applescript_escape(&shell_to_file));
    let osa_out = std::process::Command::new("/usr/bin/osascript")
      .arg("-e")
      .arg(&osa_expr)
      .output()
      .expect("osascript failed");
    assert!(
      osa_out.status.success(),
      "osascript stderr: {}",
      String::from_utf8_lossy(&osa_out.stderr)
    );
    let raw = std::fs::read(&out_file).expect("read osa output");
    let _ = std::fs::remove_file(&out_file);
    let mut osa_parsed: Vec<String> = raw
      .split(|&b| b == 0)
      .map(|s| String::from_utf8_lossy(s).into_owned())
      .collect();
    if osa_parsed.last().map(|s| s.is_empty()).unwrap_or(false) {
      osa_parsed.pop();
    }
    assert_eq!(osa_parsed, tricky_args);
    assert!(!Path::new(&pwn_marker).exists(), "command injection must not execute via osascript");
  }

  #[test]
  fn test_fifo_progress_streaming() {
    let session = FifoSession::create(901).expect("create fifo session");
    let mode = std::fs::metadata(&session.dir)
      .expect("dir metadata")
      .permissions()
      .mode()
      & 0o777;
    assert_eq!(mode, 0o700, "session dir must be 0700");

    let fifo_reader_path = session.fifo_path.clone();
    let reader_thread = std::thread::spawn(move || {
      let mut received = Vec::new();
      let f = std::fs::File::open(&fifo_reader_path).expect("open fifo");
      read_stream_cr_lf(f, |line| received.push(line));
      received
    });

    let mock_cmd = "printf '10%%\\r25%%\\r50%%\\nCreating file system...\\n100%%\\n'";
    let wrapper = session.build_wrapper_script(mock_cmd);
    let status = std::process::Command::new("/bin/sh")
      .arg("-c")
      .arg(&wrapper)
      .status()
      .expect("run wrapper");
    assert!(status.success());

    let lines = reader_thread.join().expect("join reader");
    assert_eq!(
      lines,
      vec![
        "10%".to_string(),
        "25%".to_string(),
        "50%".to_string(),
        "Creating file system...".to_string(),
        "100%".to_string(),
      ]
    );
    let rc_str = std::fs::read_to_string(&session.rc_path).expect("read rc");
    assert_eq!(rc_str.trim(), "0");
    let pid_str = std::fs::read_to_string(&session.pid_path).expect("read pid");
    assert!(pid_str.trim().parse::<u32>().unwrap_or(0) > 0);
    let _ = std::fs::remove_dir_all(&session.dir);
  }

  #[test]
  fn test_fifo_early_cancel_unblocks() {
    let session = FifoSession::create(902).expect("create fifo session");
    let fifo_reader_path = session.fifo_path.clone();
    let reader_thread = std::thread::spawn(move || {
      let mut received = Vec::new();
      if let Ok(f) = std::fs::File::open(&fifo_reader_path) {
        read_stream_cr_lf(f, |line| received.push(line));
      }
      received
    });

    while !reader_thread.is_finished() {
      unblock_fifo_reader(&session.fifo_path);
      std::thread::sleep(Duration::from_millis(5));
    }
    let lines = reader_thread.join().expect("join reader after unblock");
    assert!(lines.is_empty());
    let _ = std::fs::remove_dir_all(&session.dir);
  }

  #[test]
  fn test_candidate_bins_prefers_bundle() {
    let fake_exe = PathBuf::from("/Applications/Rufux.app/Contents/MacOS/rufux-gui");
    let bins = candidate_bins_for_exe(Some(&fake_exe));
    let bundle_macos = PathBuf::from("/Applications/Rufux.app/Contents/MacOS/rufux");
    let bundle_res = PathBuf::from("/Applications/Rufux.app/Contents/MacOS/../Resources/bin/rufux");
    let sys_brew = PathBuf::from("/opt/homebrew/bin/rufux");
    let sys_local = PathBuf::from("/usr/local/bin/rufux");

    let idx_macos = bins.iter().position(|p| p == &bundle_macos).expect("bundle MacOS/rufux present");
    let idx_res = bins.iter().position(|p| p == &bundle_res).expect("bundle Resources/bin/rufux present");
    let idx_brew = bins.iter().position(|p| p == &sys_brew).expect("/opt/homebrew/bin/rufux present");
    let idx_local = bins.iter().position(|p| p == &sys_local).expect("/usr/local/bin/rufux present");

    assert!(idx_macos < idx_brew, "bundle MacOS/rufux must precede /opt/homebrew/bin/rufux");
    assert!(idx_res < idx_brew, "bundle Resources/bin/rufux must precede /opt/homebrew/bin/rufux");
    assert!(idx_brew < idx_local, "/opt/homebrew/bin/rufux must precede /usr/local/bin/rufux");
  }
}
