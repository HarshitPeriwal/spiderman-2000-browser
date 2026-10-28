// Launcher for running the user's own Spider-Man (2000) PC install under
// Boxedwine in the browser. Everything here runs locally: files are read with
// the File System Access API and copied between folders on the user's disk.

const ROOT_FS = "TinyCore15Wine11.0-web";
const STORAGE_KEY = "spiderman2000";

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
let sourceDir = null;
let copyReady = false;

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

// --- Browser check ----------------------------------------------------------

function checkSupport() {
  const list = $("support-list");
  const gl2 = (() => {
    try { return !!document.createElement("canvas").getContext("webgl2"); } catch { return false; }
  })();
  const checks = [
    ["WebAssembly", typeof WebAssembly === "object"],
    ["WebGL 2", gl2],
    ["Folder access (File System Access API)", typeof window.showDirectoryPicker === "function"],
    ["Secure context (https or localhost)", window.isSecureContext],
  ];
  for (const [name, ok] of checks) addRow(list, name, ok ? "available" : "missing", ok ? "ok" : "bad");
  addRow(list, "Recommended", "Chrome or Edge 137+ (folder access is Chromium-only)", "");
  return checks.every(([, ok]) => ok);
}

// --- Source folder -----------------------------------------------------------

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

async function topLevelFiles(dir) {
  const files = new Map();
  for await (const [name, handle] of dir.entries()) {
    if (handle.kind === "file") files.set(name.toLowerCase(), { name, handle });
  }
  return files;
}

async function pickSource() {
  const status = $("source-status");
  const list = $("source-files");
  let dir;
  try {
    dir = await window.showDirectoryPicker({ id: "spiderman-source", mode: "read" });
  } catch (e) {
    if (e.name !== "AbortError") setStatus(status, "Couldn't open the folder: " + e.message, "bad");
    return;
  }
  list.replaceChildren();
  const files = await topLevelFiles(dir);
  let ok = true;

  for (const req of REQUIRED) {
    const entry = files.get(req);
    if (!entry) {
      addRow(list, req, "missing", "bad");
      ok = false;
      continue;
    }
    const file = await entry.handle.getFile();
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
    addRow(list, entry.name, detail, cls);
  }

  const wrappers = [...files.values()].filter((f) => EXCLUDED.has(f.name.toLowerCase()) &&
    !["unins000.exe", "unins000.dat"].includes(f.name.toLowerCase()));
  for (const w of wrappers) addRow(list, w.name, "wrapper, left out of the browser copy", "warn");

  if (!ok) {
    setStatus(status, "This doesn't look like a complete Spider-Man PC install.", "bad");
    sourceDir = null;
  } else {
    setStatus(status, "Found a complete install. Your original folder won't be modified.", "ok");
    sourceDir = dir;
  }
  $("make-copy").disabled = !sourceDir;
}

// --- Browser copy --------------------------------------------------------------

async function planCopy(dir, prefix = "") {
  const plan = [];
  for await (const [name, handle] of dir.entries()) {
    const lower = name.toLowerCase();
    if (handle.kind === "directory") {
      if (prefix === "" && EXCLUDED_DIRS.has(lower)) continue;
      plan.push({ kind: "dir", path: prefix + name });
      plan.push(...(await planCopy(handle, prefix + name + "/")));
    } else if (!(prefix === "" && EXCLUDED.has(lower))) {
      plan.push({ kind: "file", path: prefix + name, handle, size: (await handle.getFile()).size });
    }
  }
  return plan;
}

async function resolveDir(root, path, create) {
  let dir = root;
  for (const part of path.split("/").filter(Boolean)) dir = await dir.getDirectoryHandle(part, { create });
  return dir;
}

async function makeCopy() {
  const status = $("copy-status");
  const bar = $("copy-progress");
  let dest;
  try {
    dest = await window.showDirectoryPicker({ id: "spiderman-browser-copy", mode: "readwrite" });
  } catch (e) {
    if (e.name !== "AbortError") setStatus(status, "Couldn't open the folder: " + e.message, "bad");
    return;
  }
  if (await dest.isSameEntry(sourceDir)) {
    setStatus(status, "Pick a different, empty folder. The original install must stay as it is.", "bad");
    return;
  }
  for await (const _ of dest.keys()) {
    setStatus(status, "That folder isn't empty. Create a new empty folder for the copy.", "bad");
    return;
  }

  $("make-copy").disabled = true;
  const plan = await planCopy(sourceDir);
  const total = plan.reduce((n, p) => n + (p.size || 0), 0);
  let done = 0;
  bar.hidden = false;
  bar.max = total;
  try {
    for (const item of plan) {
      if (item.kind === "dir") {
        await resolveDir(dest, item.path, true);
        continue;
      }
      setStatus(status, `Copying ${item.path} (${fmtSize(done)} of ${fmtSize(total)})`);
      const slash = item.path.lastIndexOf("/");
      const parent = await resolveDir(dest, slash < 0 ? "" : item.path.slice(0, slash), true);
      const out = await parent.getFileHandle(item.path.slice(slash + 1), { create: true });
      const writable = await out.createWritable();
      const file = await item.handle.getFile();
      await file.stream().pipeTo(writable);
      done += item.size;
      bar.value = done;
    }
  } catch (e) {
    setStatus(status, `Copy failed at ${fmtSize(done)}: ${e.message}`, "bad");
    $("make-copy").disabled = false;
    return;
  }
  setStatus(status, `Copied ${plan.filter((p) => p.kind === "file").length} files (${fmtSize(total)}) to "${dest.name}". Use this folder in step 3.`, "ok");
  markCopyReady();
}

function markCopyReady() {
  copyReady = true;
  $("launch").disabled = false;
  try { localStorage.setItem("spidey.copyReady", "1"); } catch {}
}

// Returning players: the copy already exists on disk and Boxedwine remembers
// the mounted folder, so steps 1 and 2 can be skipped.
function restoreCopyReady() {
  let ready = false;
  try { ready = localStorage.getItem("spidey.copyReady") === "1"; } catch {}
  if (ready) {
    markCopyReady();
    setStatus($("copy-status"), "Browser copy already made on an earlier visit. Go straight to step 3.", "ok");
  }
}

// --- Launch ----------------------------------------------------------------

function selectedRuntime() {
  return document.querySelector('input[name="runtime"]:checked').value;
}

function launchUrl() {
  const params = new URLSearchParams({
    root: ROOT_FS,
    storage: "hostfolder",
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

checkSupport();
const canIsolate = "serviceWorker" in navigator && window.isSecureContext;
$(canIsolate ? "rt-mt" : "rt-st").checked = true;
$("rt-mt").disabled = !canIsolate;
for (const r of document.querySelectorAll('input[name="runtime"]')) r.addEventListener("change", updateLaunchUrl);
updateLaunchUrl();
restoreCopyReady();

$("pick-source").addEventListener("click", pickSource);
$("make-copy").addEventListener("click", makeCopy);
$("skip-copy").addEventListener("click", () => {
  setStatus($("copy-status"), "Using an existing browser copy. Make sure it has no ddraw.dll, d3d8.dll or D3DImm.dll in it.", "warn");
  markCopyReady();
});
$("launch").addEventListener("click", () => {
  if (copyReady) location.href = launchUrl();
});
