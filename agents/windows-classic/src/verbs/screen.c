/*   Copyright 2026 William Isted and contributors
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "screen.h"
#include "common.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Multi-monitor metrics arrived with the Win98/2000 SDK; a barebones VC98
 * winuser.h (if MSVC6 include precedence wins over the Platform SDK) lacks
 * them. The runtime values are stable -- define them defensively. */
#ifndef SM_XVIRTUALSCREEN
#define SM_XVIRTUALSCREEN  76
#endif
#ifndef SM_YVIRTUALSCREEN
#define SM_YVIRTUALSCREEN  77
#endif
#ifndef SM_CXVIRTUALSCREEN
#define SM_CXVIRTUALSCREEN 78
#endif
#ifndef SM_CYVIRTUALSCREEN
#define SM_CYVIRTUALSCREEN 79
#endif

/* GDI BitBlt capture into a bottom-up 24bpp BGR DIB, then encode as BMP
 * (native BGR, no conversion) or PNG (a self-contained stored-DEFLATE
 * encoder: no zlib, no GDI+ -- only kernel32/gdi32/user32, satisfying
 * D10's "no GDI+ bundling"). */

/* --- captured frame ---------------------------------------------------- */

typedef struct {
    unsigned char* bits;   /* bottom-up BGR, 4-byte aligned rows */
    int            w;
    int            h;
    int            stride;
} Frame;

static void frame_free(Frame* f)
{
    if (f->bits != NULL) {
        free(f->bits);
        f->bits = NULL;
    }
}

/* Capture screen rect (sx,sy,w,h) in virtual-desktop coordinates. Returns
 * 1 on success. */
