/* Rufux web replica: drives the rufux CLI backend, looks like Rufus. */
const { invoke } = window.__TAURI__.core;

const $ = (id) => document.getElementById(id);
const logLines = [];
let devs = [];
let isoPath = "", isoLabel = "", isoWindows = false, isoHybrid = false, isoValid = false;
let selectMode = 0; // 0 = SELECT file, 1 = DOWNLOAD (split button, like Rufus)
let jobId = 0, jobKind = "", jobLastError = "";
let dlJob = 0;

function humanSize(b) {
  const u = ["B", "KB", "MB", "GB", "TB"];
  let v = +b, i = 0;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  return (i < 2 || v >= 100 ? v.toFixed(0) : v.toFixed(1)) + " " + u[i];
}
function logLine(s) {
  logLines.push(s);
  const d = new Date(), p = (n) => String(n).padStart(2, "0");
  $("logView").value += `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}  ${s}\n`;
  $("logView").scrollTop = 1e9;
}

/* Tauri 2 plugin calls without a bundler: raw invoke on plugin commands. */
async function dlgOpen(options) {
  return await invoke("plugin:dialog|open", { options });
}
async function dlgSave(options) {
  return await invoke("plugin:dialog|save", { options });
}

/* The window is fixed-size (like real Rufus) so there is no auto-fit:
   content scrolls inside #dlg and the footer stays pinned. */
function fit(force) { /* noop, kept for call sites */ }

/* ---------- custom Win32 dropdowns (both faces styled) ---------- */
const DD = {
  map: new Map(),
  bind(sel) {
    if (this.map.has(sel)) return;
    const wrap = document.createElement("span");
    wrap.className = "dd";
    sel.parentNode.insertBefore(wrap, sel);
    wrap.appendChild(sel);
    sel.style.display = "none";
    sel.tabIndex = -1;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "dd-btn";
    const list = document.createElement("div");
    list.className = "dd-list hidden";
    wrap.appendChild(btn);
    wrap.appendChild(list);
    const st = { sel, btn, list, sig: "#" };
    this.map.set(sel, st);
    btn.addEventListener("click", (e) => { e.stopPropagation(); this.toggle(sel); });
    btn.addEventListener("keydown", (e) => {
      if (e.key === "ArrowDown" || e.key === "ArrowUp") { e.preventDefault(); this.step(sel, e.key === "ArrowDown" ? 1 : -1); }
      else if (e.key === "Enter" || e.key === " ") { e.preventDefault(); this.toggle(sel); }
      else if (e.key === "Escape") { this.close(sel); }
    });
    list.addEventListener("click", (e) => {
      const it = e.target.closest(".dd-item");
      if (!it) return;
      sel.selectedIndex = +it.dataset.i;
      sel.dispatchEvent(new Event("change", { bubbles: true }));
      this.sync(sel);
      this.close(sel);
      btn.focus();
    });
    document.addEventListener("click", () => this.close(sel));
    this.sync(sel);
  },
  sig(sel) {
    let s = sel.disabled + "|";
    for (const o of sel.options) s += o.text + ";";
    return s + "|" + sel.selectedIndex;
  },
  sync(sel) {
    const st = this.map.get(sel);
    if (!st) return;
    st.btn.disabled = sel.disabled;
    const s = this.sig(sel);
    const opt = sel.options[sel.selectedIndex];
    st.btn.textContent = opt ? opt.text : "";
    st.btn.title = st.btn.textContent;
    if (s === st.sig) return;
    st.sig = s;
    st.list.innerHTML = "";
    for (let i = 0; i < sel.options.length; i++) {
      const d = document.createElement("div");
      d.className = "dd-item" + (i === sel.selectedIndex ? " cur" : "");
      d.dataset.i = i;
      d.textContent = sel.options[i].text;
      st.list.appendChild(d);
    }
  },
  syncAll() { for (const sel of this.map.keys()) this.sync(sel); },
  toggle(sel) {
    const st = this.map.get(sel);
    if (!st || st.btn.disabled) return;
    const willOpen = st.list.classList.contains("hidden");
    for (const other of this.map.values()) other.list.classList.add("hidden");
    if (willOpen) {
      this.sync(sel);
      st.list.classList.remove("hidden");
      const cur = st.list.querySelector(".cur");
      if (cur) cur.scrollIntoView({ block: "nearest" });
    } else {
      st.list.classList.add("hidden");
    }
  },
  close(sel) {
    const st = this.map.get(sel);
    if (st) st.list.classList.add("hidden");
  },
  step(sel, dir) {
    const n = sel.options.length;
    if (!n) return;
    sel.selectedIndex = Math.min(n - 1, Math.max(0, sel.selectedIndex + dir));
    sel.dispatchEvent(new Event("change", { bubbles: true }));
    this.sync(sel);
  },
};

