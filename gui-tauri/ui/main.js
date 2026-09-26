/* Rufux web replica: drives the rufux CLI backend, looks like Rufus. */
const { invoke } = window.__TAURI__.core;

const $ = (id) => document.getElementById(id);
const logLines = [];
let devs = [];
let isoPath = "", isoLabel = "", isoWindows = false, isoHybrid = false, isoValid = false;
let selectMode = 0; // 0 = SELECT file, 1 = DOWNLOAD (split button, like Rufus)
let jobId = 0, jobKind = "", jobLastError = "";
let dlJob = 0;

/* ---------- i18n (Rufus's own translations; he-IL/fa-IR excluded) ---------- */
let LANG = "en-US";
try {
  LANG = localStorage.getItem("rufux.lang") || "";
  if (!LANG && window.RUFUX_I18N) {
    const nav = (navigator.language || "en-US").replace("_", "-");
    const codes = Object.keys(window.RUFUX_I18N).filter((k) => !k.startsWith("_"));
    LANG = codes.find((c) => c.toLowerCase() === nav.toLowerCase())
      || codes.find((c) => c.split("-")[0].toLowerCase() === nav.split("-")[0].toLowerCase())
      || "en-US";
  }
} catch (e) { LANG = "en-US"; }
const UPPER_KEYS = new Set((window.RUFUX_I18N && window.RUFUX_I18N._keys_upper) || []);
function t(key) {
  const I = window.RUFUX_I18N || {};
  const d = I[LANG] || {};
  let s = d[key];
  if (!s) s = (I["en-US"] || {})[key];
  if (!s) return key;
  if (UPPER_KEYS.has(key)) { try { s = s.toLocaleUpperCase(LANG); } catch (e) { s = s.toUpperCase(); } }
  return s;
}
function fmt(s) {
  const args = Array.prototype.slice.call(arguments, 1);
  let i = 0;
  return String(s).replace(/%[ds]/g, () => (i < args.length ? args[i++] : ""));
}
function toggleText(collapsed, what) {
  return fmt(t(collapsed ? "show_tpl" : "hide_tpl"),
    t(what === "drive" ? "adv_drive_noun" : "adv_fmt_noun"));
}
function refreshDevCount() {
  const n = devs.length;
  let s;
  if (LANG === "ar-SA") {
    if (n === 1) s = t("dev1");
    else if (n === 2) s = t("devN_ar2");
    else if (n <= 10) s = fmt(t("devN_ar310"), n);
    else s = fmt(t("devN_ar11"), n);
  } else {
    s = fmt(t(n === 1 ? "dev1" : "devN"), n);
  }
  $("devCount").textContent = s;
}
function setOpts(sel, items) {
  // items: [value, key-or-literal, isKey]
  const idx = sel.selectedIndex < 0 ? 0 : sel.selectedIndex;
  sel.innerHTML = "";
  items.forEach(([v, k, isKey]) => {
    const o = document.createElement("option");
    o.value = v;
    o.textContent = isKey ? t(k) : k;
    sel.appendChild(o);
  });
  sel.selectedIndex = Math.min(idx, items.length - 1);
  DD.sync(sel);
}
const FS_KEYS = { large: "large_fat32", vfat: "fat32", fat16: "fat16", ntfs: "ntfs", exfat: "exfat", udf: "udf", ext2: "ext2", ext3: "ext3", ext4: "ext4" };
const CL_KEYS = ["cl_512", "cl_1024", "cl_2048", "cl_4096", "cl_8192", "cl_16k", "cl_32k", "cl_64k"];
const CL_VALS = ["s1", "s2", "s4", "s0", "s16", "s32", "s64", "s128"];
function rebuildCombos() {
  const boot = $("cbBoot");
  const bIdx = boot.selectedIndex;
  const bDyn = isoPath ? boot.options[2].text : "";
  setOpts(boot, [[0, "nonboot", true], [1, "freedos", true], [2, bDyn || t("iso_please"), false]]);
  boot.selectedIndex = bIdx < 0 ? 2 : bIdx;
  DD.sync(boot);
  setOpts($("cbImageOpt"), [["std", "std_win", true]]);
  setOpts($("cbPersistUnits"), [["GB", "gb", true], ["MB", "mb", true]]);
  setOpts($("cbPart"), [["mbr", "mbr", true], ["gpt", "gpt", true]]);
  setOpts($("cbTarget"), [["bios", "bios_csm", true], ["uefi", "uefi_nocsm", true]]);
  setOpts($("cbBiosId"), [["80", "bios_id_val", true]]);
  setOpts($("cbCluster"), CL_VALS.map((v, i) => [v, CL_KEYS[i], true]));
  setOpts($("cbPasses"), [["p1", "pass1", true], ["p2", "pass2", true], ["p3", "pass3", true], ["p4", "pass4", true]]);
  setOpts($("dlVer"), [["11", "win11", true], ["10", "win10", true]]);
  setOpts($("dlArch"), [["", "arch_def", true], ["x64", "x64", false], ["arm64", "arm64", false]]);
  setFsOptions(isoWindows);
  DD.syncAll();
}
function applyI18n() {
  const meta = (window.RUFUX_I18N && window.RUFUX_I18N._meta) || {};
  const rtl = !!(meta[LANG] && meta[LANG].rtl);
  document.documentElement.dir = rtl ? "rtl" : "ltr";
  document.documentElement.lang = LANG;
  document.querySelectorAll("[data-i18n]").forEach((el) => { el.textContent = t(el.getAttribute("data-i18n")); });
  document.querySelectorAll("[data-i18n-title]").forEach((el) => { el.title = t(el.getAttribute("data-i18n-title")); });
  rebuildCombos();
  $("mSelect").textContent = t("select");
  $("mDownload").textContent = t("download");
  syncSelectBtn();
  setToggle("tglAdvDrive", "advDrive", "drive");
  setToggle("tglAdvFormat", "advFormat", "format");
  setProgress(null, t("ready"));
  refreshDevCount();
  DD.syncAll();
}
function syncSelectBtn() { $("btnSelect").textContent = selectMode === 1 ? t("download") : t("select"); }
function setToggle(tgl, body, what) {
  const collapsed = $(body).classList.contains("hidden");
  $(tgl).querySelector(".glyph").textContent = collapsed ? "►" : "▼";
  $(tgl).querySelector(".tgl-text").textContent = " " + toggleText(collapsed, what);
}
function buildLangMenu() {
  const m = $("langMenu");
  m.innerHTML = "";
  const I = window.RUFUX_I18N || {};
  const meta = I._meta || {};
  const codes = ["en-US", ...Object.keys(I).filter((k) => !k.startsWith("_") && k !== "en-US").sort()];
  codes.forEach((c) => {
    const s = document.createElement("span");
    s.className = "menu-item" + (c === LANG ? " checked" : "");
    s.textContent = (meta[c] && meta[c].name) || c;
    s.onclick = (ev) => {
      ev.stopPropagation();
      m.classList.add("hidden");
      setLang(c);
    };
    m.appendChild(s);
  });
}
function setLang(c) {
  LANG = (window.RUFUX_I18N && window.RUFUX_I18N[c]) ? c : "en-US";
  try { localStorage.setItem("rufux.lang", c); } catch (e) {}
  buildLangMenu();
  applyI18n();
  refreshDevices();
}

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
      const o = document.createElement("option");
      o.value = "";
      o.textContent = t("no_usb");
      cb.appendChild(o);
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
    refreshDevCount();
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
  const v = $("cbFs").value;
  if (v === "fat16") return "fat16";
  if (v === "vfat" || v === "large") return "vfat";
  if (v === "exfat") return "exfat";
  if (v === "udf") return "udf";
  if (v === "ext2") return "ext2";
  if (v === "ext3") return "ext3";
  if (v === "ext4") return "ext4";
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
  $("btnStart").textContent = run ? t("cancel_op") : t("start");
  DD.syncAll();
  fit();
}

