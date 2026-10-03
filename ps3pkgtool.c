/*
 * ps3pkgtool - fast PS3 PKG extractor / creator / splitter / joiner
 *
 * Single-file C, no dependencies.
 *   Linux/macOS : cc -O2 ps3pkgtool.c -o ps3pkgtool -lpthread
 *   Windows     : gcc -O2 ps3pkgtool.c -o ps3pkgtool.exe -lshell32   (mingw-w64)
 *
 * Format knowledge comes from public homebrew sources (psdevwiki, PSL1GHT's
 * pkg.py, RPCS3). Created packages are debug-style ("non finalized") packages
 * with the same layout PSL1GHT produces.
 */
#define _FILE_OFFSET_BITS 64
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#define fseek64 fseeko
#define ftell64 ftello
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#include <cpuid.h>
#define HAVE_X86 1
#endif

#define VERSION "1.4.2"
#define BUFSZ (16u * 1024 * 1024)
#define ALIGN16(x) (((x) + 15ULL) & ~15ULL)

static void die(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2), noreturn))
#endif
    ;
#include <stdarg.h>
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\nerror: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

/* ------------------------------------------------------------------ */
/* big endian helpers                                                  */
/* ------------------------------------------------------------------ */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

/* ------------------------------------------------------------------ */
/* platform layer (UTF-8 paths everywhere)                             */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static FILE *xfopen(const char *p, const char *m)
{
    wchar_t *wp = widen(p), *wm = widen(m);
    FILE *f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
}
static int xmkdir(const char *p)
{
    wchar_t *w = widen(p);
    int ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok ? 0 : -1;
}
static int path_stat(const char *p, int *is_dir, uint64_t *size)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = widen(p);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) return -1;
    *is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    return 0;
}
static double now(void) { return GetTickCount64() / 1000.0; }
#else
static FILE *xfopen(const char *p, const char *m) { return fopen(p, m); }
static int xmkdir(const char *p) { return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1; }
static int path_stat(const char *p, int *is_dir, uint64_t *size)
{
    struct stat st;
    if (stat(p, &st)) return -1;
    *is_dir = S_ISDIR(st.st_mode);
    *size = (uint64_t)st.st_size;
    return 0;
}
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

static char *pathcat(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *r = xmalloc(la + lb + 2);
    memcpy(r, a, la);
    r[la] = '/';
    memcpy(r + la + 1, b, lb + 1);
    return r;
}

/* create every directory in `path`; if `last` is 0 the final component is a file */
static void mkdirs(const char *path, int last)
{
    char *p = xmalloc(strlen(path) + 1);
    strcpy(p, path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/' || *s == '\\') {
            char c = *s;
            *s = 0;
            if (!(s > p && s[-1] == ':')) xmkdir(p);
            *s = c;
        }
    }
    if (last && xmkdir(p)) die("cannot create directory '%s'", p);
    free(p);
}

static uint64_t file_size(FILE *f)
{
    fseek64(f, 0, SEEK_END);
    uint64_t s = (uint64_t)ftell64(f);
    fseek64(f, 0, SEEK_SET);
    return s;
}

/* ------------------------------------------------------------------ */
/* progress                                                            */
/* ------------------------------------------------------------------ */
static double g_t0, g_last;
static uint64_t g_done, g_total;
static void progress_start(uint64_t total) { g_t0 = g_last = now(); g_done = 0; g_total = total; }
static void progress(int force)
{
    double t = now();
    if (!force && t - g_last < 0.2) return;
    g_last = t;
    double el = t - g_t0, mb = g_done / 1048576.0;
    fprintf(stderr, "\r  %5.1f%%  %.0f / %.0f MB  %.0f MB/s   ",
            g_total ? 100.0 * g_done / g_total : 100.0, mb, g_total / 1048576.0,
            el > 0.001 ? mb / el : 0.0);
    if (force) fprintf(stderr, "\n");
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* SHA-1                                                               */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; unsigned n; } Sha1;
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static const uint32_t SHA1_IV[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };

static void sha1_compress(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], t;
    int i;
    for (i = 0; i < 16; i++) w[i] = rd32(p + 4 * i);
    for (; i < 80; i++) { t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]; w[i] = ROL(t, 1); }
#define RND(f, k) t = ROL(a, 5) + (f) + e + (k) + w[i]; e = d; d = c; c = ROL(b, 30); b = a; a = t;
    for (i = 0; i < 20; i++) { RND((b & c) | (~b & d), 0x5A827999) }
    for (; i < 40; i++) { RND(b ^ c ^ d, 0x6ED9EBA1) }
    for (; i < 60; i++) { RND((b & c) | (b & d) | (c & d), 0x8F1BBCDC) }
    for (; i < 80; i++) { RND(b ^ c ^ d, 0xCA62C1D6) }
#undef RND
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void sha1_init(Sha1 *s) { memcpy(s->h, SHA1_IV, sizeof s->h); s->len = 0; s->n = 0; }
static void sha1_update(Sha1 *s, const uint8_t *p, size_t len)
{
    s->len += len;
    if (s->n) {
        size_t k = 64 - s->n;
        if (k > len) k = len;
        memcpy(s->buf + s->n, p, k);
        s->n += (unsigned)k; p += k; len -= k;
        if (s->n == 64) { sha1_compress(s->h, s->buf); s->n = 0; }
    }
    for (; len >= 64; p += 64, len -= 64) sha1_compress(s->h, p);
    if (len) { memcpy(s->buf, p, len); s->n = (unsigned)len; }
}
static void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t padlen = (s->n < 56) ? 56 - s->n : 120 - s->n;
    wr64(pad + padlen, bits);
    sha1_update(s, pad, padlen + 8);
    for (int i = 0; i < 5; i++) wr32(out + 4 * i, s->h[i]);
}
static void sha1(const uint8_t *p, size_t len, uint8_t out[20])
{
    Sha1 s;
    sha1_init(&s); sha1_update(&s, p, len); sha1_final(&s, out);
}

/* ------------------------------------------------------------------ */
/* AES-128 (encrypt only, that's all CTR needs)                        */
/* ------------------------------------------------------------------ */
static uint8_t sbox[256];
static int use_aesni = 0;

