/* Native launcher: loads the game executable's image, wires up the Windows API layer and runs the game's
 * real entry point (the MSVC C runtime startup, which then calls WinMain).
 *
 * Usage: recomp [game-folder]      (default: ./game)
 * Env:   RC_EXE=name.exe      the executable inside the game folder (default: PROFILE_EXE)
 * Env:   RC_LOG=1          host-side log (files, registry, threads, ...)
 *        RC_LOG_IMPORTS=1  every Windows call the game makes
 *        RC_LOG_D3D=1|2    Direct3D object creation (1) or every Direct3D call (2)
 *        RC_LOG_FPS=1      frames per second
 *        RC_HEADLESS=1     no window (rendering skipped, Present paced at 60 Hz)
 *        RC_NO_AUDIO=1     mute
 */
#include "win32.h"
#include "render.h"

#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static LoadedImage g_img;
static CPU g_cpu;

/* DLLs that ship with the game and run as translated guest code (build.py translates them next to the
   executable). Each is optional: without its translated code it isn't loaded, and its imports stay
   host stubs. */
void register_all_functions_bink(void) __attribute__((weak));
static struct { const char *file; void (*reg)(void); LoadedImage img; int loaded; } g_dlls[] = {
    { "binkw32.dll", NULL },
};

/* RC_WATCHDOG=secs: periodically print the game thread's guest call stack (return addresses found
   on its stack), to see where it is when it seems stuck. */
static void *watchdog(void *arg) {
    int secs = (int)(intptr_t)arg;
    for (;;) {
        sleep((unsigned)secs);
        if (deterministic()) {   /* a stuck game thread that doesn't move these spins on memory */
            extern uint32_t g_det_reads, g_det_sleeps, g_det_waits;
            extern uint64_t g_vtime_us;
            fprintf(stderr, "[watchdog] virtual time %.6f s, game thread: clock reads %u, sleeps %u, waits %u\n",
                    g_vtime_us * 1e-6, g_det_reads, g_det_sleeps, g_det_waits);
        }
        extern GuestThread *g_all_threads[64];
        extern int g_nthreads;
        for (int t = -1; t < g_nthreads && t < 64; t++) {
            CPU *c = t < 0 ? &g_cpu : &g_all_threads[t]->cpu;
            fprintf(stderr, "[watchdog] %s %x esp=%08x stack:", t < 0 ? "game thread" : "thread", t < 0 ? 0x100 : g_all_threads[t]->id,
                    c->esp);
            for (int k = 0, n = 0; k < 2048 && n < 16; k++) {
                uint32_t v = R32(c->esp + 4 * k);
                if (v >= g_image_lo && v < g_image_hi) { fprintf(stderr, " %08x", v); n++; }
            }
            fprintf(stderr, "\n");
        }
    }
    return NULL;
}

static void *game_main(void *arg) {
    (void)arg;
    CPU *c = &g_cpu;
    thread_main_init(c);
    sched_main_init();
    t_self->cpu = *c;            /* keep the thread record consistent with the main CPU */
    fpu_sync_rounding(c);
    /* DllMain(hinstDLL, DLL_PROCESS_ATTACH, NULL) for each guest DLL, as the Windows loader does */
    for (size_t i = 0; i < sizeof g_dlls / sizeof g_dlls[0]; i++) {
        if (!g_dlls[i].loaded) continue;
        GuestFn dll_main = lookup_function(g_dlls[i].img.entry);
        if (!dll_main) { fprintf(stderr, "%s: entry point %08x was not recompiled\n", g_dlls[i].file, g_dlls[i].img.entry); exit(1); }
        uint32_t args[3] = { g_dlls[i].img.image_base, 1, 0 };
        guest_call(c, dll_main, 3, args);
        fprintf(stderr, "%s: DllMain returned %u\n", g_dlls[i].file, c->eax);
    }
    GuestFn entry = lookup_function(g_img.entry);
    if (!entry) { fprintf(stderr, "entry point %08x was not recompiled\n", g_img.entry); exit(1); }
    fprintf(stderr, "starting game at %08x\n", g_img.entry);
    PUSH32(c, GUEST_RETURN_SENTINEL);
    entry(c);
    fprintf(stderr, "game returned %d\n", (int)c->eax);
    exit((int)c->eax);
}