/* ---------- image probe ---------- */
async function loadImage(path) {
  setProgress(null, t("reading"));
  try {
    const info = await invoke("probe_iso", { path });
    isoPath = path;
    isoWindows = info.windows === "yes";
    isoValid = info.valid;
    isoHybrid = info.valid && info.bootable;
    isoLabel = info.label && info.label !== "(none)" ? info.label : "";
    const base = path.split(/[\\/]/).pop();
    const cb = $("cbBoot");
    const bIdx = cb.selectedIndex;
    cb.innerHTML = "";
    [[0, "nonboot", true], [1, "freedos", true]].forEach(([v, k]) => {
      const o = document.createElement("option");
      o.value = v;
      o.textContent = t(k);
      cb.appendChild(o);
    });
    const bo = document.createElement("option");
    bo.value = 2;
    bo.textContent = base;
    cb.appendChild(bo);
    cb.selectedIndex = 2;
    DD.sync(cb);
    logLine(`Using image: ${path} (${humanSize(info.size_bytes)})`);
    if (isoWindows) {
      setFsOptions(true);
      $("cbFs").value = "ntfs";
      $("cbPart").selectedIndex = 1; $("cbTarget").selectedIndex = 1;
    } else {
      setFsOptions(false);
      $("cbFs").value = "vfat";
      $("cbPart").selectedIndex = 0; $("cbTarget").selectedIndex = 0;
    }
    $("editLabel").value = sanitizeLabel(isoLabel || "NO_LABEL", fsKey());
    setProgress(null, t("ready"));
  } catch (e) {
    mbox(t("cannot_use_img") + e, [t("ok")]);
    setProgress(null, t("ready"));
  }
  syncEnabled();
}
function setFsOptions(windows) {
  const cb = $("cbFs");
  const cur = cb.value;
  const full = ["large", "vfat", "fat16", "ntfs", "exfat", "udf", "ext2", "ext3", "ext4"];
  const list = windows ? ["vfat", "ntfs"] : full;
  const def = windows ? "ntfs" : "vfat";
  cb.innerHTML = "";
  list.forEach((v) => {
    const o = document.createElement("option");
    o.value = v;
    o.textContent = v === def ? fmt(t("def_tpl"), t(FS_KEYS[v])) : t(FS_KEYS[v]);
    cb.appendChild(o);
  });
  cb.selectedIndex = Math.max(0, list.indexOf(list.includes(cur) ? cur : def));
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
      setProgress(null, t("ready"));
      syncEnabled();
      if (code === 0) { setProgress(100, t("ready")); logLine("Done."); }
      else {
        const why = (code === 126 || code === 127)
          ? t("auth_cancel")
          : (jobLastError || fmt(t("worker_exited"), code));
        logLine("Failed: " + why);
        mbox(why, [t("ok")]);
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
      if ($("wLocale").checked) {
        parts.push("locale");
        const tag = ((navigator.language || "en-US") + "").replace("_", "-");
        extra.push("--locale", tag);
      }
      if ($("wUserOn").checked) {
        const u = $("wUser").value.trim();
        if (!u) { mbox(t("enter_user"), [t("ok")]); resolve(askWue()); return; }
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
    if (!isoPath) { mbox(t("sel_img"), [t("ok")]); return; }
    if (isoWindows) mode = "windows";
    else if (!isoValid) mode = "dd";
    else if (isoHybrid) {
      const c = await mbox(
        fmt(t("iso_msg"), "ISO Image", "DD Image", "ISO Image", "DD Image"),
        [t("iso_btn"), t("dd_btn"), t("cancel")]);
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
  const dev = $("cbDevice").selectedOptions.length ? $("cbDevice").selectedOptions[0].text : "";
  const ok = await mbox(
    fmt(t("destroy_msg"), dev),
    [t("ok"), t("cancel_op")]);
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
  } catch (e) { mbox(t("cannot_start") + e, [t("ok")]); jobKind = ""; return; }
  setProgress(0, "%");
  clockStart();
  logLine(`Starting: ${mode} -> ${dst}`);
  syncEnabled();
}

/* ---------- checksums ---------- */
async function onHash() {
  if (!isoPath) return;
  $("hashOverlay").classList.remove("hidden");
  $("hashRows").innerHTML = '<div class="hash-wait">' + t("computing") + '</div>';
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
  $("dlbar").classList.add("hidden");
  $("dlStatus").textContent = t("dl_listing");
  try {
    dlProducts = await invoke("dl_products");
  } catch (e) { $("dlStatus").textContent = t("dl_list_fail") + e; return; }
  fillEditions();
  $("dlStatus").textContent = t("dl_pick");
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
  $("dlStatus").textContent = t("dl_langs");
  try {
    const langs = await invoke("dl_langs", { version: ver, edition: +$("dlEd").value || 0 });
    langs.forEach((l) => {
      const o = document.createElement("option"); o.value = l.id; o.textContent = l.display; cb.appendChild(o);
    });
    DD.sync(cb);
    $("dlStatus").textContent = t("dl_pick");
  } catch (e) { $("dlStatus").textContent = t("dl_lang_fail") + e; }
}
async function startDl() {
  if (dlJob) return;
  $("dlStatus").textContent = t("dl_starting");
  $("dlbar").classList.remove("hidden");
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
  } catch (e) { $("dlStatus").textContent = t("cannot_start") + e; $("dlbar").classList.add("hidden"); jobKind = ""; }
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
    $("dlStatus").textContent = t("dl_done") + p;
    loadImage(p);
    $("dlOverlay").classList.add("hidden");
    $("dlbar").classList.add("hidden");
    return;
  }
  $("dlStatus").textContent = line;
  logLine(line);
}
function dlExit(code) {
  jobKind = "";
  if (code !== 0) $("dlStatus").textContent = fmt(t("dl_failed"), code);
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
  $("mSelect").onclick = () => { selectMode = 0; syncSelectBtn(); $("mSelect").classList.add("checked"); $("mDownload").classList.remove("checked"); };
  $("mDownload").onclick = () => { selectMode = 1; syncSelectBtn(); $("mDownload").classList.add("checked"); $("mSelect").classList.remove("checked"); };
  $("btnHash").onclick = onHash;
  $("btnSave").onclick = () => mbox(t("save_unsupported"), [t("ok")]);
  $("hashClose").onclick = () => $("hashOverlay").classList.add("hidden");
  $("btnStart").onclick = onStart;
  $("btnClose").onclick = async () => { try { await invoke("close_window"); } catch (e) { window.close(); } };
  $("tbAbout").onclick = () => mbox(t("about_text"), [t("ok")]);
  $("tbLog").onclick = () => $("logOverlay").classList.remove("hidden");
  $("tbSettings").onclick = () => mbox(t("updates_pkg"), [t("ok")]);
  $("tbLang").onclick = (e) => { e.stopPropagation(); buildLangMenu(); $("langMenu").classList.toggle("hidden"); };
  document.addEventListener("click", () => $("langMenu").classList.add("hidden"));
  $("logClose").onclick = () => $("logOverlay").classList.add("hidden");
  $("logClear").onclick = () => { logLines.length = 0; $("logView").value = ""; };
  $("logSave").onclick = async () => {
    const p = await dlgSave({ defaultPath: "rufux.log" });
    if (p) { try { await invoke("save_file", { path: p, content: logLines.join("\n") }); } catch (e) { mbox(t("save_failed") + e, [t("ok")]); } }
  };
  const adv = (tgl, body, what) => {
    $(tgl).onclick = () => {
      $(body).classList.toggle("hidden");
      setToggle(tgl, body, what);
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
    $("dlbar").classList.add("hidden");
  };
}

async function boot() {
  try {
    wire();
    try {
      const info = await invoke("backend_info");
      logLine(`Rufux 2.2.0 (backend: ${info.bin})`);
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
    buildLangMenu();
    applyI18n();
    try {
      const f = await invoke("startup_file");
      if (f) loadImage(f);
    } catch (e) { /* no startup file */ }
  } catch (e) {
    try { mbox(t("startup_failed") + e, [t("ok")]); } catch (e2) { /* noop */ }
  }
}
document.addEventListener("DOMContentLoaded", boot);