static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }
static void aes_init(void)
{
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
#ifdef HAVE_X86
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 25)) && (c & (1u << 9))) use_aesni = 1;
#endif
}
static void aes_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rc = 1;
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rc = (uint8_t)((rc << 1) ^ ((rc & 0x80) ? 0x1B : 0));
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
}
#define XT(x) ((uint8_t)(((x) << 1) ^ (((x) & 0x80) ? 0x1B : 0)))
static void aes_enc(const uint8_t *rk, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) t[4 * c + row] = sbox[s[4 * ((c + row) & 3) + row]];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c]     = XT(a0) ^ XT(a1) ^ a1 ^ a2 ^ a3;
                s[4 * c + 1] = a0 ^ XT(a1) ^ XT(a2) ^ a2 ^ a3;
                s[4 * c + 2] = a0 ^ a1 ^ XT(a2) ^ XT(a3) ^ a3;
                s[4 * c + 3] = XT(a0) ^ a0 ^ a1 ^ a2 ^ XT(a3);
            }
        } else memcpy(s, t, 16);
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

#ifdef HAVE_X86
__attribute__((target("aes,ssse3")))
static void aesni_ctr(const uint8_t *rk, uint64_t hi, uint64_t lo, uint64_t idx, uint8_t *buf, size_t nb)
{
    __m128i k[11];
    const __m128i rev = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    for (int i = 0; i < 11; i++) k[i] = _mm_loadu_si128((const __m128i *)(rk + 16 * i));
    uint64_t l = lo + idx, h = hi + (l < lo);
    size_t i = 0;
    for (; i + 8 <= nb; i += 8) {
        __m128i b[8];
        for (int j = 0; j < 8; j++) {
            uint64_t lj = l + (uint64_t)j, hj = h + (lj < l);
            b[j] = _mm_xor_si128(_mm_shuffle_epi8(_mm_set_epi64x((long long)hj, (long long)lj), rev), k[0]);
        }
        for (int r = 1; r < 10; r++)
            for (int j = 0; j < 8; j++) b[j] = _mm_aesenc_si128(b[j], k[r]);
        for (int j = 0; j < 8; j++) {
            __m128i *d = (__m128i *)(buf + 16 * (i + (size_t)j));
            _mm_storeu_si128(d, _mm_xor_si128(_mm_loadu_si128(d), _mm_aesenclast_si128(b[j], k[10])));
        }
        uint64_t nl = l + 8;
        h += (nl < l); l = nl;
    }
    for (; i < nb; i++) {
        __m128i b = _mm_xor_si128(_mm_shuffle_epi8(_mm_set_epi64x((long long)h, (long long)l), rev), k[0]);
        for (int r = 1; r < 10; r++) b = _mm_aesenc_si128(b, k[r]);
        __m128i *d = (__m128i *)(buf + 16 * i);
        _mm_storeu_si128(d, _mm_xor_si128(_mm_loadu_si128(d), _mm_aesenclast_si128(b, k[10])));
        if (++l == 0) h++;
    }
}
#endif

/* ------------------------------------------------------------------ */
/* PKG stream cipher (seekable)                                        */
/* ------------------------------------------------------------------ */
static const uint8_t KEY_PS3[16] = { 0x2E, 0x7B, 0x71, 0xD7, 0xC9, 0xC9, 0xA1, 0x4E,
                                     0xA3, 0x22, 0x1F, 0x18, 0x88, 0x28, 0xB8, 0xF8 };
static const uint8_t KEY_PSP[16] = { 0x07, 0xF2, 0xC6, 0x82, 0x90, 0xB5, 0x0D, 0x2C,
                                     0x33, 0x81, 0x8D, 0x70, 0x9B, 0x60, 0xE6, 0x2B };

typedef struct {
    int aes;              /* 0 = debug (SHA-1 keystream), 1 = retail (AES-128-CTR) */
    uint8_t dkey[56];     /* debug: digest-derived key block (counter appended) */
    uint8_t rk[176];      /* retail: AES round keys */
    uint64_t ivhi, ivlo;  /* retail: IV */
} Crypt;

static void crypt_debug(Crypt *c, const uint8_t digest[16])
{
    memset(c, 0, sizeof *c);
    memcpy(c->dkey, digest, 8);
    memcpy(c->dkey + 8, digest, 8);
    memcpy(c->dkey + 16, digest + 8, 8);
    memcpy(c->dkey + 24, digest + 8, 8);
}
static void crypt_retail(Crypt *c, const uint8_t key[16], const uint8_t iv[16])
{
    memset(c, 0, sizeof *c);
    c->aes = 1;
    aes_expand(key, c->rk);
    c->ivhi = rd64(iv);
    c->ivlo = rd64(iv + 8);
}
static void ks_block(const Crypt *c, uint64_t idx, uint8_t out[16])
{
    if (c->aes) {
        uint8_t ctr[16];
        uint64_t l = c->ivlo + idx, h = c->ivhi + (l < c->ivlo);
        wr64(ctr, h); wr64(ctr + 8, l);
        aes_enc(c->rk, ctr, out);
    } else {
        static const uint8_t pad[64] = { 0x80, [62] = 0x02 };
        uint8_t blk[64];
        uint32_t h[5];
        memcpy(blk, c->dkey, 56);
        wr64(blk + 56, idx);
        memcpy(h, SHA1_IV, sizeof h);
        sha1_compress(h, blk);
        sha1_compress(h, pad);
        for (int i = 0; i < 4; i++) wr32(out + 4 * i, h[i]);
    }
}
/* XOR keystream for absolute data offset `off` into buf */
static void crypt_run(const Crypt *c, uint64_t off, uint8_t *buf, size_t len)
{
    uint64_t idx = off / 16;
    unsigned sk = (unsigned)(off % 16);
    uint8_t ks[16];
    if (sk && len) {
        size_t n = 16 - sk;
        if (n > len) n = len;
        ks_block(c, idx, ks);
        for (size_t i = 0; i < n; i++) buf[i] ^= ks[sk + i];
        buf += n; len -= n; idx++;
    }
    size_t nb = len / 16;
    if (nb) {
#ifdef HAVE_X86
        if (c->aes && use_aesni) aesni_ctr(c->rk, c->ivhi, c->ivlo, idx, buf, nb);
        else
#endif
            for (size_t i = 0; i < nb; i++) {
                ks_block(c, idx + i, ks);
                for (int j = 0; j < 16; j++) buf[16 * i + (size_t)j] ^= ks[j];
            }
        buf += nb * 16; len -= nb * 16; idx += nb;
    }
    if (len) {
        ks_block(c, idx, ks);
        for (size_t i = 0; i < len; i++) buf[i] ^= ks[i];
    }
}


