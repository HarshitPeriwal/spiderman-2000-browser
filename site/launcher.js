// Launcher for running the player's own Spider-Man (2000) PC install under
// Boxedwine in the browser.
//
// The game folder is read once and installed into the browser's own storage:
// the IndexedDB database that Boxedwine mounts as drive D:. Nothing is
// uploaded and nothing is written back to the player's disk. (Writing .dll and
// .exe files to disk through the File System Access API runs Chrome's Safe
// Browsing checks, which can refuse binkw32.dll.)

const ROOT_FS = "TinyCore15Wine11.0-web";
const STORAGE_KEY = "spiderman2000";

// Must match Boxedwine's configureBrowserStorage(): storageKey=<key> mounts
// D: at /d_drive/key/<encoded key>, and Emscripten's IDBFS keeps each mount in
// a database named after the mount point.
const D_DRIVE = "/d_drive/key/" + encodeURIComponent(STORAGE_KEY);
const IDB_VERSION = 21;           // IDBFS.DB_VERSION
const IDB_STORE = "FILE_DATA";    // IDBFS.DB_STORE_NAME
const MODE_DIR = 0o40777;
const MODE_FILE = 0o100666;

// Files the game itself needs (from SpideyPC.exe's imports and its PKR loader).
const REQUIRED = ["spideypc.exe", "data.pkr", "media.pkr", "texture.dat", "binkw32.dll"];

// DirectX wrappers and installer leftovers. Wine searches the game folder for
// native DLLs before system32, so a wrapper ddraw.dll here would shadow the
// WebGL ddraw that Boxedwine provides. None of these are part of the game.
const EXCLUDED = new Set([
  "ddraw.dll", "d3d8.dll", "d3d9.dll", "d3d10.dll", "d3d10core.dll", "d3d11.dll", "d3d12.dll",
  "d3dimm.dll", "dxgi.dll", "dgvoodoo.conf", "dgvoodoocpl.exe", "dgvoodoo.dll",
  "unins000.exe", "unins000.dat", "dinput.dll", "dinput8.dll", "dsound.dll", "winmm.dll",
]);
const EXCLUDED_DIRS = new Set(["docs"]);

// Linker timestamp of the Treyarch PC build this port was prepared against.
const KNOWN_BUILD_TIMESTAMP = Date.UTC(2001, 7, 15, 8, 23, 3) / 1000;

const $ = (id) => document.getElementById(id);
let source = null;   // [{ path, getFile }] of the selected install, top level paths relative

function fmtSize(bytes) {
  if (bytes >= 1 << 20) return (bytes / (1 << 20)).toFixed(1) + " MB";
  if (bytes >= 1 << 10) return (bytes / (1 << 10)).toFixed(0) + " KB";
  return bytes + " B";
}

function addRow(list, name, detail, cls) {
  const li = document.createElement("li");
  const a = document.createElement("span");
  const b = document.createElement("span");
  a.textContent = name;
  b.textContent = detail;
  if (cls) b.className = cls;
  li.append(a, b);
  list.append(li);
}

function setStatus(el, text, cls) {
  el.textContent = text;
  el.className = "status" + (cls ? " " + cls : "");
}

// --- IndexedDB (Boxedwine's D: drive) ------------------------------------------

function openDrive() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(D_DRIVE, IDB_VERSION);
    req.onupgradeneeded = (e) => {
      const db = req.result;
      const store = db.objectStoreNames.contains(IDB_STORE)
        ? e.target.transaction.objectStore(IDB_STORE)
        : db.createObjectStore(IDB_STORE);
      if (!store.indexNames.contains("timestamp")) store.createIndex("timestamp", "timestamp", { unique: false });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function txDone(tx) {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error || new Error("transaction aborted"));
  });
}

async function putEntry(db, path, entry) {
  const tx = db.transaction(IDB_STORE, "readwrite");
  tx.objectStore(IDB_STORE).put(entry, D_DRIVE + "/" + path);
  await txDone(tx);
}