/* ---------- Win32 message box ---------- */
function mbox(text, buttons) {
  return new Promise((resolve) => {
    $("mboxBody").textContent = text;
    const bb = $("mboxBtns");
    bb.innerHTML = "";
    buttons.forEach((t, i) => {
      const b = document.createElement("button");
      b.className = "cmdbtn" + (i === 0 ? " default" : "");
      b.textContent = t;
      b.onclick = () => { $("overlay").classList.add("hidden"); resolve(i); };
      bb.appendChild(b);
    });
    $("overlay").classList.remove("hidden");
    bb.firstChild.focus();
  });
}

/* ---------- devices ---------- */
async function refreshDevices() {
  if (jobId) return;
  try {
    const list = await invoke("list_devices", { includeFixed: $("chkListHdd").checked });
    const keep = devs[$("cbDevice").selectedIndex]?.node;
    devs = list;
    const cb = $("cbDevice");
    cb.innerHTML = "";
    if (!list.length) {
      cb.innerHTML = "<option>No USB drive found</option>";
    } else {
      let ki = 0;
      list.forEach((d, i) => {
        const name = (d.model.trim() || d.vendor.trim() || "NO_LABEL");
        const o = document.createElement("option");
        o.textContent = `${name} (${d.sys}) [${humanSize(d.size_bytes)}]`;
        cb.appendChild(o);
        if (d.node === keep) ki = i;
      });
      cb.selectedIndex = ki;
    }
    const n = list.length;
    $("devCount").textContent = n === 1 ? "1 device found" : `${n} devices found`;
    DD.sync($("cbDevice"));
  } catch (e) {
    const cb = $("cbDevice");
    cb.innerHTML = "";
    const o = document.createElement("option");
    o.textContent = "Backend error: " + e;
    cb.appendChild(o);
  }
}
function selectedDevice() {
  const d = devs[$("cbDevice").selectedIndex];
  return d ? d.node : "";
}

/* ---------- labels ---------- */
function fsKey() {
  const t = $("cbFs").value;
  if (t.startsWith("FAT16")) return "fat16";
  if (t === "FAT32" || t === "Large FAT32") return "vfat";
  if (t.startsWith("exFAT")) return "exfat";
  if (t === "UDF") return "udf";
  if (t === "ext2") return "ext2";
  if (t === "ext3") return "ext3";
  if (t === "ext4") return "ext4";
  return "ntfs";
}
function labelLimit(fs) {
  return fs === "vfat" || fs === "fat16" ? 11 : fs === "exfat" ? 15 :
    ["ext2", "ext3", "ext4"].includes(fs) ? 16 : fs === "udf" ? 30 : 32;
}
function sanitizeLabel(s, fs) {
  let out = "";
  for (const c of s.replace(/ /g, "_")) {
    if (/[A-Za-z0-9]/.test(c)) out += (fs === "vfat" || fs === "fat16") ? c.toUpperCase() : c;
    else if (c === "_" || c === "-") out += c;
    if (out.length >= labelLimit(fs)) break;
  }
  return out || "NO_LABEL";
}
function relabel() { $("editLabel").value = sanitizeLabel($("editLabel").value, fsKey()); }