static int capture_rect(int sx, int sy, int w, int h, Frame* out)
{
    HDC              screen;
    HDC              mem;
    HBITMAP          dib;
    HGDIOBJ          old;
    BITMAPINFOHEADER bi;
    int              stride;
    unsigned char*   buf;
    int              ok;

    if (w <= 0 || h <= 0) {
        return 0;
    }
    stride = ((w * 3) + 3) & ~3;

    screen = GetDC(NULL);
    if (screen == NULL) {
        return 0;
    }
    mem = CreateCompatibleDC(screen);
    if (mem == NULL) {
        ReleaseDC(NULL, screen);
        return 0;
    }
    dib = CreateCompatibleBitmap(screen, w, h);
    if (dib == NULL) {
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return 0;
    }
    old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, sx, sy, SRCCOPY);

    buf = (unsigned char*)malloc((size_t)stride * (size_t)h);
    if (buf == NULL) {
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return 0;
    }

    memset(&bi, 0, sizeof(bi));
    bi.biSize        = sizeof(BITMAPINFOHEADER);
    bi.biWidth       = w;
    bi.biHeight      = h;          /* positive => bottom-up rows */
    bi.biPlanes      = 1;
    bi.biBitCount    = 24;
    bi.biCompression = BI_RGB;

    ok = GetDIBits(mem, dib, 0, (UINT)h, buf,
                   (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    if (ok == 0) {
        free(buf);
        return 0;
    }
    out->bits   = buf;
    out->w      = w;
    out->h      = h;
    out->stride = stride;
    return 1;
}

/* --- little-endian / big-endian byte writers --------------------------- */

static void put_le32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_be32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

/* --- BMP encoder (BGR, native; no conversion) -------------------------- */

static int bmp_encode(const Frame* f, unsigned char** out, int* outlen)
{
    int            imgsize;
    int            total;
    unsigned char* b;

    imgsize = f->stride * f->h;
    total   = 14 + 40 + imgsize;
    b = (unsigned char*)malloc((size_t)total);
    if (b == NULL) {
        return 0;
    }

    /* BITMAPFILEHEADER (14 bytes, written by hand -- the struct is
     * 2-byte-packed and writing it as a struct is fragile on VS6). */
    b[0] = 'B';
    b[1] = 'M';
    put_le32(b + 2, (unsigned long)total);
    b[6] = 0; b[7] = 0; b[8] = 0; b[9] = 0;
    put_le32(b + 10, 54);                       /* bfOffBits */

    /* BITMAPINFOHEADER (40 bytes). */
    put_le32(b + 14, 40);
    put_le32(b + 18, (unsigned long)f->w);
    put_le32(b + 22, (unsigned long)f->h);      /* bottom-up */
    b[26] = 1; b[27] = 0;                        /* biPlanes  */
    b[28] = 24; b[29] = 0;                       /* biBitCount */
    put_le32(b + 30, 0);                         /* BI_RGB */
    put_le32(b + 34, (unsigned long)imgsize);
    put_le32(b + 38, 2835);
    put_le32(b + 42, 2835);
    put_le32(b + 46, 0);
    put_le32(b + 50, 0);

    memcpy(b + 54, f->bits, (size_t)imgsize);

    *out    = b;
    *outlen = total;
    return 1;
}

/* --- PNG encoder (stored DEFLATE; no zlib) ----------------------------- */

static unsigned long g_crc_table[256];
static int           g_crc_ready = 0;

static void crc_init(void)
{
    unsigned long c;
    int           n;
    int           k;

    for (n = 0; n < 256; ++n) {
        c = (unsigned long)n;
        for (k = 0; k < 8; ++k) {
            if (c & 1UL) {
                c = 0xedb88320UL ^ (c >> 1);
            } else {
                c = c >> 1;
            }
        }
        g_crc_table[n] = c;
    }
    g_crc_ready = 1;
}

static unsigned long crc32(const unsigned char* p, int n)
{
    unsigned long c;
    int           i;

    if (!g_crc_ready) {
        crc_init();
    }
    c = 0xffffffffUL;
    for (i = 0; i < n; ++i) {
        c = g_crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffffUL;
}

static unsigned long adler32(const unsigned char* p, int n)
{
    unsigned long a;
    unsigned long b;
    int           i;

    a = 1UL;
    b = 0UL;
    for (i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521UL;
        b = (b + a) % 65521UL;
    }
    return (b << 16) | a;
}

/* Build the raw (unfiltered, filter-type-0) RGB scanlines top-down from the
 * bottom-up BGR DIB, swapping B<->R. Returns malloc'd buffer + length. */
static unsigned char* build_raw_scanlines(const Frame* f, int* raw_len)
{
    int            rl;
    unsigned char* raw;
    int            y;
    int            x;
    int            di;

    rl  = f->h * (1 + f->w * 3);
    raw = (unsigned char*)malloc((size_t)rl);
    if (raw == NULL) {
        return NULL;
    }
    di = 0;
    for (y = 0; y < f->h; ++y) {
        const unsigned char* src;
        /* DIB is bottom-up: top image row is the last DIB row. */
        src = f->bits + (size_t)(f->h - 1 - y) * (size_t)f->stride;
        raw[di++] = 0;                       /* filter: None */
        for (x = 0; x < f->w; ++x) {
            unsigned char bch = src[x * 3 + 0];
            unsigned char gch = src[x * 3 + 1];
            unsigned char rch = src[x * 3 + 2];
            raw[di++] = rch;
            raw[di++] = gch;
            raw[di++] = bch;
        }
    }
    *raw_len = rl;
    return raw;
}

static int png_encode(const Frame* f, unsigned char** out, int* outlen)
{
    unsigned char* raw;
    int            raw_len;
    int            nblocks;
    int            zlib_len;
    int            idat_len;
    int            total;
    unsigned char* b;
    int            pos;
    int            ihdr_at;
    int            idat_at;
    int            iend_at;
    int            off;
    int            blk;
    unsigned long  ad;

    raw = build_raw_scanlines(f, &raw_len);
    if (raw == NULL) {
        return 0;
    }

    /* Stored DEFLATE: one block per <=65535-byte chunk. */
    nblocks  = (raw_len + 65534) / 65535;
    if (nblocks == 0) {
        nblocks = 1;
    }
    zlib_len = 2 + (5 * nblocks) + raw_len + 4;   /* CMF/FLG + blocks + adler */
    idat_len = zlib_len;

    total = 8                                   /* signature            */
          + (12 + 13)                           /* IHDR                 */
          + (12 + idat_len)                     /* IDAT                 */
          + (12 + 0);                           /* IEND                 */

    b = (unsigned char*)malloc((size_t)total);
    if (b == NULL) {
        free(raw);
        return 0;
    }

    pos = 0;
    /* Signature. */
    b[pos++] = 0x89; b[pos++] = 0x50; b[pos++] = 0x4e; b[pos++] = 0x47;
    b[pos++] = 0x0d; b[pos++] = 0x0a; b[pos++] = 0x1a; b[pos++] = 0x0a;

    /* IHDR. */
    put_be32(b + pos, 13); pos += 4;
    ihdr_at = pos;
    memcpy(b + pos, "IHDR", 4); pos += 4;
    put_be32(b + pos, (unsigned long)f->w); pos += 4;
    put_be32(b + pos, (unsigned long)f->h); pos += 4;
    b[pos++] = 8;     /* bit depth   */
    b[pos++] = 2;     /* color type 2 = truecolour RGB */
    b[pos++] = 0;     /* compression */
    b[pos++] = 0;     /* filter      */
    b[pos++] = 0;     /* interlace   */
    put_be32(b + pos, crc32(b + ihdr_at, pos - ihdr_at)); pos += 4;

    /* IDAT. */
    put_be32(b + pos, (unsigned long)idat_len); pos += 4;
    idat_at = pos;
    memcpy(b + pos, "IDAT", 4); pos += 4;
    b[pos++] = 0x78;  /* zlib CMF */
    b[pos++] = 0x01;  /* zlib FLG (no dict, fastest); 0x7801 % 31 == 0 */

    off = 0;
    for (blk = 0; blk < nblocks; ++blk) {
        int chunk = raw_len - off;
        if (chunk > 65535) {
            chunk = 65535;
        }
        b[pos++] = (unsigned char)((blk == nblocks - 1) ? 1 : 0); /* BFINAL, BTYPE=00 */
        b[pos++] = (unsigned char)(chunk & 0xff);
        b[pos++] = (unsigned char)((chunk >> 8) & 0xff);
        b[pos++] = (unsigned char)((~chunk) & 0xff);
        b[pos++] = (unsigned char)(((~chunk) >> 8) & 0xff);
        memcpy(b + pos, raw + off, (size_t)chunk);
        pos += chunk;
        off += chunk;
    }
    ad = adler32(raw, raw_len);
    put_be32(b + pos, ad); pos += 4;
    put_be32(b + pos, crc32(b + idat_at, pos - idat_at)); pos += 4;

    /* IEND. */
    put_be32(b + pos, 0); pos += 4;
    iend_at = pos;
    memcpy(b + pos, "IEND", 4); pos += 4;
    put_be32(b + pos, crc32(b + iend_at, 4)); pos += 4;

    free(raw);
    *out    = b;
    *outlen = pos;
    return 1;
}

/* --- the verb ---------------------------------------------------------- */

void rh_verb_screen_capture(RhConn* c, const RhRequest* req)
{
    static const RhFlagDef defs[] = {
        { "--format",  1 },
        { "--region",  1 },
        { "--window",  1 },
        { "--monitor", 1 },
        { "--no-cursor", 0 }
    };
    RhArgs         a;
    const char*    fmt;
    int            want_png;
    int            sx;
    int            sy;
    int            w;
    int            h;
    Frame          frame;
    unsigned char* enc;
    int            enclen;
    int            ok;

    rh_args_parse(req, defs, 5, &a);

    if (a.unknown != NULL) {
        rh_err_unknown_flag(c, a.unknown);
        return;
    }
    if (a.missing_val) {
        rh_err_msg(c, "invalid_args", "flag requires a value");
        return;
    }
    /* --region / --window are mutually exclusive (and with --monitor). */
    if ((a.seen[1] && a.seen[2]) ||
        (a.seen[1] && a.seen[3]) ||
        (a.seen[2] && a.seen[3])) {
        rh_err_msg(c, "invalid_args",
                   "--region / --window / --monitor are mutually exclusive");
        return;
    }

    fmt = a.val[0] ? a.val[0] : "png";
    if (strcmp(fmt, "png") == 0) {
        want_png = 1;
    } else if (strcmp(fmt, "bmp") == 0) {
        want_png = 0;
    } else {
        /* webp / webp:<q> / tiff-2000 / anything else: classic does not
         * carry these encoders. PROTOCOL.md 5.1 -> not_supported. */
        rh_err_kv(c, "not_supported", "format", fmt);
        return;
    }

    /* Resolve the capture rectangle. */
    sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
        /* NT4 has no virtual-screen metrics: fall back to the primary. */
        sx = 0;
        sy = 0;
        w  = GetSystemMetrics(SM_CXSCREEN);
        h  = GetSystemMetrics(SM_CYSCREEN);
    }

    if (a.seen[1]) {                 /* --region x,y,w,h */
        int rx;
        int ry;
        int rw;
        int rh_;
        if (a.val[1] == NULL ||
            sscanf(a.val[1], "%d,%d,%d,%d", &rx, &ry, &rw, &rh_) != 4 ||
            rw <= 0 || rh_ <= 0) {
            rh_err_msg(c, "invalid_args", "bad --region (want x,y,w,h)");
            return;
        }
        sx = rx; sy = ry; w = rw; h = rh_;
    } else if (a.seen[2]) {          /* --window win:0x... */
        HWND rect_w;
        RECT rc;
        if (a.val[2] == NULL || !rh_hwnd_parse(a.val[2], &rect_w)) {
            rh_err_msg(c, "invalid_args", "bad --window handle");
            return;
        }
        if (!IsWindow(rect_w) || !GetWindowRect(rect_w, &rc)) {
            rh_err_kv(c, "target_gone", "handle", a.val[2]);
            return;
        }
        sx = rc.left;
        sy = rc.top;
        w  = rc.right - rc.left;
        h  = rc.bottom - rc.top;
    } else if (a.seen[3]) {          /* --monitor <index> */
        int mi = a.val[3] ? atoi(a.val[3]) : 0;
        if (mi != 0) {
            /* Multi-monitor enumeration (EnumDisplayMonitors) is Win98+/
             * Win2000+; classic spans down to NT4 where it is absent.
             * Index 0 == primary always works; others -> not_found. */
            rh_err_kv(c, "not_found", "monitor", a.val[3]);
            return;
        }
        sx = 0;
        sy = 0;
        w  = GetSystemMetrics(SM_CXSCREEN);
        h  = GetSystemMetrics(SM_CYSCREEN);
    }
    /* --no-cursor (a.seen[4]): classic never composites the cursor, so the
     * flag is accepted and is already satisfied. */

    frame.bits = NULL;
    if (!capture_rect(sx, sy, w, h, &frame)) {
        rh_err_msg(c, "permission_denied", "screen capture failed");
        return;
    }

    if (want_png) {
        ok = png_encode(&frame, &enc, &enclen);
    } else {
        ok = bmp_encode(&frame, &enc, &enclen);
    }
    frame_free(&frame);

    if (!ok) {
        rh_err_msg(c, "wire_desync", "image encode failed");
        return;
    }
    rh_ok_bytes(c, (const char*)enc, enclen);
    free(enc);
}