async function installedFiles() {
  const db = await openDrive();
  try {
    const tx = db.transaction(IDB_STORE, "readonly");
    const keys = await new Promise((resolve, reject) => {
      const req = tx.objectStore(IDB_STORE).getAllKeys();
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    return new Set(keys.map((k) => String(k).slice(D_DRIVE.length + 1).toLowerCase()));
  } finally {
    db.close();
  }
}

async function isInstalled() {
  try {
    const files = await installedFiles();
    return REQUIRED.every((f) => files.has(f));
  } catch {
    return false;
  }
}

async function removeInstalled() {
  await new Promise((resolve, reject) => {
    const req = indexedDB.deleteDatabase(D_DRIVE);
    req.onsuccess = () => resolve();
    req.onerror = () => reject(req.error);
    req.onblocked = () => reject(new Error("close the game tab first"));
  });
}

// --- Browser check ----------------------------------------------------------

function checkSupport() {
  const list = $("support-list");
  const gl2 = (() => {
    try { return !!document.createElement("canvas").getContext("webgl2"); } catch { return false; }
  })();
  const checks = [
    ["WebAssembly", typeof WebAssembly === "object"],
    ["WebGL 2", gl2],
    ["Browser storage (IndexedDB)", typeof indexedDB === "object"],
    ["Secure context (https or localhost)", window.isSecureContext],
  ];
  for (const [name, ok] of checks) addRow(list, name, ok ? "available" : "missing", ok ? "ok" : "bad");
  addRow(list, "Recommended", "Chrome or Edge 137+, Firefox 131+", "");
}

// --- Reading the install folder ---------------------------------------------------

// The folder is read through <input webkitdirectory>, not showDirectoryPicker():
// Chrome's File System Access API leaves .dll files out of directory listings,
// which hid binkw32.dll. The folder input lists every file.
function fromFileList(files) {
  // webkitRelativePath is "<picked folder>/<path inside it>"
  const out = [];
  const dirs = new Set();
  for (const f of files) {
    const rel = f.webkitRelativePath ? f.webkitRelativePath.split("/").slice(1) : [f.name];
    for (let i = 1; i < rel.length; i++) dirs.add(rel.slice(0, i).join("/"));
    out.push({ path: rel.join("/"), getFile: async () => f });
  }
  for (const d of dirs) out.push({ path: d, dir: true });
  return out;
}

function isExcluded(path) {
  const parts = path.toLowerCase().split("/");
  if (EXCLUDED_DIRS.has(parts[0])) return true;
  return parts.length === 1 && EXCLUDED.has(parts[0]);
}

async function readPeInfo(file) {
  const buf = new DataView(await file.slice(0, 4096).arrayBuffer());
  if (buf.getUint16(0, true) !== 0x5a4d) return null;
  const pe = buf.getUint32(0x3c, true);
  if (pe + 24 > buf.byteLength || buf.getUint32(pe, true) !== 0x4550) return null;
  return { machine: buf.getUint16(pe + 4, true), timestamp: buf.getUint32(pe + 8, true) };
}

async function hasPkrMagic(file) {
  const head = new Uint8Array(await file.slice(0, 4).arrayBuffer());
  return String.fromCharCode(...head) === "PKR3";
}

async function checkSource(entries) {
  const status = $("source-status");
  const list = $("source-files");
  list.replaceChildren();
  const top = new Map(entries.filter((e) => !e.dir && !e.path.includes("/")).map((e) => [e.path.toLowerCase(), e]));
  let ok = true;

  for (const req of REQUIRED) {
    const entry = top.get(req);
    if (!entry) {
      addRow(list, req, "missing", "bad");
      ok = false;
      continue;
    }
    const file = await entry.getFile();
    let detail = fmtSize(file.size);
    let cls = "ok";
    if (req.endsWith(".pkr") && !(await hasPkrMagic(file))) {
      detail += ", not a PKR3 archive";
      cls = "bad";
      ok = false;
    }
    if (req === "spideypc.exe") {
      const pe = await readPeInfo(file);
      if (!pe || pe.machine !== 0x14c) {
        detail += ", not a 32-bit Windows executable";
        cls = "bad";
        ok = false;
      } else {
        const built = new Date(pe.timestamp * 1000).toISOString().slice(0, 10);
        if (pe.timestamp === KNOWN_BUILD_TIMESTAMP) {
          detail += `, PC build of ${built} (tested target)`;
        } else {
          detail += `, PC build of ${built} (not the tested build)`;
          cls = "warn";
        }
      }
    }
    addRow(list, entry.path, detail, cls);
  }
  for (const [name, e] of top) {
    if (EXCLUDED.has(name) && !name.startsWith("unins")) addRow(list, e.path, "wrapper, not installed", "warn");
  }

  if (!ok) {
    const found = [...top.values()].map((e) => e.path).sort();
    const nested = entries.filter((e) => !e.dir && /(^|\/)binkw32\.dll$/i.test(e.path) && e.path.includes("/"));
    let hint = found.length ? " Files found in that folder: " + found.join(", ") + "." : " That folder has no files at its top level.";
    if (nested.length) hint += " binkw32.dll is in a subfolder (" + nested[0].path + "); pick the folder that holds SpideyPC.exe.";
    setStatus(status, "This doesn't look like a complete Spider-Man PC install." + hint, "bad");
    source = null;
    $("add-file").hidden = !(top.has("spideypc.exe") && !top.has("binkw32.dll"));
  } else {
    setStatus(status, "Found a complete install. Your folder is only read, never changed.", "ok");
    source = entries;
    $("add-file").hidden = true;
  }
  $("install").disabled = !source;
}

let picked = [];   // entries from the folder input, plus any single files added after

function pickSource() {
  $("folder-input").click();
}

// Backup for a browser that still leaves binkw32.dll out of the folder listing.
async function addSingleFile(file) {
  picked = picked.filter((e) => e.path.toLowerCase() !== file.name.toLowerCase());
  picked.push({ path: file.name, getFile: async () => file });
  await checkSource(picked);
}

// --- Install into browser storage --------------------------------------------------

async function install() {
  const status = $("install-status");
  const bar = $("install-progress");
  $("install").disabled = true;
  $("pick-source").disabled = true;

  if (navigator.storage?.persist) {
    try { await navigator.storage.persist(); } catch {}
  }
  const plan = source.filter((e) => !isExcluded(e.path));
  const files = [];
  for (const e of plan) if (!e.dir) files.push({ ...e, file: await e.getFile() });
  const total = files.reduce((n, f) => n + f.file.size, 0);

  if (navigator.storage?.estimate) {
    const { quota = Infinity, usage = 0 } = await navigator.storage.estimate();
    if (quota - usage < total * 1.1) {
      setStatus(status, `Not enough browser storage: need ${fmtSize(total)}, ${fmtSize(quota - usage)} free.`, "bad");
      $("install").disabled = false;
      $("pick-source").disabled = false;
      return;
    }
  }

  let db;
  let done = 0;
  bar.hidden = false;
  bar.max = total;
  bar.value = 0;
  try {
    await removeInstalled();   // start from a clean drive
    db = await openDrive();
    const now = new Date();
    for (const d of plan.filter((e) => e.dir)) {
      await putEntry(db, d.path, { timestamp: now, mode: MODE_DIR });
    }
    for (const f of files) {
      setStatus(status, `Installing ${f.path} (${fmtSize(done)} of ${fmtSize(total)})`);
      const contents = new Uint8Array(await f.file.arrayBuffer());
      await putEntry(db, f.path, { timestamp: new Date(f.file.lastModified || now), mode: MODE_FILE, contents });
      done += f.file.size;
      bar.value = done;
    }
  } catch (e) {
    setStatus(status, `Install failed at ${fmtSize(done)}: ${e.message || e}`, "bad");
    $("install").disabled = false;
    $("pick-source").disabled = false;
    return;
  } finally {
    db?.close();
  }

  const installed = await installedFiles();
  const lost = REQUIRED.filter((f) => !installed.has(f));
  if (lost.length) {
    setStatus(status, "Install incomplete, missing: " + lost.join(", "), "bad");
    $("install").disabled = false;
    return;
  }
  setStatus(status, `Installed ${files.length} files (${fmtSize(total)}) in this browser. You won't need to do this again.`, "ok");
  $("pick-source").disabled = false;
  setInstalled(true);
}

function setInstalled(yes) {
  $("launch").disabled = !yes;
  $("remove").hidden = !yes;
}

// --- Launch ----------------------------------------------------------------

function selectedRuntime() {
  return document.querySelector('input[name="runtime"]:checked').value;
}

function launchUrl() {
  const params = new URLSearchParams({
    root: ROOT_FS,
    storage: "indexeddb",
    storageKey: STORAGE_KEY,
    p: "SpideyPC.exe",
    w: "d:/",
    sound: "true",
  });
  return `runtime/${selectedRuntime()}/boxedwine.html?${params}`;
}

function updateLaunchUrl() {
  $("launch-url").textContent = new URL(launchUrl(), location.href).href;
}

// --- Wiring ----------------------------------------------------------------

checkSupport();
const canIsolate = "serviceWorker" in navigator && window.isSecureContext;
$(canIsolate ? "rt-mt" : "rt-st").checked = true;
$("rt-mt").disabled = !canIsolate;
for (const r of document.querySelectorAll('input[name="runtime"]')) r.addEventListener("change", updateLaunchUrl);
updateLaunchUrl();

$("pick-source").addEventListener("click", pickSource);
$("folder-input").addEventListener("change", async (e) => {
  if (!e.target.files.length) return;
  setStatus($("source-status"), "Reading folder...");
  picked = fromFileList(e.target.files);
  await checkSource(picked);
});
$("add-file").addEventListener("click", () => $("file-input").click());
$("file-input").addEventListener("change", async (e) => {
  if (e.target.files.length) await addSingleFile(e.target.files[0]);
  e.target.value = "";
});
$("install").addEventListener("click", install);
$("launch").addEventListener("click", () => { location.href = launchUrl(); });
$("remove").addEventListener("click", async () => {
  try {
    await removeInstalled();
    setInstalled(false);
    setStatus($("install-status"), "Removed the installed copy from this browser. Your saves in it are gone too.", "warn");
  } catch (e) {
    setStatus($("install-status"), "Couldn't remove it: " + e.message, "bad");
  }
});

isInstalled().then((yes) => {
  setInstalled(yes);
  if (yes) setStatus($("install-status"), "Already installed in this browser. Go straight to step 3.", "ok");
});