/* ---------- enable/disable (Rufus's own rules) ---------- */
function isoMode() { return $("cbBoot").selectedIndex === 2; }
function syncEnabled() {
  const run = !!jobId, have = !!isoPath, img = isoMode() && have;
  $("imgOptBlock").classList.toggle("hidden", !(img && isoWindows));
  const p = img && !isoWindows && isoValid;
  $("persistBlock").classList.toggle("hidden", !p);
  for (const id of ["cbDevice", "cbBoot", "cbPart", "cbTarget", "cbFs", "editLabel"]) $(id).disabled = run;
  $("btnSelect").disabled = run;
  $("btnHash").disabled = run || !have;
  const fs = fsKey();
  $("cbCluster").disabled = run || !(fs === "vfat" || fs === "ntfs" || fs === "fat16");
  $("btnClose").disabled = run;
  $("btnStart").textContent = run ? "CANCEL" : "START";
  DD.syncAll();
  fit();
}

/* ---------- image probe ---------- */
async function loadImage(path) {
  setProgress(null, "Reading image…");
  try {
    const info = await invoke("probe_iso", { path });
    isoPath = path;
    isoWindows = info.windows === "yes";
    isoValid = info.valid;
    isoHybrid = info.valid && info.bootable;
    isoLabel = info.label && info.label !== "(none)" ? info.label : "";
    const base = path.split(/[\\/]/).pop();
    const cb = $("cbBoot");
    cb.innerHTML = "";
    ["Non bootable", "FreeDOS", base].forEach((t) => {
      const o = document.createElement("option"); o.textContent = t; cb.appendChild(o);
    });
    cb.selectedIndex = 2;
    DD.sync(cb);
    logLine(`Using image: ${path} (${humanSize(info.size_bytes)})`);
    if (isoWindows) {
      setFsOptions(true);
      $("cbFs").value = "NTFS";
      $("cbPart").selectedIndex = 1; $("cbTarget").selectedIndex = 1;
    } else {
      setFsOptions(false);
      $("cbFs").value = "FAT32";
      $("cbPart").selectedIndex = 0; $("cbTarget").selectedIndex = 0;
    }
    $("editLabel").value = sanitizeLabel(isoLabel || "NO_LABEL", fsKey());
    setProgress(null, "READY");
  } catch (e) {
    mbox(`Cannot use image:\n${e}`, ["OK"]);
    setProgress(null, "READY");
  }
  syncEnabled();
}
function setFsOptions(windows) {
  const cb = $("cbFs");
  const cur = cb.value.replace(/ \(Default\)$/, "");
  const full = ["Large FAT32", "FAT32", "FAT16", "NTFS", "exFAT", "UDF", "ext2", "ext3", "ext4"];
  const list = windows ? ["FAT32", "NTFS"] : full;
  const def = windows ? "NTFS" : "FAT32";
  cb.innerHTML = "";
  list.forEach((t) => {
    const o = document.createElement("option");
    o.textContent = t === def ? t + " (Default)" : t;
    o.value = t;
    cb.appendChild(o);
  });
  cb.value = list.includes(cur) ? cur : def;
  DD.sync(cb);
}

/* ---------- progress ---------- */
function setProgress(pct, text) {
  if (pct !== null && pct !== undefined) $("pfill").style.width = pct + "%";
  if (text !== null && text !== undefined) $("ptext").textContent = text;
}