/* multithreaded wrapper: the keystream is seekable, so big buffers are split across cores */
typedef struct { const Crypt *c; uint64_t off; uint8_t *buf; size_t len; } Job;
#ifdef _WIN32
static DWORD WINAPI job_run(LPVOID a) { Job *j = a; crypt_run(j->c, j->off, j->buf, j->len); return 0; }
static int ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
static void *job_run(void *a) { Job *j = a; crypt_run(j->c, j->off, j->buf, j->len); return NULL; }
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
static int g_threads = 0;
static void crypt_run_mt(const Crypt *c, uint64_t off, uint8_t *buf, size_t len)
{
    if (!g_threads) { g_threads = ncpu(); if (g_threads > 32) g_threads = 32; if (g_threads < 1) g_threads = 1; }
    int n = g_threads;
    if (n < 2 || len < (1u << 20)) { crypt_run(c, off, buf, len); return; }
    Job jobs[32];
    size_t chunk = (len / (size_t)n + 15) & ~(size_t)15;
    int used = 0;
    for (size_t pos = 0; pos < len; pos += chunk, used++) {
        jobs[used].c = c; jobs[used].off = off + pos; jobs[used].buf = buf + pos;
        jobs[used].len = (len - pos < chunk) ? len - pos : chunk;
    }
#ifdef _WIN32
    HANDLE h[32];
    for (int i = 1; i < used; i++) h[i] = CreateThread(NULL, 0, job_run, &jobs[i], 0, NULL);
    job_run(&jobs[0]);
    for (int i = 1; i < used; i++) {
        if (h[i]) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
        else job_run(&jobs[i]);
    }
#else
    pthread_t t[32]; int ok[32];
    for (int i = 1; i < used; i++) ok[i] = !pthread_create(&t[i], NULL, job_run, &jobs[i]);
    job_run(&jobs[0]);
    for (int i = 1; i < used; i++) { if (ok[i]) pthread_join(t[i], NULL); else job_run(&jobs[i]); }
#endif
}

/* ------------------------------------------------------------------ */
/* PKG reading                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    char *name;
    uint64_t off, size;
    uint32_t flags;
    int is_dir, psp;
} Entry;

typedef struct {
    FILE *f;
    uint64_t fsize;
    uint16_t rev, type;
    uint32_t meta_off, meta_cnt, hdr_size, nitems;
    uint64_t total, data_off, data_size;
    char cid[0x31];
    uint8_t digest[16], riv[16];
    int retail;
    Crypt cr, cr_psp;
    Entry *e;
} Pkg;

static void xread(FILE *f, uint64_t off, void *buf, size_t len)
{
    if (fseek64(f, (int64_t)off, SEEK_SET) || fread(buf, 1, len, f) != len)
        die("read failed at offset 0x%llx (file truncated?)", (unsigned long long)off);
}

static void pkg_open(Pkg *p, const char *path)
{
    uint8_t h[0x80];
    memset(p, 0, sizeof *p);
    p->f = xfopen(path, "rb");
    if (!p->f) die("cannot open '%s': %s", path, strerror(errno));
    setvbuf(p->f, NULL, _IONBF, 0);
    p->fsize = file_size(p->f);
    if (p->fsize < 0x80) die("'%s' is too small to be a PKG", path);
    xread(p->f, 0, h, sizeof h);
    if (rd32(h) != 0x7F504B47) die("'%s' is not a PKG (bad magic)", path);
    p->rev = rd16(h + 4); p->type = rd16(h + 6);
    p->meta_off = rd32(h + 8); p->meta_cnt = rd32(h + 0xC); p->hdr_size = rd32(h + 0x10);
    p->nitems = rd32(h + 0x14);
    p->total = rd64(h + 0x18); p->data_off = rd64(h + 0x20); p->data_size = rd64(h + 0x28);
    memcpy(p->cid, h + 0x30, 0x30);
    memcpy(p->digest, h + 0x60, 16);
    memcpy(p->riv, h + 0x70, 16);
    p->retail = (p->rev & 0x8000) != 0;
    if (p->retail) {
        crypt_retail(&p->cr, p->type == 2 ? KEY_PSP : KEY_PS3, p->riv);
        crypt_retail(&p->cr_psp, KEY_PSP, p->riv);
    } else {
        crypt_debug(&p->cr, p->digest);
        p->cr_psp = p->cr;
    }
}

static int sanitize(char *n)
{
    char *s = n, *d = n;
    while (*s == '/' || *s == '\\') s++;
    for (; *s; s++) {
        char c = *s;
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (strchr(":*?\"<>|", c)) c = '_';
#endif
        if ((unsigned char)c < 0x20) c = '_';
        *d++ = c;
    }
    *d = 0;
    /* drop "", "." and ".." components: custom installer pkgs use ../../dev_hdd0/... names
       to write outside the game dir; on PC we keep everything inside the output folder */
    d = n;
    for (s = n; *s;) {
        char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        int skip = l == 0 || (l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.');
        if (!skip) {
            if (d != n) *d++ = '/';
            memmove(d, s, l);
            d += l;
        }
        if (!e) break;
        s = e + 1;
    }
    *d = 0;
    return *n ? 0 : -1;
}

static void pkg_read_table(Pkg *p)
{
    if (p->data_off + p->data_size > p->fsize)
        die("file is shorter than the header says (%llu < %llu bytes).\n"
            "       Is this one part of a split package? Run 'join' first.",
            (unsigned long long)p->fsize, (unsigned long long)(p->data_off + p->data_size));
    if ((uint64_t)p->nitems * 0x20 > p->data_size || p->nitems > 4000000)
        die("bogus item count (%u)", p->nitems);
    size_t tsz = (size_t)p->nitems * 0x20;
    uint8_t *t = xmalloc(tsz);
    xread(p->f, p->data_off, t, tsz);
    crypt_run(&p->cr, 0, t, tsz);
    p->e = xmalloc(sizeof(Entry) * p->nitems);
    for (uint32_t i = 0; i < p->nitems; i++) {
        const uint8_t *r = t + (size_t)i * 0x20;
        Entry *e = &p->e[i];
        uint32_t noff = rd32(r), nlen = rd32(r + 4);
        e->off = rd64(r + 8); e->size = rd64(r + 0x10); e->flags = rd32(r + 0x18);
        e->psp = (e->flags & 0x10000000) != 0;
        uint32_t ty = e->flags & 0xFF;
        e->is_dir = (ty == 0x04) || (ty == 0x12 && e->size == 0);
        if (nlen == 0 || nlen > 4096 || (uint64_t)noff + nlen > p->data_size ||
            e->off > p->data_size || e->size > p->data_size - e->off)
            die("file table is garbage at entry %u (corrupt package or unsupported encryption)", i);
        e->name = xmalloc(nlen + 1);
        xread(p->f, p->data_off + noff, e->name, nlen);
        crypt_run(e->psp ? &p->cr_psp : &p->cr, noff, (uint8_t *)e->name, nlen);
        e->name[nlen] = 0;
    }
    free(t);
}

