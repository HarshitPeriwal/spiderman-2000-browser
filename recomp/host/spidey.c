/* Windows calls Spider-Man (2000) needs that the shared layer doesn't provide:
 *   KERNEL32  GetVersion
 *   USER32    MessageBeep
 *   WINMM     mmio* (RIFF/WAVE parsing, on files or memory buffers), mciSendCommandA
 */
#include "win32.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W(name) static void w_##name(CPU *c)

/* ------------------------------------------------------------------ kernel32 / user32 */

/* Windows XP SP2, matching GetVersionExA: build 2600 in the high word, 5.1 in the low word
   (bit 31 clear = NT). */
W(GetVersion) { RET(0, 0x0A280105u); }

W(MessageBeep) { RET(1, 1); }

/* ------------------------------------------------------------------ binkw32.dll's extra imports
 * binkw32.dll runs as translated guest code. The game plays movies with BinkCopyToBuffer into a
 * DirectDraw surface and BinkOpenDirectSound, so Bink's GDI window blitter and waveOut sound output are
 * not used; those calls report failure and are logged, so an unexpected use is visible.
 */
W(GetWindowsDirectoryA) {
    const char *d = "C:\\WINDOWS";
    uint32_t n = (uint32_t)strlen(d);
    if (ARG(1) > n) memcpy(HP(ARG(0)), d, n + 1);
    RET(2, ARG(1) > n ? n : n + 1);
}
W(GetSystemDirectoryA) {
    const char *d = "C:\\WINDOWS\\system32";
    uint32_t n = (uint32_t)strlen(d);
    if (ARG(1) > n) memcpy(HP(ARG(0)), d, n + 1);
    RET(2, ARG(1) > n ? n : n + 1);
}
W(GetEnvironmentVariableA) { if (t_self) t_self->last_error = 203; /* ERROR_ENVVAR_NOT_FOUND */ RET(3, 0); }
W(SetErrorMode) { static uint32_t mode; uint32_t old = mode; mode = ARG(0); RET(1, old); }

W(GetDC) { RET(1, 0x00050002u); }
W(ReleaseDC) { RET(2, 1); }
W(GetTopWindow) { RET(1, MAIN_HWND); }
W(GetWindow) { RET(2, 0); }
W(GetActiveWindow) { RET(0, MAIN_HWND); }
W(GetWindowThreadProcessId) { if (ARG(1)) W32(ARG(1), 0x10); RET(2, 0x100); }
W(GetClassLongA) { RET(2, 0); }
W(IsWindowVisible) { RET(1, 1); }
W(GetWindowRect) {
    uint32_t r = ARG(1);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, (uint32_t)g_screen_w); W32(r + 12, (uint32_t)g_screen_h);
    RET(2, 1);
}
W(ChangeDisplaySettingsA) { RET(2, 0 /* DISP_CHANGE_SUCCESSFUL */); }

/* GetDeviceCaps(hdc, nIndex) */
W(GetDeviceCaps) {
    switch (ARG(1)) {
    case 8: RET(2, g_screen_w);    /* HORZRES */
    case 10: RET(2, g_screen_h);   /* VERTRES */
    case 12: RET(2, 16);           /* BITSPIXEL, as the game's 16-bit display mode */
    case 14: RET(2, 1);            /* PLANES */
    case 116: RET(2, 60);          /* VREFRESH */
    default: RET(2, 0);
    }
}
W(SetStretchBltMode) { RET(2, 1); }
W(CreateCompatibleBitmap) { LOG("[gdi] CreateCompatibleBitmap (unused by the game)\n"); RET(3, 0); }
W(CreateDIBSection) { LOG("[gdi] CreateDIBSection (unused by the game)\n"); if (ARG(3)) W32(ARG(3), 0); RET(6, 0); }
W(GetDIBits) { LOG("[gdi] GetDIBits (unused by the game)\n"); RET(7, 0); }
W(StretchBlt) { LOG("[gdi] StretchBlt (unused by the game)\n"); RET(11, 0); }
W(SetPixel) { RET(4, 0xFFFFFFFFu); }

/* waveOut: MMSYSERR_NODRIVER (6) from waveOutOpen, so Bink keeps to DirectSound */
W(waveOutOpen) { LOG("[winmm] waveOutOpen -> MMSYSERR_NODRIVER\n"); if (ARG(0)) W32(ARG(0), 0); RET(6, 6); }
W(waveOutClose) { RET(1, 0); }
W(waveOutPrepareHeader) { RET(3, 0); }
W(waveOutUnprepareHeader) { RET(3, 0); }
W(waveOutWrite) { RET(3, 6); }
W(waveOutPause) { RET(1, 0); }
W(waveOutRestart) { RET(1, 0); }
W(waveOutReset) { RET(1, 0); }
W(waveOutSetVolume) { RET(2, 0); }