/* ---------- elapsed clock (Rufus's status-bar timer) ---------- */
let clockT0 = 0, clockTimer = null;
function clockFmt(ms) {
  const s = Math.floor(ms / 1000);
  const p = (n) => String(n).padStart(2, "0");
  return `${p(Math.floor(s / 3600))}:${p(Math.floor(s / 60) % 60)}:${p(s % 60)}`;
}
function clockStart() {
  clockStop();
  clockT0 = Date.now();
  $("clock").textContent = "00:00:00";
  clockTimer = setInterval(() => { $("clock").textContent = clockFmt(Date.now() - clockT0); }, 500);
}
function clockStop() {
  if (clockTimer) { clearInterval(clockTimer); clockTimer = null; }
}

/* ---------- job events ---------- */
async function armJobEvents() {
  const { listen } = window.__TAURI__.event;
  await listen("job-line", (e) => {
    const { id, line } = e.payload;
    if (id === jobId && jobKind === "create") {
      const m = line.match(/(\d+)%/);
      if (m) { setProgress(+m[1], `${m[1]}%`); return; }
      logLine(line);
      if (line !== "Done.") jobLastError = line;
    } else if (id === dlJob && jobKind === "download") {
      dlLine(line);
    }
  });
  await listen("job-exit", async (e) => {
    const { id, code } = e.payload;
    if (id === jobId && jobKind === "create") {
      const jid = jobId; jobId = 0;
      clockStop();
      setProgress(null, "READY");
      syncEnabled();
      if (code === 0) { setProgress(100, "READY"); logLine("Done."); }
      else {
        const why = (code === 126 || code === 127)
          ? "Authorization was cancelled or pkexec is unavailable."
          : (jobLastError || `The worker exited with code ${code}.`);
        logLine("Failed: " + why);
        mbox(why, ["OK"]);
      }
      void jid;
    } else if (id === dlJob && jobKind === "download") {
      dlJob = 0;
      dlExit(code);
    }
  });
}

/* ---------- START ---------- */
async function askWue() {
  if (!$("wueOverlay").classList.contains("hidden")) return null;
  $("wueOverlay").classList.remove("hidden");
  return new Promise((resolve) => {
    $("wOk").onclick = () => {
      $("wueOverlay").classList.add("hidden");
      const parts = [];
      if ($("wBypass").checked) parts.push("bypass");
      if ($("wNro").checked) parts.push("nro");
      if ($("wPrivacy").checked) parts.push("privacy");
      if ($("wBitlocker").checked) parts.push("bitlocker");
      if ($("wQol").checked) parts.push("qol");
      const extra = [];
      if ($("wLocale").checked) { parts.push("locale"); extra.push("--locale", "en-US"); }
      if ($("wUserOn").checked) {
        const u = $("wUser").value.trim();
        if (!u) { mbox("Please enter a user name.", ["OK"]); resolve(askWue()); return; }
        parts.push("user=" + u);
      }
      resolve({ wue: parts.length ? parts.join(",") : "none", extra });
    };
    $("wCancel").onclick = () => { $("wueOverlay").classList.add("hidden"); resolve(null); };
  });
}
async function onStart() {
  if (jobId) { // CANCEL
    logLine("Cancel requested.");
    try { await invoke("kill_job", { id: jobId }); } catch (e) { /* exits on its own */ }
    return;
  }
  const dst = selectedDevice();
  if (!dst) return;
  const bi = $("cbBoot").selectedIndex;
  let mode;
  if (bi === 1) mode = "dos";
  else if (bi === 0) mode = "format";
  else {
    if (!isoPath) { mbox("Please select a disk or ISO image.", ["OK"]); return; }
    if (isoWindows) mode = "windows";
    else if (!isoValid) mode = "dd";
    else if (isoHybrid) {
      const c = await mbox(
        "The image you have selected is an 'ISOHybrid' image. This means it can be written either in ISO Image (file copy) mode or DD Image (disk image) mode.\nRufux recommends using ISO Image mode, so that you always have full access to the drive after writing it.\nHowever, if you encounter issues during boot, you can try writing this image again in DD Image mode.\n\nPlease select the mode that you want to use to write this image:",
        ["Write in ISO Image mode (Recommended)", "Write in DD Image mode", "Cancel"]);
      if (c === 0) mode = "extract";
      else if (c === 1) mode = "dd";
      else return;
    } else mode = "extract";
  }
  let wue = "none", extra = [];
  if (mode === "windows") {
    const w = await askWue();
    if (!w) return;
    wue = w.wue; extra = w.extra;
  }
  const dev = $("cbDevice").value;
  const ok = await mbox(
    `WARNING: ALL DATA ON DEVICE '${dev}' WILL BE DESTROYED.\nTo continue with this operation, click OK. To quit click CANCEL.`,
    ["OK", "CANCEL"]);
  if (ok !== 0) return;

  const fs = fsKey();
  const clusters = [1, 2, 4, 0, 16, 32, 64, 128];
  const spec = {
    src: isoPath, dst, mode,
    scheme: $("cbPart").selectedIndex === 1 ? "gpt" : "dos",
    fs, label: sanitizeLabel($("editLabel").value, fs),
    persistMb: (mode === "extract" && !$("persistBlock").classList.contains("hidden")) ? (+$("persistSize").value || 0) * ($("cbPersistUnits").value === "GB" ? 1024 : 1) : 0,
    clusterSectors: $("cbCluster").disabled ? 0 : clusters[$("cbCluster").selectedIndex],
    badblockPasses: $("chkBad").checked ? $("cbPasses").selectedIndex + 1 : 0,
    quick: $("chkQuick").checked,
    extendedLabel: $("chkExtLabel").checked,
    uefiValidate: $("chkUefiValid").checked,
    ignoreSmart: false,
    allowFixed: $("chkListHdd").checked,
    wue, extra,
  };
  jobLastError = "";
  jobKind = "create";
  try {
    jobId = await invoke("start_create", { spec });
  } catch (e) { mbox(`Cannot start:\n${e}`, ["OK"]); jobKind = ""; return; }
  setProgress(0, "%");
  clockStart();
  logLine(`Starting: ${mode} -> ${dst}`);
  syncEnabled();
}