static const char *content_type_name(uint32_t t)
{
    switch (t) {
    case 0x4: return "GameData"; case 0x5: return "GameExec"; case 0x6: return "PS1emu";
    case 0x7: return "PSP/PCEngine"; case 0x9: return "Theme"; case 0xA: return "Widget";
    case 0xB: return "License"; case 0xC: return "VSH module"; case 0xD: return "PSN avatar";
    case 0xE: return "PSPgo"; case 0xF: return "Minis"; case 0x10: return "NeoGeo";
    case 0x11: return "VMC"; case 0x12: return "PS2 classic"; case 0x14: return "PSP remaster";
    default: return "?";
    }
}

static int cmd_info(const char *path)
{
    Pkg p;
    pkg_open(&p, path);
    printf("File         : %s (%llu bytes)\n", path, (unsigned long long)p.fsize);
    printf("Kind         : %s, %s\n", p.retail ? "finalized (retail, AES-128-CTR)" : "debug (SHA-1 keystream)",
           p.type == 1 ? "PS3" : p.type == 2 ? "PSP/Vita" : "unknown platform");
    printf("Content ID   : %s\n", p.cid);
    printf("Items        : %u\n", p.nitems);
    printf("Total size   : %llu\n", (unsigned long long)p.total);
    printf("Data         : offset 0x%llx, size %llu\n", (unsigned long long)p.data_off,
           (unsigned long long)p.data_size);
    printf("Digest       : ");
    for (int i = 0; i < 16; i++) printf("%02X", p.digest[i]);
    printf("\nData RIV     : ");
    for (int i = 0; i < 16; i++) printf("%02X", p.riv[i]);
    printf("\n");
    if (p.total != p.fsize)
        printf("WARNING      : header size and real size differ (truncated or split part?)\n");
    uint64_t off = p.meta_off;
    for (uint32_t i = 0; i < p.meta_cnt && i < 64 && off + 8 <= p.fsize; i++) {
        uint8_t h[8], d[64];
        xread(p.f, off, h, 8);
        uint32_t id = rd32(h), sz = rd32(h + 4);
        if (sz > p.fsize - off - 8) break;
        size_t n = sz > sizeof d ? sizeof d : sz;
        xread(p.f, off + 8, d, n);
        printf("Metadata %02X  : ", id);
        for (size_t j = 0; j < n; j++) printf("%02X", d[j]);
        if (id == 1 && sz >= 4) {
            uint32_t v = rd32(d);
            printf("  (DRM: %s)", v == 1 ? "network" : v == 2 ? "local" : v == 3 ? "free" : "?");
        }
        if (id == 2 && sz >= 4) printf("  (content type: %s)", content_type_name(rd32(d)));
        printf("\n");
        off += 8 + sz;
    }
    fclose(p.f);
    return 0;
}

static int cmd_list(const char *path)
{
    Pkg p;
    pkg_open(&p, path);
    pkg_read_table(&p);
    uint64_t tot = 0;
    for (uint32_t i = 0; i < p.nitems; i++) {
        Entry *e = &p.e[i];
        if (e->is_dir) printf("%08X %14s  %s/\n", e->flags, "<dir>", e->name);
        else printf("%08X %14llu  %s\n", e->flags, (unsigned long long)e->size, e->name);
        tot += e->size;
    }
    printf("%u items, %llu bytes\n", p.nitems, (unsigned long long)tot);
    fclose(p.f);
    return 0;
}