#if defined(__EMSCRIPTEN__) && !defined(RC_NODE_BUILD)
#include <emscripten/wasmfs.h>
#endif

int main(int argc, char **argv) {
#if defined(__EMSCRIPTEN__) && !defined(RC_NODE_BUILD)
    /* browser: the page copied the player's game folder into OPFS; reach it through /opfs */
    wasmfs_create_directory("/opfs", 0777, wasmfs_create_opfs_backend());
#endif
    const char *game = argc > 1 ? argv[1] : "game";
    if (!realpath(game, g_game_root)) { perror(game); return 1; }
    /* RC_USERDATA: another folder for saves and settings (e.g. a fresh copy per test run) */
    if (getenv("RC_USERDATA")) snprintf(g_user_root, sizeof g_user_root, "%s", getenv("RC_USERDATA"));
    else snprintf(g_user_root, sizeof g_user_root, "%s/../userdata", g_game_root);
    mkdir(g_user_root, 0755);
    g_log = getenv("RC_LOG") != NULL;
    extern int g_log_d3d;
    g_log_d3d = getenv("RC_LOG_D3D") ? atoi(getenv("RC_LOG_D3D")) : 0;

    static ImportHandler all[1024];
    uint32_t n = 0;
    const ImportTable *tables[] = { &spidey_imports, &ddraw7_imports, &kernel32_imports, &user32_imports, &gdi32_imports, &advapi32_imports,
                                    &winmm_imports, &directx_imports };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (uint32_t i = 0; i < tables[t]->count; i++) all[n++] = tables[t]->list[i];
    set_import_handlers(all, n);

    runtime_init(&g_cpu);
    vclock_init();
    g_dlls[0].reg = register_all_functions_bink;
    for (size_t i = 0; i < sizeof g_dlls / sizeof g_dlls[0]; i++) {
        if (!g_dlls[i].reg) continue;
        char dll[1100];
        snprintf(dll, sizeof dll, "%s/%s", g_game_root, g_dlls[i].file);
        if (load_image(dll, &g_dlls[i].img)) return 1;
        g_dlls[i].reg();
        g_dlls[i].loaded = 1;
        fprintf(stderr, "loaded %s at %08x as guest code\n", dll, g_dlls[i].img.image_base);
    }
    char exe[1100];
    snprintf(exe, sizeof exe, "%s/%s", g_game_root, getenv("RC_EXE") ? getenv("RC_EXE") : PROFILE_EXE);
    if (load_image(exe, &g_img)) return 1;
    register_all_functions();
    fprintf(stderr, "loaded %s: %u imports, %u of them implemented\n", exe, g_img.num_imports, n);

    /* The window and its event loop must live on the main thread (macOS); the game gets its own
       thread. RC_HEADLESS=1 runs without a window (no rendering, 60 Hz pacing). */
    int headless = getenv("RC_HEADLESS") != NULL;
    extern int window_create(const char *title);
    extern void window_event_loop(void);
#ifdef __EMSCRIPTEN__
    /* browser: the page owns the canvas and replays our frames; this thread just waits for the game */
    if (!headless) g_render = &g_render_gpu;
    headless = 1;
#else
    if (!headless && window_create(PROFILE_TITLE)) g_render = &g_render_gpu;
    else headless = 1;
#endif

    /* Recompiled code nests one host frame per guest call, so give the game a generous host stack. */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u << 20);
    pthread_t th;
    pthread_create(&th, &at, game_main, NULL);
    if (getenv("RC_WATCHDOG")) {
        pthread_t wd;
        pthread_create(&wd, NULL, watchdog, (void *)(intptr_t)atoi(getenv("RC_WATCHDOG")));
    }
    if (headless) pthread_join(th, NULL);
    else window_event_loop();  /* never returns; closing the window exits */
    return 0;
}