/* ---------- checksums ---------- */
async function onHash() {
  if (!isoPath) return;
  $("hashOverlay").classList.remove("hidden");
  $("hashRows").innerHTML = '<div class="hash-wait">Computing…</div>';
  const rows = [];
  for (const a of [["MD5", "md5"], ["SHA-1", "sha1"], ["SHA-256", "sha256"], ["SHA-512", "sha512"]]) {
    try {
      const hex = await invoke("checksum", { path: isoPath, algo: a[1] });
      rows.push(`<div class="hash-row"><b>${a[0]}</b><code>${hex}</code></div>`);
    } catch (e) { rows.push(`<div class="hash-row"><b>${a[0]}</b><code>failed: ${e}</code></div>`); }
  }
  $("hashRows").innerHTML = rows.join("");
}

/* ---------- download (proper 2.0 flow) ---------- */
let dlProducts = [];
async function openDownload() {
  $("dlOverlay").classList.remove("hidden");
  $("dlStatus").textContent = "Listing products…";
  try {
    dlProducts = await invoke("dl_products");
  } catch (e) { $("dlStatus").textContent = "Download list failed: " + e; return; }
  fillEditions();
  $("dlStatus").textContent = "Pick edition, language, then Download.";
}
function fillEditions() {
  const ver = $("dlVer").value;
  const p = dlProducts.find((x) => x.name.includes(ver === "11" ? "11" : "10")) || dlProducts[0];
  const cb = $("dlEd");
  cb.innerHTML = "";
  (p ? p.editions : []).forEach((e, i) => {
    const o = document.createElement("option"); o.value = i; o.textContent = e; cb.appendChild(o);
  });
  DD.sync(cb);
  fillLangs();
}
async function fillLangs() {
  const ver = $("dlVer").value;
  const cb = $("dlLang");
  cb.innerHTML = "";
  $("dlStatus").textContent = "Listing languages…";
  try {
    const langs = await invoke("dl_langs", { version: ver, edition: +$("dlEd").value || 0 });
    langs.forEach((l) => {
      const o = document.createElement("option"); o.value = l.id; o.textContent = l.display; cb.appendChild(o);
    });
    DD.sync(cb);
    $("dlStatus").textContent = "Ready.";
  } catch (e) { $("dlStatus").textContent = "Language list failed: " + e; }
}
async function startDl() {
  if (dlJob) return;
  $("dlStatus").textContent = "Starting download…";
  $("dlfill").style.width = "0%"; $("dltext").textContent = "";
  jobKind = "download";
  let outdir = "";
  try { outdir = localStorage.getItem("rufux.dlDir") || ""; } catch (e) { /* noop */ }
  try {
    dlJob = await invoke("start_download", {
      opts: {
        version: $("dlVer").value, edition: +$("dlEd").value || 0,
        lang: $("dlLang").value, arch: $("dlArch").value,
        outdir: outdir || (await dlDir()) || ".",
      },
    });
  } catch (e) { $("dlStatus").textContent = "Cannot start: " + e; jobKind = ""; }
}
async function dlDir() {
  try { return await invoke("download_dir"); } catch (e) { return ""; }
}
function dlLine(line) {
  const m = line.match(/(\d+)%/);
  if (m) { $("dlfill").style.width = m[1] + "%"; $("dltext").textContent = m[1] + "%"; return; }
  if (line.startsWith("ISO: ")) {
    const p = line.slice(5).trim();
    lastDir("rufux.dlDir", p);
    $("dlStatus").textContent = "Done: " + p;
    loadImage(p);
    $("dlOverlay").classList.add("hidden");
    return;
  }
  $("dlStatus").textContent = line;
  logLine(line);
}
function dlExit(code) {
  jobKind = "";
  if (code !== 0) $("dlStatus").textContent = "Download failed (code " + code + ").";
}