static int cmd_extract(const char *path, const char *outdir, int verbose)
{
    Pkg p;
    char *autodir = NULL;
    pkg_open(&p, path);
    pkg_read_table(&p);
    if (!outdir) {
        size_t l = strlen(path);
        autodir = xmalloc(l + 8);
        strcpy(autodir, path);
        if (l > 4 && (!strcmp(path + l - 4, ".pkg") || !strcmp(path + l - 4, ".PKG"))) autodir[l - 4] = 0;
        else strcat(autodir, ".out");
        outdir = autodir;
    }
    mkdirs(outdir, 1);
    uint64_t total = 0;
    uint32_t nfiles = 0;
    for (uint32_t i = 0; i < p.nitems; i++)
        if (!p.e[i].is_dir) { total += p.e[i].size; nfiles++; }
    printf("Extracting %s -> %s  (%u files, %.1f MB, %s%s)\n", p.cid, outdir, nfiles, total / 1048576.0,
           p.retail ? "AES" : "debug", (p.retail && use_aesni) ? "-NI" : "");
    uint8_t *buf = xmalloc(BUFSZ);
    progress_start(total);
    for (uint32_t i = 0; i < p.nitems; i++) {
        Entry *e = &p.e[i];
        if (sanitize(e->name)) continue; /* entry that only points at a parent dir */
        char *out = pathcat(outdir, e->name);
        if (verbose) fprintf(stderr, "\r%-70s\n", e->name);
        if (e->is_dir) { mkdirs(out, 1); free(out); continue; }
        mkdirs(out, 0);
        FILE *o = xfopen(out, "wb");
        if (!o) die("cannot create '%s': %s", out, strerror(errno));
        setvbuf(o, NULL, _IONBF, 0);
        const Crypt *c = e->psp ? &p.cr_psp : &p.cr;
        if (fseek64(p.f, (int64_t)(p.data_off + e->off), SEEK_SET)) die("seek failed");
        for (uint64_t done = 0; done < e->size;) {
            size_t n = (e->size - done > BUFSZ) ? BUFSZ : (size_t)(e->size - done);
            if (fread(buf, 1, n, p.f) != n) die("read failed in '%s'", e->name);
            crypt_run_mt(c, e->off + done, buf, n);
            if (fwrite(buf, 1, n, o) != n) die("write failed on '%s' (disk full?)", out);
            done += n; g_done += n;
            progress(0);
        }
        if (fclose(o)) die("write failed on '%s'", out);
        free(out);
    }
    progress(1);
    printf("Done in %.2f s\n", now() - g_t0);
    fclose(p.f);
    free(buf); free(autodir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PKG creation (debug-style, PSL1GHT layout)                          */
/* ------------------------------------------------------------------ */
typedef struct {
    char *rel, *full;
    int is_dir;
    uint64_t size, name_off, data_off;
} Item;
static Item *items;
static size_t nitems, capitems;

static void add_item(char *rel, char *full, int is_dir, uint64_t size)
{
    if (nitems == capitems) {
        capitems = capitems ? capitems * 2 : 256;
        items = realloc(items, capitems * sizeof *items);
        if (!items) die("out of memory");
    }
    Item *it = &items[nitems++];
    it->rel = rel; it->full = full; it->is_dir = is_dir; it->size = size;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(const char *full, const char *rel)
{
    char **names = NULL;
    size_t n = 0, cap = 0;
#define PUSH(s) do { if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, cap * sizeof *names); \
                     if (!names) die("out of memory"); } names[n++] = (s); } while (0)
#ifdef _WIN32
    char *pat = pathcat(full, "*");
    wchar_t *w = widen(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) die("cannot read directory '%s'", full);
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        PUSH(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(w); free(pat);
#else
    DIR *d = opendir(full);
    struct dirent *de;
    if (!d) die("cannot read directory '%s'", full);
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *s = xmalloc(strlen(de->d_name) + 1);
        strcpy(s, de->d_name);
        PUSH(s);
    }
    closedir(d);
#endif
#undef PUSH
    if (n) qsort(names, n, sizeof *names, cmpstr);
    for (size_t i = 0; i < n; i++) {
        char *cf = pathcat(full, names[i]);
        char *cr;
        if (rel) cr = pathcat(rel, names[i]);
        else { cr = xmalloc(strlen(names[i]) + 1); strcpy(cr, names[i]); }
        int is_dir; uint64_t size;
        if (path_stat(cf, &is_dir, &size)) die("cannot stat '%s'", cf);
        add_item(cr, cf, is_dir, is_dir ? 0 : size);
        if (is_dir) walk(cf, cr);
        free(names[i]);
    }
    free(names);
}

/* pull TITLE_ID out of PARAM.SFO to build a default content id */
static int sfo_title_id(const char *dir, char out[10])
{
    char *path = pathcat(dir, "PARAM.SFO");
    FILE *f = xfopen(path, "rb");
    free(path);
    if (!f) return -1;
    uint8_t b[8192];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
#define LE32(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24)
    if (n < 0x14 || memcmp(b, "\0PSF", 4)) return -1;
    uint32_t kt = LE32(b + 8), dt = LE32(b + 12), cnt = LE32(b + 16);
    for (uint32_t i = 0; i < cnt && 0x14 + (i + 1) * 16 <= n; i++) {
        const uint8_t *e = b + 0x14 + i * 16;
        uint32_t ko = kt + (uint32_t)(e[0] | e[1] << 8), dof = dt + LE32(e + 12);
        if (ko + 9 <= n && dof + 9 <= n && !memcmp(b + ko, "TITLE_ID", 9)) {
            memcpy(out, b + dof, 9);
            out[9] = 0;
            return strlen(out) == 9 ? 0 : -1;
        }
    }
#undef LE32
    return -1;
}

typedef struct { int pass; Sha1 sha; Crypt *cr; FILE *out; uint64_t off; } Sink;
static void sink_put(Sink *s, uint8_t *buf, size_t len)
{
    if (s->pass == 1) sha1_update(&s->sha, buf, len);
    else {
        crypt_run_mt(s->cr, s->off, buf, len);
        if (fwrite(buf, 1, len, s->out) != len) die("write failed (disk full?)");
    }
    s->off += len;
    g_done += len;
    progress(0);
}
static void emit_files(Sink *s, uint8_t *buf)
{
    for (size_t i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (it->is_dir) continue;
        FILE *f = xfopen(it->full, "rb");
        if (!f) die("cannot open '%s': %s", it->full, strerror(errno));
        setvbuf(f, NULL, _IONBF, 0);
        uint64_t left = it->size;
        do {
            size_t n = left > BUFSZ ? BUFSZ : (size_t)left;
            if (n && fread(buf, 1, n, f) != n) die("'%s' changed size while packing", it->full);
            left -= n;
            if (!left) { /* pad the tail to 16 bytes */
                size_t pad = (size_t)(ALIGN16(it->size) - it->size);
                memset(buf + n, 0, pad);
                n += pad;
            }
            if (n) sink_put(s, buf, n);
        } while (left);
        fclose(f);
    }
}

static int build_pkg(const char *outpath, const char *cid, uint32_t ctype);
static int cmd_create(const char *dir, const char *outpath, const char *cid_arg, uint32_t ctype, const char *root_arg,
                      uint64_t part)
{
    char cid[0x30] = { 0 };
    int is_dir; uint64_t sz;
    if (path_stat(dir, &is_dir, &sz) || !is_dir) die("'%s' is not a directory", dir);
    if (cid_arg) {
        if (strlen(cid_arg) != 36) die("content id must be 36 chars (XX0000-TITLEID00_00-XXXXXXXXXXXXXXXX), got %u",
                                       (unsigned)strlen(cid_arg));
        strcpy(cid, cid_arg);
    } else if (root_arg) {
        strcpy(cid, "CUSTOM-INSTALLER_00-0000000000000000");
    } else {
        char tid[10];
        if (sfo_title_id(dir, tid)) die("no -c CONTENTID given and no TITLE_ID found in PARAM.SFO");
        snprintf(cid, sizeof cid, "UP0001-%s_00-0000000000000000", tid);
        printf("Content ID not given, using %s\n", cid);
    }
    /* custom install root: every name becomes ../../../<root>/<name>, which climbs out of
       /dev_hdd0/game/<id>/ to / and back down (same trick "custom installer" pkgs use) */
    char *prefix = NULL;
    size_t nprefix = 0;
    if (root_arg) {
        char *r = xmalloc(strlen(root_arg) + 1), *d = r;
        for (const char *q = root_arg; *q; q++) {
            char c = (*q == '\\') ? '/' : *q;
            if (c == '/' && (d == r || d[-1] == '/')) continue;
            *d++ = c;
        }
        while (d > r && d[-1] == '/') d--;
        *d = 0;
        if (strchr(r, ':'))
            die("bad install root '%s': that looks like a PC path.\n"
                "       Git Bash/MSYS rewrites arguments starting with '/', so write it without the\n"
                "       leading slash there: -r dev_hdd0/GAMES/MyGame", root_arg);
        if (!*r || strstr(r, "..")) die("bad install root '%s' (use e.g. /dev_hdd0/GAMES/MyGame)", root_arg);
        prefix = xmalloc(strlen(r) + 16);
        sprintf(prefix, "../../../%s", r);
        /* directory entries for each level below the device (dev_hdd0 itself already exists) */
        for (char *q = strchr(prefix + 9, '/'); q; ) {
            char *nx = strchr(q + 1, '/');
            size_t l = nx ? (size_t)(nx - prefix) : strlen(prefix);
            char *dn = xmalloc(l + 1);
            memcpy(dn, prefix, l); dn[l] = 0;
            add_item(dn, NULL, 1, 0);
            q = nx;
        }
        nprefix = nitems;
        free(r);
    }
    walk(dir, NULL);
    if (nitems == nprefix) die("'%s' is empty", dir);
    for (size_t i = nprefix; prefix && i < nitems; i++) {
        char *nr = pathcat(prefix, items[i].rel);
        free(items[i].rel);
        items[i].rel = nr;
    }
    if (prefix) printf("Install root: /%s (content id %s)\n", prefix + 9, cid);
    if (nitems > 4000000) die("too many files");
    if (!part) return build_pkg(outpath, cid, ctype);

    /* multi-pkg mode: every part is a standalone pkg holding all the directory entries plus
       a slice of the files; installing them in any order rebuilds the whole tree */
    Item *all = items;
    size_t nall = nitems, ndirs = 0;
    uint64_t base = 0x1A0;
    for (size_t i = 0; i < nall; i++)
        if (all[i].is_dir) { ndirs++; base += 0x20 + ALIGN16(strlen(all[i].rel)); }
    if (base >= part) die("part size too small");
    int toobig = 0;
    for (size_t i = 0; i < nall; i++) {
        if (all[i].is_dir) continue;
        if (base + 0x20 + ALIGN16(strlen(all[i].rel)) + ALIGN16(all[i].size) > part) {
            if (!toobig++) fprintf(stderr, "\nThese files do not fit in a %llu-byte pkg on their own:\n",
                                   (unsigned long long)part);
            fprintf(stderr, "  %12.1f MB  %s\n", all[i].size / 1048576.0, all[i].full);
        }
    }
    if (toobig)
        die("'create -s' is not compatible with this folder: a pkg cannot hold part of a file,\n"
            "       so %d file(s) would make a pkg bigger than the limit (FAT32 tops out at 4 GB).\n"
            "       Nothing was written. Use the raw split instead:\n"
            "         1) ps3pkgtool create <dir> <out.pkg> [same options, without -s]\n"
            "         2) ps3pkgtool split  <out.pkg>          -> out.pkg.66600, .66601, ...\n"
            "       Those pieces are not installable by themselves: they must be joined back into\n"
            "       one pkg on the PS3 (by a separate homebrew) or on PC with 'ps3pkgtool join'.", toobig);
    int *partof = xmalloc(sizeof(int) * nall);
    int nparts = 0;
    uint64_t cur = 0;
    for (size_t i = 0; i < nall; i++) {
        if (all[i].is_dir) continue;
        uint64_t cost = 0x20 + ALIGN16(strlen(all[i].rel)) + ALIGN16(all[i].size);
        if (!nparts || (cur > base && cur + cost > part)) { nparts++; cur = base; }
        cur += cost;
        partof[i] = nparts;
    }
    if (nparts == 0) nparts = 1;
    size_t ol = strlen(outpath);
    int has_ext = ol > 4 && (!strcmp(outpath + ol - 4, ".pkg") || !strcmp(outpath + ol - 4, ".PKG"));
    char *name = xmalloc(ol + 32);
    Item *sub = xmalloc(sizeof(Item) * nall);
    printf("Splitting into %d pkgs of up to %llu bytes\n", nparts, (unsigned long long)part);
    for (int p = 1; p <= nparts; p++) {
        size_t n = 0;
        for (size_t i = 0; i < nall; i++)
            if (all[i].is_dir || partof[i] == p) sub[n++] = all[i];
        sprintf(name, "%.*s_%dp.pkg", (int)(has_ext ? ol - 4 : ol), outpath, p);
        printf("[%d/%d] ", p, nparts);
        items = sub; nitems = n;
        build_pkg(name, cid, ctype);
    }
    items = all; nitems = nall;
    return 0;
}

static int build_pkg(const char *outpath, const char *cid, uint32_t ctype)
{

    /* layout: [table][names][file data], everything 16-byte aligned */
    uint64_t off = (uint64_t)nitems * 0x20;
    for (size_t i = 0; i < nitems; i++) { items[i].name_off = off; off += ALIGN16(strlen(items[i].rel)); }
    uint64_t meta_len = off;
    for (size_t i = 0; i < nitems; i++) { items[i].data_off = off; off += ALIGN16(items[i].size); }
    uint64_t data_size = off;
    if (meta_len > 0x7FFFFFFF) die("file table too large");

    uint8_t *meta = xmalloc((size_t)meta_len), *metawork = xmalloc((size_t)meta_len);
    memset(meta, 0, (size_t)meta_len);
    for (size_t i = 0; i < nitems; i++) {
        uint8_t *r = meta + i * 0x20;
        Item *it = &items[i];
        wr32(r, (uint32_t)it->name_off); wr32(r + 4, (uint32_t)strlen(it->rel));
        wr64(r + 8, it->data_off); wr64(r + 0x10, it->size);
        wr32(r + 0x18, 0x80000000u | (it->is_dir ? 0x04 : 0x03));
        memcpy(meta + it->name_off, it->rel, strlen(it->rel));
    }

    uint8_t *buf = xmalloc(BUFSZ + 16);
    Sink s;
    printf("Packing %u items, %.1f MB\n", (unsigned)nitems, data_size / 1048576.0);

    /* pass 1: QA digest = SHA1(plaintext data)[3..19] */
    progress_start(data_size * 2);
    memset(&s, 0, sizeof s);
    s.pass = 1;
    sha1_init(&s.sha);
    memcpy(metawork, meta, (size_t)meta_len);
    sink_put(&s, metawork, (size_t)meta_len);
    emit_files(&s, buf);
    uint8_t dg[20], qa[16];
    sha1_final(&s.sha, dg);
    memcpy(qa, dg + 3, 16);

    Crypt cr, tmp;
    crypt_debug(&cr, qa);

    /* header */
    uint8_t hdr[0x80] = { 0 }, md[0x40] = { 0 };
    wr32(hdr, 0x7F504B47); wr32(hdr + 4, 0x00000001);
    wr32(hdr + 8, 0xC0); wr32(hdr + 0xC, 5); wr32(hdr + 0x10, 0x80);
    wr32(hdr + 0x14, (uint32_t)nitems);
    wr64(hdr + 0x18, 0x140 + data_size + 0x60); wr64(hdr + 0x20, 0x140); wr64(hdr + 0x28, data_size);
    memcpy(hdr + 0x30, cid, 0x30);
    memcpy(hdr + 0x60, qa, 16);
    ks_block(&cr, ~0ULL, hdr + 0x70);
    /* metadata TLVs */
    wr32(md + 0x00, 1); wr32(md + 0x04, 4); wr32(md + 0x08, 3);      /* DRM: free */
    wr32(md + 0x0C, 2); wr32(md + 0x10, 4); wr32(md + 0x14, ctype);  /* content type */
    wr32(md + 0x18, 3); wr32(md + 0x1C, 4); wr32(md + 0x20, 0x4E);   /* package flags */
    wr32(md + 0x24, 4); wr32(md + 0x28, 8); wr64(md + 0x2C, data_size);
    wr32(md + 0x34, 5); wr32(md + 0x38, 4); wr32(md + 0x3C, 0x10610000);

    uint8_t hsha[16], msha[16], pad1[0x30] = { 0 }, pad2[0x30];
    sha1(hdr, sizeof hdr, dg); memcpy(hsha, dg + 3, 16);
    sha1(md, sizeof md, dg);   memcpy(msha, dg + 3, 16);
    crypt_debug(&tmp, msha); crypt_run(&tmp, 0, pad1, sizeof pad1);
    memcpy(pad2, pad1, sizeof pad2);
    crypt_debug(&tmp, hsha); crypt_run(&tmp, 0, pad2, sizeof pad2);

    FILE *o = xfopen(outpath, "wb");
    if (!o) die("cannot create '%s': %s", outpath, strerror(errno));
    setvbuf(o, NULL, _IONBF, 0);
    uint8_t head[0x140];
    memcpy(head, hdr, 0x80); memcpy(head + 0x80, hsha, 16); memcpy(head + 0x90, pad2, 0x30);
    memcpy(head + 0xC0, md, 0x40); memcpy(head + 0x100, msha, 16); memcpy(head + 0x110, pad1, 0x30);
    if (fwrite(head, 1, sizeof head, o) != sizeof head) die("write failed");

    /* pass 2: encrypt + write */
    s.pass = 2; s.cr = &cr; s.out = o; s.off = 0;
    sink_put(&s, meta, (size_t)meta_len);
    emit_files(&s, buf);
    if (s.off != data_size) die("internal error: size mismatch");
    uint8_t tail[0x60] = { 0 };
    if (fwrite(tail, 1, sizeof tail, o) != sizeof tail || fclose(o)) die("write failed");
    progress(1);
    printf("Created %s (%llu bytes) in %.2f s\n", outpath, (unsigned long long)(0x140 + data_size + 0x60),
           now() - g_t0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* split / join (.666NN parts, the multiMAN/webMAN convention)         */
/* ------------------------------------------------------------------ */
static uint64_t parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    uint64_t m = 1;
    if (*end == 'k' || *end == 'K') m = 1024ULL;
    else if (*end == 'm' || *end == 'M') m = 1024ULL * 1024;
    else if (*end == 'g' || *end == 'G') m = 1024ULL * 1024 * 1024;
    else if (*end) die("bad size '%s' (use e.g. 4095M, 2G, 700000000)", s);
    if (v <= 0) die("bad size '%s'", s);
    return (uint64_t)(v * (double)m);
}

static void copy_bytes(FILE *in, FILE *out, uint64_t len, uint8_t *buf)
{
    while (len) {
        size_t n = len > BUFSZ ? BUFSZ : (size_t)len;
        if (fread(buf, 1, n, in) != n) die("read failed");
        if (fwrite(buf, 1, n, out) != n) die("write failed (disk full?)");
        len -= n; g_done += n;
        progress(0);
    }
}

static int cmd_split(const char *path, uint64_t part)
{
    FILE *in = xfopen(path, "rb");
    if (!in) die("cannot open '%s': %s", path, strerror(errno));
    setvbuf(in, NULL, _IONBF, 0);
    uint64_t size = file_size(in);
    if (size <= part) { printf("File already fits in one part (%llu bytes), nothing to do.\n", (unsigned long long)size); return 0; }
    uint64_t nparts = (size + part - 1) / part;
    if (nparts > 100) die("that would be %llu parts; max is 100 (.66600-.66699)", (unsigned long long)nparts);
    uint8_t *buf = xmalloc(BUFSZ);
    char *name = xmalloc(strlen(path) + 8);
    printf("Splitting into %llu parts of up to %llu bytes\n", (unsigned long long)nparts, (unsigned long long)part);
    progress_start(size);
    for (uint64_t i = 0; i < nparts; i++) {
        sprintf(name, "%s.666%02u", path, (unsigned)i);
        FILE *o = xfopen(name, "wb");
        if (!o) die("cannot create '%s': %s", name, strerror(errno));
        setvbuf(o, NULL, _IONBF, 0);
        uint64_t left = size - i * part;
        copy_bytes(in, o, left > part ? part : left, buf);
        if (fclose(o)) die("write failed on '%s'", name);
    }
    progress(1);
    printf("Done: %s.66600 .. .666%02u\n", path, (unsigned)(nparts - 1));
    fclose(in);
    return 0;
}

static int cmd_join(const char *path, const char *outpath, int force)
{
    size_t l = strlen(path);
    char *base = xmalloc(l + 1);
    strcpy(base, path);
    if (l > 6 && !strncmp(base + l - 6, ".666", 4) && base[l - 2] >= '0' && base[l - 2] <= '9' &&
        base[l - 1] >= '0' && base[l - 1] <= '9')
        base[l - 6] = 0;
    if (!outpath) outpath = base;
    char *name = xmalloc(strlen(base) + 8);
    uint64_t total = 0, sz;
    int n = 0, is_dir;
    for (; n < 100; n++) {
        sprintf(name, "%s.666%02d", base, n);
        if (path_stat(name, &is_dir, &sz) || is_dir) break;
        total += sz;
    }
    if (!n) die("no parts found (looked for '%s.66600')", base);
    if (!force && !path_stat(outpath, &is_dir, &sz)) die("'%s' already exists (use -f to overwrite)", outpath);
    FILE *o = xfopen(outpath, "wb");
    if (!o) die("cannot create '%s': %s", outpath, strerror(errno));
    setvbuf(o, NULL, _IONBF, 0);
    uint8_t *buf = xmalloc(BUFSZ);
    printf("Joining %d parts -> %s (%.1f MB)\n", n, outpath, total / 1048576.0);
    progress_start(total);
    for (int i = 0; i < n; i++) {
        sprintf(name, "%s.666%02d", base, i);
        FILE *in = xfopen(name, "rb");
        if (!in) die("cannot open '%s'", name);
        setvbuf(in, NULL, _IONBF, 0);
        copy_bytes(in, o, file_size(in), buf);
        fclose(in);
    }
    if (fclose(o)) die("write failed");
    progress(1);
    /* sanity check if the result is a PKG */
    FILE *chk = xfopen(outpath, "rb");
    uint8_t h[0x20];
    if (chk && fread(h, 1, sizeof h, chk) == sizeof h && rd32(h) == 0x7F504B47) {
        if (rd64(h + 0x18) == total) printf("PKG size matches its header, looks complete.\n");
        else printf("WARNING: PKG header says %llu bytes but joined file is %llu (missing part?)\n",
                    (unsigned long long)rd64(h + 0x18), (unsigned long long)total);
    }
    if (chk) fclose(chk);
    return 0;
}

/* ------------------------------------------------------------------ */
static int cmd_selftest(void)
{
    int bad = 0;
    uint8_t d[20];
    static const uint8_t abc[20] = { 0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E,
                                     0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0, 0xD8, 0x9D };
    sha1((const uint8_t *)"abc", 3, d);
    printf("SHA-1        : %s\n", memcmp(d, abc, 20) ? (bad = 1, "FAIL") : "ok");
    uint8_t key[16], pt[16], ct[16], rk[176];
    static const uint8_t exp[16] = { 0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                     0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A };
    for (int i = 0; i < 16; i++) { key[i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
    aes_expand(key, rk);
    aes_enc(rk, pt, ct);
    printf("AES-128      : %s\n", memcmp(ct, exp, 16) ? (bad = 1, "FAIL") : "ok");
    uint8_t iv[16], a[1000], b[1000];
    memset(iv, 0xFF, 16); iv[15] = 0xFA; /* force carry across the 64-bit boundary */
    Crypt c;
    crypt_retail(&c, key, iv);
    for (int i = 0; i < 1000; i++) a[i] = b[i] = (uint8_t)(i * 7);
    int ni = use_aesni;
    use_aesni = 0; crypt_run(&c, 5, a, sizeof a);
    use_aesni = ni; crypt_run(&c, 5, b, sizeof b);
    printf("AES-NI       : %s\n", !ni ? "not available (portable path in use)"
                                      : memcmp(a, b, sizeof a) ? (bad = 1, "FAIL") : "ok");
    return bad;
}

static void usage(void)
{
    printf(
"ps3pkgtool " VERSION " - fast PS3 PKG extractor / creator / splitter\n\n"
"  ps3pkgtool info    <file.pkg>                    show header + metadata\n"
"  ps3pkgtool list    <file.pkg>                    list contents\n"
"  ps3pkgtool extract <file.pkg> [outdir] [-v]      extract (retail or debug)\n"
"  ps3pkgtool create  <dir> <out.pkg> [-c CONTENTID] [-t TYPE] [-r ROOT] [-s SIZE]\n"
"                                                   build a debug-style pkg from a folder\n"
"                                                   (PARAM.SFO, ICON0.PNG, USRDIR/... at its root)\n"
"  ps3pkgtool split   <file> [-s SIZE]              raw split (like rar volumes) into file.66600...\n"
"                                                   (default 4294901760 = FAT32 safe)\n"
"  ps3pkgtool join    <file.66600> [out] [-f]       join the parts back\n"
"  ps3pkgtool selftest                              check the crypto on this CPU\n\n"
"  CONTENTID: 36 chars, e.g. UP0001-GR33N0000_00-0000000000000000\n"
"             (if omitted it is built from TITLE_ID in PARAM.SFO)\n"
"  ROOT     : custom install path on the PS3, e.g. /dev_hdd0/GAMES/MyGame\n"
"             (<dir> contents land there instead of /dev_hdd0/game/<TITLE_ID>)\n"
"  create -s: make several standalone pkgs (out_1p.pkg, out_2p.pkg...) of up to SIZE each,\n"
"             to be installed one after another, instead of a single big one\n"
"  TYPE     : content type, default 5 (GameExec); 4 = GameData, 9 = Theme...\n"
"  SIZE     : bytes, or with K/M/G suffix (e.g. 4095M)\n");
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        argv = xmalloc(sizeof(char *) * (size_t)(wargc + 1));
        for (int i = 0; i < wargc; i++) argv[i] = narrow(wargv[i]);
        argv[wargc] = NULL;
        argc = wargc;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    aes_init();
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];

    /* split positional args from options */
    const char *pos[4] = { 0 };
    int npos = 0, verbose = 0, force = 0, part_set = 0;
    const char *cid = NULL, *root = NULL;
    uint32_t ctype = 5;
    uint64_t part = 0xFFFF0000ULL;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-f")) force = 1;
        else if (!strcmp(a, "-c") && i + 1 < argc) cid = argv[++i];
        else if (!strcmp(a, "-r") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(a, "-t") && i + 1 < argc) ctype = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "-s") && i + 1 < argc) { part = parse_size(argv[++i]); part_set = 1; }
        else if (a[0] == '-' && a[1]) die("unknown option '%s'", a);
        else if (npos < 4) pos[npos++] = a;
    }

    if (!strcmp(cmd, "selftest")) return cmd_selftest();
    if (!strcmp(cmd, "info") && npos == 1) return cmd_info(pos[0]);
    if (!strcmp(cmd, "list") && npos == 1) return cmd_list(pos[0]);
    if ((!strcmp(cmd, "extract") || !strcmp(cmd, "x")) && npos >= 1) return cmd_extract(pos[0], pos[1], verbose);
    if (!strcmp(cmd, "create") && npos == 2) return cmd_create(pos[0], pos[1], cid, ctype, root, part_set ? part : 0);
    if (!strcmp(cmd, "split") && npos == 1) return cmd_split(pos[0], part);
    if (!strcmp(cmd, "join") && npos >= 1) return cmd_join(pos[0], pos[1], force);
    usage();
    return 1;
}