/* ------------------------------------------------------------------ mmio
 * An mmio handle is a byte stream with a position: either the whole file read into host memory
 * (the game only reads), or a guest memory buffer (MMIOINFO.fccIOProc == FOURCC_MEM).
 */
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FOURCC_RIFF FOURCC('R', 'I', 'F', 'F')
#define FOURCC_LIST FOURCC('L', 'I', 'S', 'T')
#define FOURCC_MEM  FOURCC('M', 'E', 'M', ' ')
#define MMIO_FINDCHUNK 0x10
#define MMIO_FINDRIFF  0x20
#define MMIO_FINDLIST  0x40
#define MMIOERR_CHUNKNOTFOUND 265
#define MMIOERR_CANNOTOPEN    257

typedef struct {
    int used;
    uint8_t *host;     /* file contents (owned), or NULL for a guest buffer */
    uint32_t guest;    /* guest buffer address */
    uint32_t size, pos;
} Mmio;

static Mmio g_mmio[64];

static Mmio *mmio_get(uint32_t h) { return h >= 1 && h <= 64 && g_mmio[h - 1].used ? &g_mmio[h - 1] : NULL; }

static uint32_t mmio_read(Mmio *m, uint32_t dst, uint32_t n) {
    if (m->pos >= m->size) return 0;
    if (n > m->size - m->pos) n = m->size - m->pos;
    for (uint32_t i = 0; i < n; i++) W8(dst + i, m->host ? m->host[m->pos + i] : R8(m->guest + m->pos + i));
    m->pos += n;
    return n;
}

static int mmio_u32(Mmio *m, uint32_t *v) {
    if (m->pos + 4 > m->size) return 0;
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = m->host ? m->host[m->pos + i] : R8(m->guest + m->pos + i);
    *v = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    m->pos += 4;
    return 1;
}

/* mmioOpenA(szFilename, lpmmioinfo, dwOpenFlags) */
W(mmioOpenA) {
    uint32_t info = ARG(1);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!g_mmio[i].used) { slot = i; break; }
    if (slot < 0) RET(3, 0);
    Mmio m = { 1, NULL, 0, 0, 0 };
    if (info && R32(info + 4) == FOURCC_MEM) {          /* MMIOINFO.fccIOProc, cchBuffer, pchBuffer */
        m.guest = R32(info + 24);
        m.size = R32(info + 20);
        LOG("[mmio] open memory %08x, %u bytes\n", m.guest, m.size);
    } else {
        const char *name = ARG(0) ? GSTR(ARG(0)) : "";
        char host[1024];
        map_path(name, host, sizeof host);
        int fd = open(host, O_RDONLY);
        if (fd < 0) {
            LOG("[mmio] open %s -> %s failed\n", name, host);
            if (info) W16(info + 12, MMIOERR_CANNOTOPEN);
            RET(3, 0);
        }
        off_t len = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        m.host = malloc(len > 0 ? (size_t)len : 1);
        m.size = (uint32_t)(len > 0 ? len : 0);
        if (m.size && read(fd, m.host, m.size) != (ssize_t)m.size) m.size = 0;
        close(fd);
        LOG("[mmio] open %s -> %s, %u bytes\n", name, host, m.size);
    }
    g_mmio[slot] = m;
    if (info) W16(info + 12, 0);
    RET(3, (uint32_t)slot + 1);
}

/* mmioRead(hmmio, pch, cch) */
W(mmioRead) {
    Mmio *m = mmio_get(ARG(0));
    if (!m) RET(3, 0xFFFFFFFFu);
    RET(3, mmio_read(m, ARG(1), ARG(2)));
}

/* mmioDescend(hmmio, lpck, lpckParent, wFlags). MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags */
W(mmioDescend) {
    Mmio *m = mmio_get(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) RET(4, MMIOERR_CHUNKNOTFOUND);
    uint32_t end = parent ? R32(parent + 12) + R32(parent + 4) : m->size;
    uint32_t want_id = R32(ck + 0), want_type = R32(ck + 8);
    for (;;) {
        uint32_t start = m->pos, id, size, type = 0;
        if (start + 8 > end || !mmio_u32(m, &id) || !mmio_u32(m, &size)) { m->pos = start; RET(4, MMIOERR_CHUNKNOTFOUND); }
        int list = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (list && !mmio_u32(m, &type)) { m->pos = start; RET(4, MMIOERR_CHUNKNOTFOUND); }
        int match = 1;
        if (flags & MMIO_FINDCHUNK) match = id == want_id;
        else if (flags & MMIO_FINDRIFF) match = id == FOURCC_RIFF && type == want_type;
        else if (flags & MMIO_FINDLIST) match = id == FOURCC_LIST && type == want_type;
        if (match) {
            W32(ck + 0, id);
            W32(ck + 4, size);
            W32(ck + 8, type);
            W32(ck + 12, start + 8);   /* data starts after the chunk header (fccType is part of it) */
            W32(ck + 16, 0);
            RET(4, 0);
        }
        m->pos = start + 8 + size + (size & 1);   /* chunks are word aligned */
    }
}

