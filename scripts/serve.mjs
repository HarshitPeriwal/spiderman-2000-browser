// Minimal static server for testing dist/ locally.
// Usage: node scripts/serve.mjs [port] [dir]
// Sends COOP/COEP so the multi-threaded runtime works without the service worker.

import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { extname, join, normalize, resolve } from "node:path";

const port = Number(process.argv[2] || 8080);
const root = resolve(process.argv[3] || "dist");

const types = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript",
  ".css": "text/css", ".json": "application/json", ".wasm": "application/wasm",
  ".zip": "application/zip", ".md": "text/markdown; charset=utf-8",
};

createServer((req, res) => {
  const url = new URL(req.url, "http://localhost");
  let path = normalize(join(root, decodeURIComponent(url.pathname)));
  if (!path.startsWith(root)) {
    res.writeHead(403).end();
    return;
  }
  try {
    if (statSync(path).isDirectory()) path = join(path, "index.html");
    const { size } = statSync(path);
    res.writeHead(200, {
      "Content-Type": types[extname(path)] || "application/octet-stream",
      "Content-Length": size,
      "Cross-Origin-Opener-Policy": "same-origin",
      "Cross-Origin-Embedder-Policy": "require-corp",
      "Cross-Origin-Resource-Policy": "same-origin",
    });
    createReadStream(path).pipe(res);
  } catch {
    res.writeHead(404).end("not found");
  }
}).listen(port, "127.0.0.1", () => console.log(`serving ${root} at http://localhost:${port}/`));