/* ---------- wiring ---------- */
function lastDir(key, path) {
  try {
    if (path) {
      const dir = path.split(/[\\/]/).slice(0, -1).join("/") || "/";
      localStorage.setItem(key, dir);
      return dir;
    }
    return localStorage.getItem(key) || "";
  } catch (e) { return ""; }
}
async function selectFile() {
  const def = lastDir("rufux.imgDir") || lastDir("rufux.dlDir");
  const f = await dlgOpen({
    title: "Select a disk image",
    defaultPath: def || undefined,
    filters: [{ name: "Images", extensions: ["iso", "img", "bin", "vhd", "vhdx", "vmdk", "qcow2", "vdi"] }, { name: "All files", extensions: ["*"] }],
    multiple: false,
    directory: false,
  });
  if (!f) return;
  const p = Array.isArray(f) ? f[0] : f;
  lastDir("rufux.imgDir", p);
  loadImage(p);
}
function wire() {
  document.querySelectorAll("select.combo").forEach((s) => DD.bind(s));
  $("btnSelect").onclick = () => (selectMode === 1 ? openDownload() : selectFile());
  $("btnSelectArrow").onclick = (e) => { e.stopPropagation(); $("selMenu").classList.toggle("hidden"); };
  document.addEventListener("click", () => $("selMenu").classList.add("hidden"));
  $("mSelect").onclick = () => { selectMode = 0; $("btnSelect").textContent = "SELECT"; $("mSelect").classList.add("checked"); $("mDownload").classList.remove("checked"); };
  $("mDownload").onclick = () => { selectMode = 1; $("btnSelect").textContent = "DOWNLOAD"; $("mDownload").classList.add("checked"); $("mSelect").classList.remove("checked"); };
  $("btnHash").onclick = onHash;
  $("btnSave").onclick = () => mbox("Saving drive contents is not supported by the Linux backend yet.", ["OK"]);
  $("hashClose").onclick = () => $("hashOverlay").classList.add("hidden");
  $("btnStart").onclick = onStart;
  $("btnClose").onclick = async () => { try { await invoke("close_window"); } catch (e) { window.close(); } };
  $("tbAbout").onclick = () => mbox("Rufux 2.1.0\nCreate bootable USB drives on Linux.\nA Linux port of Rufus by Pete Batard. License: GPLv3.\nhttps://github.com/Hultwl/Rufux", ["OK"]);
  $("tbLog").onclick = () => $("logOverlay").classList.remove("hidden");
  $("tbSettings").onclick = () => mbox("Updates are handled by your package manager.", ["OK"]);
  $("tbLang").onclick = () => mbox("English only in this build.", ["OK"]);
  $("logClose").onclick = () => $("logOverlay").classList.add("hidden");
  $("logClear").onclick = () => { logLines.length = 0; $("logView").value = ""; };
  $("logSave").onclick = async () => {
    const p = await dlgSave({ defaultPath: "rufux.log" });
    if (p) { try { await invoke("save_file", { path: p, content: logLines.join("\n") }); } catch (e) { mbox("Save failed: " + e, ["OK"]); } }
  };
  const adv = (tgl, body, what) => {
    $(tgl).onclick = () => {
      const collapsed = $(body).classList.toggle("hidden");
      $(tgl).querySelector(".glyph").textContent = collapsed ? "►" : "▼";
      $(tgl).childNodes[1].textContent = ` ${collapsed ? "Show" : "Hide"} advanced ${what} properties`;
      fit();
    };
  };
  adv("tglAdvDrive", "advDrive", "drive");
  adv("tglAdvFormat", "advFormat", "format");
  $("chkBad").onchange = () => ($("cbPasses").disabled = !$("chkBad").checked);
  $("chkListHdd").onchange = refreshDevices;
  $("cbBoot").onchange = syncEnabled;
  $("cbFs").onchange = () => { relabel(); syncEnabled(); };
  $("cbTarget").onchange = () => { $("cbPart").selectedIndex = $("cbTarget").selectedIndex === 1 ? 1 : 0; };
  $("cbPart").onchange = () => { $("cbTarget").selectedIndex = $("cbPart").selectedIndex === 1 ? 1 : 0; };
  $("persistSlider").oninput = () => {
    const v = +$("persistSlider").value;
    if (v % 2) { $("cbPersistUnits").value = "MB"; $("persistSize").value = v * 512; }
    else { $("cbPersistUnits").value = "GB"; $("persistSize").value = v / 2; }
  };
  $("dlVer").onchange = fillEditions;
  $("dlEd").onchange = fillLangs;
  $("dlGo").onclick = startDl;
  $("dlCancel").onclick = async () => {
    if (dlJob) { try { await invoke("kill_job", { id: dlJob }); } catch (e) { /* noop */ } }
    $("dlOverlay").classList.add("hidden");
  };
}

async function boot() {
  try {
    wire();
    try {
      const info = await invoke("backend_info");
      logLine(`Rufux 2.1.0 (backend: ${info.bin})`);
    } catch (e) { logLine("Backend missing: " + e); }
    try {
      await armJobEvents();
    } catch (e) { logLine("Event bus unavailable: " + e); }
    await refreshDevices();
    syncEnabled();
    setInterval(refreshDevices, 2000);
    try {
      const { getCurrentWebview } = window.__TAURI__.webview;
      await getCurrentWebview().onDragDropEvent((e) => {
        if (e.payload.type === "drop" && e.payload.paths.length) loadImage(e.payload.paths[0]);
      });
    } catch (e) { logLine("Drag-drop unavailable: " + e); }
    setFsOptions(false);
  } catch (e) {
    try { mbox("Startup failed: " + e, ["OK"]); } catch (e2) { /* noop */ }
  }
}
document.addEventListener("DOMContentLoaded", boot);