/* mmioAscend(hmmio, lpck, wFlags): move past the chunk entered with mmioDescend */
W(mmioAscend) {
    Mmio *m = mmio_get(ARG(0));
    if (!m) RET(3, MMIOERR_CHUNKNOTFOUND);
    uint32_t ck = ARG(1), size = R32(ck + 4);
    m->pos = R32(ck + 12) + size + (size & 1);
    RET(3, 0);
}

W(mmioClose) {
    Mmio *m = mmio_get(ARG(0));
    if (m) { free(m->host); memset(m, 0, sizeof *m); }
    RET(2, 0);
}

/* mmioStringToFOURCCA(sz, uFlags): MMIO_TOUPPER = 0x10 */
W(mmioStringToFOURCCA) {
    uint32_t s = ARG(0), v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t ch = R8(s + i);
        if (!ch) { for (; i < 4; i++) v |= (uint32_t)' ' << (8 * i); break; }
        if ((ARG(1) & 0x10) && ch >= 'a' && ch <= 'z') ch -= 32;
        v |= (uint32_t)ch << (8 * i);
    }
    RET(2, v);
}

/* mciSendCommandA(IDDevice, uMsg, fdwCommand, dwParam): Redbook CD audio. There is no CD drive, as on a
   PC without the disc in it, so opening a device fails and the game uses its own fallback. */
W(mciSendCommandA) {
    LOG("[mci] mciSendCommandA(dev %u, msg %#x, flags %#x) -> MCIERR_INVALID_DEVICE_NAME\n", ARG(0), ARG(1), ARG(2));
    RET(4, 256u + 7u);   /* MCIERR_BASE + 7 */
}

void w_DirectSoundCreate(CPU *c);   /* dsound.c: DirectSoundCreate8 takes the same arguments */
void w_DirectInput8Create(CPU *c);  /* dinput.c */

static const ImportHandler spidey_list[] = {
    { "DSOUND.DLL!#11", w_DirectSoundCreate },
    { "DINPUT8.DLL!DirectInput8Create", w_DirectInput8Create },
    IMPORT("KERNEL32.DLL", GetVersion), IMPORT("USER32.DLL", MessageBeep),
    IMPORT("WINMM.DLL", mmioOpenA), IMPORT("WINMM.DLL", mmioRead), IMPORT("WINMM.DLL", mmioDescend),
    IMPORT("WINMM.DLL", mmioAscend), IMPORT("WINMM.DLL", mmioClose), IMPORT("WINMM.DLL", mmioStringToFOURCCA),
    IMPORT("WINMM.DLL", mciSendCommandA),
    IMPORT("KERNEL32.DLL", GetWindowsDirectoryA), IMPORT("KERNEL32.DLL", GetSystemDirectoryA),
    IMPORT("KERNEL32.DLL", GetEnvironmentVariableA), IMPORT("KERNEL32.DLL", SetErrorMode),
    IMPORT("USER32.DLL", GetDC), IMPORT("USER32.DLL", ReleaseDC), IMPORT("USER32.DLL", GetTopWindow),
    IMPORT("USER32.DLL", GetWindow), IMPORT("USER32.DLL", GetActiveWindow),
    IMPORT("USER32.DLL", GetWindowThreadProcessId), IMPORT("USER32.DLL", GetClassLongA),
    IMPORT("USER32.DLL", IsWindowVisible), IMPORT("USER32.DLL", GetWindowRect),
    IMPORT("USER32.DLL", ChangeDisplaySettingsA),
    IMPORT("GDI32.DLL", GetDeviceCaps), IMPORT("GDI32.DLL", SetStretchBltMode),
    IMPORT("GDI32.DLL", CreateCompatibleBitmap), IMPORT("GDI32.DLL", CreateDIBSection), IMPORT("GDI32.DLL", GetDIBits),
    IMPORT("GDI32.DLL", StretchBlt), IMPORT("GDI32.DLL", SetPixel),
    IMPORT("WINMM.DLL", waveOutOpen), IMPORT("WINMM.DLL", waveOutClose), IMPORT("WINMM.DLL", waveOutPrepareHeader),
    IMPORT("WINMM.DLL", waveOutUnprepareHeader), IMPORT("WINMM.DLL", waveOutWrite), IMPORT("WINMM.DLL", waveOutPause),
    IMPORT("WINMM.DLL", waveOutRestart), IMPORT("WINMM.DLL", waveOutReset), IMPORT("WINMM.DLL", waveOutSetVolume),
};
const ImportTable spidey_imports = { spidey_list, sizeof spidey_list / sizeof spidey_list[0] };
