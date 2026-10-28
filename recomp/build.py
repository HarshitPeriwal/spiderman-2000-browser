#!/usr/bin/env python3
"""Builds the recompiled Spider-Man with Emscripten (run inside WSL/Linux with emcc on PATH).

  build.py translate <SpideyPC.exe> <functions.json>   x86 -> C into work/out (Windows or Linux)
  build.py translate-dll <file.dll> <functions.json> <prefix>   a guest DLL into work/out_<prefix>
  build.py node                                       headless test build: <build>/node/recomp.js
  build.py web                                        browser build:       <build>/www/recomp.js

Objects are cached by content hash under $SPIDEY_BUILD (default ~/spidey-build), so only changed
files are recompiled. Paths may contain spaces. Run the node build with:
  node <build>/node/recomp.js "<game folder>"
"""
import concurrent.futures as cf
import glob
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "work", "out")
BUILD = os.environ.get("SPIDEY_BUILD", os.path.expanduser("~/spidey-build"))
OPT = os.environ.get("OPT", "-O2")
JOBS = int(os.environ.get("JOBS", "6"))
CFLAGS = [OPT, "-pthread", "-frounding-math", "-w"]
HOST_SKIP = {"window.c", "watch.c"}          # SDL window and signal watchpoints: native only
NODE_HOST_SKIP = HOST_SKIP | {"render_wgpu.c"}  # no WebGPU executor under Node


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"failed ({r.returncode}): {' '.join(cmd[:6])} ...")


def compile_one(src, obj, flags):
    key = hashlib.sha1(open(src, "rb").read() + " ".join(flags).encode()).hexdigest()
    stamp = obj + ".hash"
    if os.path.exists(obj) and os.path.exists(stamp) and open(stamp).read() == key:
        return None
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run(["emcc", *flags, "-c", src, "-o", obj], capture_output=True, text=True)
    if r.returncode:
        return f"{os.path.basename(src)}:\n{r.stderr[-4000:]}"
    open(stamp, "w").write(key)
    return f"compiled {os.path.basename(src)}"


def compile_all(jobs):
    errors = []
    with cf.ThreadPoolExecutor(JOBS) as ex:
        for msg in ex.map(lambda j: compile_one(*j), jobs):
            if msg and msg.startswith("compiled"):
                print(" ", msg, flush=True)
            elif msg:
                errors.append(msg)
    if errors:
        sys.exit("\n".join(errors))


def objects(mode):
    inc = ["-I" + os.path.join(HERE, "runtime"), "-I" + OUT]
    hinc = inc + ["-I" + os.path.join(HERE, "host")]
    gen = sorted(glob.glob(os.path.join(OUT, "*.c")))
    if not gen:
        sys.exit("no translated code in work/out; run: build.py translate <exe> <functions.json>")
    jobs = [(f, os.path.join(BUILD, "obj", os.path.basename(f)[:-2] + ".o"), CFLAGS + inc) for f in gen]
    for d in sorted(glob.glob(os.path.join(HERE, "work", "out_*"))):   # translated guest DLLs
        dinc = ["-I" + os.path.join(HERE, "runtime"), "-I" + d]
        jobs += [(f, os.path.join(BUILD, "obj", os.path.basename(f)[:-2] + ".o"), CFLAGS + dinc)
                 for f in sorted(glob.glob(os.path.join(d, "*.c")))]
    for name in ("runtime.c", "interp.c"):
        jobs.append((os.path.join(HERE, "runtime", name), os.path.join(BUILD, "rt", name[:-2] + ".o"), CFLAGS + inc))
    skip = NODE_HOST_SKIP if mode == "node" else HOST_SKIP
    hflags = CFLAGS + hinc + (["-DRC_NODE_BUILD"] if mode == "node" else ["--use-port=emdawnwebgpu"])
    for f in sorted(glob.glob(os.path.join(HERE, "host", "*.c"))):
        if os.path.basename(f) not in skip:
            jobs.append((f, os.path.join(BUILD, mode, "host", os.path.basename(f)[:-2] + ".o"), hflags))
    if mode == "node":
        jobs.append((os.path.join(HERE, "web", "node_stubs.c"), os.path.join(BUILD, "node", "node_stubs.o"), CFLAGS))
    # drop objects of translated files that no longer exist
    live = {j[1] for j in jobs}
    for o in glob.glob(os.path.join(BUILD, "obj", "*.o")):
        if o not in live:
            os.remove(o)
    compile_all(jobs)
    return [j[1] for j in jobs]


def link(mode, objs):
    common = ["-pthread", OPT, "--profiling-funcs", "-sALLOW_MEMORY_GROWTH", "-sMAXIMUM_MEMORY=4GB",
              "-sSTACK_SIZE=8MB", "-sDEFAULT_PTHREAD_STACK_SIZE=32MB", "-sPROXY_TO_PTHREAD"]
    if mode == "node":
        out = os.path.join(BUILD, "node", "recomp.js")
        run(["emcc", *common, "-o", out, *objs, "-sPTHREAD_POOL_SIZE=12", "-sENVIRONMENT=node",
             "-sNODERAWFS=1", "-sEXIT_RUNTIME=1", "--pre-js", os.path.join(HERE, "web", "node_env.js")])
        # Node gives each worker a 4 MB engine stack; recompiled code nests one engine frame per guest call
        js = open(out).read()
        js2 = re.sub(r'("?name"?:\s*"em-pthread")', r'\1, resourceLimits: { stackSizeMb: 256 }', js)
        if js2 == js:
            print("warning: worker stack size patch did not apply", file=sys.stderr)
        open(out, "w").write(js2)
    else:
        out = os.path.join(BUILD, "www", "recomp.js")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        run(["emcc", *common, "-o", out, *objs, "--use-port=emdawnwebgpu", "-sWASMFS",
             "-sPTHREAD_POOL_SIZE=16", "-sENVIRONMENT=web,worker", "-sMODULARIZE", "-sEXPORT_NAME=createRecomp",
             "-sEXPORTED_RUNTIME_METHODS=ENV,wasmMemory", "-sEXIT_RUNTIME=0"])
    print("built", out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mode = sys.argv[1]
    if mode == "translate":
        exe, funcs = sys.argv[2], sys.argv[3]
        run([sys.executable, "-E", "-P", os.path.join(HERE, "translator", "x86toc.py"), exe, funcs, OUT,
             "--all", "--per-file", "300"])
        return
    if mode == "translate-dll":
        dll, funcs, prefix = sys.argv[2], sys.argv[3], sys.argv[4]
        run([sys.executable, "-E", "-P", os.path.join(HERE, "translator", "x86toc.py"), dll, funcs,
             os.path.join(HERE, "work", "out_" + prefix), "--all", "--per-file", "300", "--prefix", prefix])
        return
    if mode not in ("node", "web"):
        sys.exit(__doc__)
    link(mode, objects(mode))


if __name__ == "__main__":
    main()
