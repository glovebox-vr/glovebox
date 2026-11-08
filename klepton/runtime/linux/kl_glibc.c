// The glibc personality. See kl_glibc.h.
//
// Three kinds of entry, in order of preference:
//   ALIAS  the glibc name is the Bionic name Klepton already serves with the
//          right Linux ABI (errno translation, AAPCS64 varargs, FILE wrapping):
//          __errno_location -> __errno, open64 -> open, __isoc23_sscanf -> sscanf
//   HOST   Darwin's function has the same C ABI and no Linux-visible difference
//   IMPL   glibc-specific behaviour or layout implemented here: ctype tables,
//          the locale struct libstdc++ reads directly, _dl_find_object for the
//          unwinder, the old __xstat ABI, fortified printf, and stubs for what a
//          sandboxed app cannot do (exec, mount, SysV shm).
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <iconv.h>
#include <inttypes.h>
#include <netdb.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <complex.h>
#include <fenv.h>
#include <nl_types.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <poll.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <zlib.h>
#include "kl_glibc.h"
#include "kl_elftls.h"
#include "kl_va.h"

static kl_image *g_cxx, *g_gcc;

void kl_glibc_set_cxx_images(kl_image *libstdcxx, kl_image *libgcc_s) {
    g_cxx = libstdcxx;
    g_gcc = libgcc_s;
}

int kl_glibc_serves_soname(const char *s) {
    static const char *served[] = {
        "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
        "ld-linux-aarch64.so.1", "libutil.so.1", "libz.so.1",
        "libX11.so.6", "libxcb.so.1", "libudev.so.1", "libdrm.so.2", "libuuid.so.1",
        "libmimalloc.so", "libSDL2.so", "libSDL2-2.0.so.0",
    };
    for (unsigned i = 0; i < sizeof served / sizeof served[0]; i++)
        if (!strcmp(s, served[i])) return 1;
    return 0;
}

static int is_cxx_abi(const char *n) {
    return !strncmp(n, "_Z", 2) || !strncmp(n, "__cxa_", 6) || !strncmp(n, "__gxx_", 6) ||
           !strncmp(n, "_Unwind_", 8) || !strcmp(n, "__dynamic_cast");
}

// Klepton's own answer for a (Bionic) name, bypassing this override.
static __thread int g_in_lookup;
static void *bionic(const char *name) {
    g_in_lookup = 1;
    void *p = kl_shim_lookup(name);
    g_in_lookup = 0;
    return p;
}

// ------------------------------------------------------------------ ctype
// glibc's tables are indexed -128..255 and read inline by the guest's
// isalpha()/toupper() macros, so they are data with glibc's exact bit layout.
enum {
    G_ISupper = 1 << 8, G_ISlower = 1 << 9, G_ISalpha = 1 << 10, G_ISdigit = 1 << 11,
    G_ISxdigit = 1 << 12, G_ISspace = 1 << 13, G_ISprint = 1 << 14, G_ISgraph = 1 << 15,
    G_ISblank = 1 << 0, G_IScntrl = 1 << 1, G_ISpunct = 1 << 2, G_ISalnum = 1 << 3,
};
static unsigned short g_ctype_b[384];
static int32_t g_ctype_lower[384], g_ctype_upper[384];
static const unsigned short *g_ctype_b_ptr = g_ctype_b + 128;
static const int32_t *g_ctype_lower_ptr = g_ctype_lower + 128;
static const int32_t *g_ctype_upper_ptr = g_ctype_upper + 128;

static void ctype_init(void) {
    for (int i = -128; i < 256; i++) {
        int c = i & 0xff;
        unsigned short m = 0;
        if (i >= 0 && i < 128) {
            if (isupper(c)) m |= G_ISupper;
            if (islower(c)) m |= G_ISlower;
            if (isalpha(c)) m |= G_ISalpha;
            if (isdigit(c)) m |= G_ISdigit;
            if (isxdigit(c)) m |= G_ISxdigit;
            if (isspace(c)) m |= G_ISspace;
            if (isprint(c)) m |= G_ISprint;
            if (isgraph(c)) m |= G_ISgraph;
            if (c == ' ' || c == '\t') m |= G_ISblank;
            if (iscntrl(c)) m |= G_IScntrl;
            if (ispunct(c)) m |= G_ISpunct;
            if (isalnum(c)) m |= G_ISalnum;
        }
        g_ctype_b[i + 128] = m;
        g_ctype_lower[i + 128] = (i >= 0 && i < 128) ? tolower(c) : i;
        g_ctype_upper[i + 128] = (i >= 0 && i < 128) ? toupper(c) : i;
    }
}
static const unsigned short **g___ctype_b_loc(void) { return &g_ctype_b_ptr; }
static const int32_t **g___ctype_tolower_loc(void) { return &g_ctype_lower_ptr; }
static const int32_t **g___ctype_toupper_loc(void) { return &g_ctype_upper_ptr; }

// ------------------------------------------------------------------ locale
// libstdc++'s "gnu" locale model reads struct __locale_struct directly
// (ctype_members.cc: _M_table = __cloc->__ctype_b, ...), so a locale_t has to
// BE that struct. Every locale here is the C locale.
struct g_locale_data { char pad[256]; };
struct g_locale {
    struct g_locale_data *locales[13];
    const unsigned short *ctype_b;
    const int32_t *ctype_tolower, *ctype_toupper;
    const char *names[13];
};
static struct g_locale_data g_locdata;
static struct g_locale g_c_locale;
static void locale_init(void) {
    for (int i = 0; i < 13; i++) { g_c_locale.locales[i] = &g_locdata; g_c_locale.names[i] = "C"; }
    g_c_locale.ctype_b = g_ctype_b_ptr;
    g_c_locale.ctype_tolower = g_ctype_lower_ptr;
    g_c_locale.ctype_toupper = g_ctype_upper_ptr;
}
static void *g___newlocale(int mask, const char *name, void *base) {
    (void)mask; (void)name;
    if (base && base != &g_c_locale) return base;
    struct g_locale *l = malloc(sizeof *l);
    *l = g_c_locale;
    return l;
}
static void *g___duplocale(void *l) {
    if (!l || l == (void *)-1) return g___newlocale(0, "C", NULL);
    struct g_locale *n = malloc(sizeof *n);
    *n = *(struct g_locale *)l;
    return n;
}
static void g___freelocale(void *l) { if (l && l != &g_c_locale && l != (void *)-1) free(l); }
static __thread void *g_cur_locale;
static void *g___uselocale(void *l) {
    void *old = g_cur_locale ? g_cur_locale : (void *)-1;   // LC_GLOBAL_LOCALE
    if (l) g_cur_locale = l;
    return old;
}
// nl_langinfo items (glibc numbering) that libstdc++ and friends ask for.
static char *g_nl_langinfo(int item) {
    switch (item) {
    case 14:      return "UTF-8";                          // CODESET
    case 0x10000: return ".";                              // RADIXCHAR / DECIMAL_POINT
    case 0x10001: return "";                               // THOUSEP
    case 0x10002: return "";                               // GROUPING
    case 0x10003: return (char *)(uintptr_t)L'.';          // _NL_NUMERIC_DECIMAL_POINT_WC
    case 0x10004: return (char *)(uintptr_t)L',';          // _NL_NUMERIC_THOUSANDS_SEP_WC
    default:      return "";
    }
}
static char *g___nl_langinfo_l(int item, void *l) { (void)l; return g_nl_langinfo(item); }
static double g___strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
static float g___strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
static long double g_strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
static long g_strtol_l(const char *s, char **e, int b, void *l) { (void)l; return strtol(s, e, b); }
static unsigned long g_strtoul_l(const char *s, char **e, int b, void *l) { (void)l; return strtoul(s, e, b); }
static int g_toupper_l(int c, void *l) { (void)l; return toupper(c); }
static int g_tolower_l(int c, void *l) { (void)l; return tolower(c); }
static int g_isalpha_l(int c, void *l) { (void)l; return isalpha(c); }
static int g_isdigit_l(int c, void *l) { (void)l; return isdigit(c); }
static int g_isspace_l(int c, void *l) { (void)l; return isspace(c); }
static int g_isupper_l(int c, void *l) { (void)l; return isupper(c); }
static int g_islower_l(int c, void *l) { (void)l; return islower(c); }
static int g_isalnum_l(int c, void *l) { (void)l; return isalnum(c); }
static int g_isxdigit_l(int c, void *l) { (void)l; return isxdigit(c); }
static int g_strcasecmp_l(const char *a, const char *b, void *l) { (void)l; return strcasecmp(a, b); }
static int g_strncasecmp_l(const char *a, const char *b, size_t n, void *l) { (void)l; return strncasecmp(a, b, n); }
static int g___strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
static size_t g___strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
static int g___wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
static size_t g___wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
static wint_t g___towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
static wint_t g___towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
static wctype_t g___wctype_l(const char *p, void *l) { (void)l; return wctype(p); }
static int g___iswctype_l(wint_t c, wctype_t t, void *l) { (void)l; return iswctype(c, t); }
static size_t g___strftime_l(char *s, size_t m, const char *f, const struct tm *t, void *l) { (void)l; return strftime(s, m, f, t); }
static size_t g___wcsftime_l(wchar_t *s, size_t m, const wchar_t *f, const struct tm *t, void *l) { (void)l; return wcsftime(s, m, f, t); }

// ------------------------------------------------------------------ stat (old ABI)
// glibc < 2.33 exported stat as __xstat(ver, ...). Klepton's stat already
// produces the Linux/arm64 struct stat layout.
typedef int (*stat_fn)(const char *, void *);
typedef int (*fstat_fn)(int, void *);
static int g___xstat(int v, const char *p, void *b) { (void)v; return ((stat_fn)bionic("stat"))(p, b); }
static int g___lxstat(int v, const char *p, void *b) { (void)v; return ((stat_fn)bionic("lstat"))(p, b); }
static int g___fxstat(int v, int fd, void *b) { (void)v; return ((fstat_fn)bionic("fstat"))(fd, b); }
static int g___xmknod(int v, const char *p, mode_t m, dev_t *d) { (void)v; (void)p; (void)m; (void)d; errno = EPERM; return -1; }

// ------------------------------------------------------------------ _dl_find_object
// libgcc's unwinder (GCC >= 12 against glibc >= 2.35) asks this for the
// PT_GNU_EH_FRAME of the object containing a pc. Answered from the guest image
// registry; host code is not ours to unwind.
struct g_dl_find_object {
    unsigned long long flags;
    void *map_start, *map_end, *link_map, *eh_frame;
    unsigned long long reserved[7];
};
extern kl_image *kl_registry_at(int i, const char **soname);
static int g__dl_find_object(void *pc, struct g_dl_find_object *r) {
    typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
                     p_filesz, p_memsz, p_align; } phdr;
    for (int i = 0; ; i++) {
        kl_image *img = kl_registry_at(i, NULL);
        if (!img) break;
        uint8_t *b = kl_base(img);
        if ((uint8_t *)pc < b || (uint8_t *)pc >= b + kl_span(img)) continue;
        unsigned n = 0;
        const phdr *ph = kl_phdrs(img, &n);
        void *eh = NULL;
        for (unsigned k = 0; ph && k < n; k++)
            if (ph[k].p_type == 0x6474e550) eh = b + ph[k].p_vaddr;   // PT_GNU_EH_FRAME
        memset(r, 0, sizeof *r);
        r->map_start = b;
        r->map_end = b + kl_span(img);
        r->link_map = img;
        r->eh_frame = eh;
        return eh ? 0 : -1;
    }
    return -1;
}

// ------------------------------------------------------------------ varargs (_chk)
extern int klh_printf(const char *fmt, kl_va *va);
extern int klh_fprintf(void *f, const char *fmt, kl_va *va);
extern void klg_printf_chk(void), klg_fprintf_chk(void), klg_swscanf(void), klg_syslog_chk(void);
extern int klh_syslog(int pri, const char *fmt, kl_va *va);
int klgh_syslog_chk(int pri, int flag, const char *fmt, kl_va *va) { (void)flag; return klh_syslog(pri, fmt, va); }
// glibc's getc() macro reads FILE internals inline and calls __uflow when its
// buffer is empty. Klepton's FILE is not glibc's, so the inline read is
// meaningless; this at least makes the out-of-line path real.
static int g___uflow(void *fp) { return fgetc(kl_host_file(fp)); }
static char *g_dcgettext(const char *d, const char *m, int c) { (void)d; (void)c; return (char *)m; }
struct g_random_data { int32_t *fptr, *rptr, *state; int rand_type, rand_deg, rand_sep; int32_t *end_ptr; };
static int g_initstate_r(unsigned seed, char *st, size_t n, struct g_random_data *b) {
    (void)st; (void)n; b->rand_type = (int)seed ? (int)seed : 1; return 0;
}
static int g_random_r(struct g_random_data *b, int32_t *out) {
    uint32_t x = (uint32_t)b->rand_type;         // xorshift over the seed slot
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    b->rand_type = (int)x; *out = (int32_t)(x & 0x7fffffff); return 0;
}
static void g_free_sized(void *p, size_t n) { (void)n; free(p); }
static void g_free_aligned_sized(void *p, size_t a, size_t n) { (void)a; (void)n; free(p); }
// fstatfs: the fd's path through F_GETPATH, then Klepton's statfs, which
// already fills the Linux/arm64 struct statfs.
static int g_fstatfs(int fd, void *buf) {
    char path[PATH_MAX];
    if (fcntl(fd, F_GETPATH, path) != 0) return -1;
    int (*sf)(const char *, void *) = (int (*)(const char *, void *))bionic("statfs");
    return sf(path, buf);
}
// ------------------------------------------------------------------ batch 2
extern int klh_asprintf(char **out, const char *fmt, kl_va *va);
extern void klg_asprintf_chk(void), klg_err(void), klg_errx(void), klg_warn(void);
int klgh_asprintf_chk(char **out, int flag, const char *fmt, kl_va *va) { (void)flag; return klh_asprintf(out, fmt, va); }
extern int klh_fprintf(void *f, const char *fmt, kl_va *va);
static void vmsg(const char *fmt, kl_va *va) {
    char m[512] __attribute__((aligned(16)));
    if (fmt && kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) != (size_t)-1) vfprintf(stderr, fmt, (va_list)m);
}
void klgh_err(int code, const char *fmt, kl_va *va) { fprintf(stderr, "[guest err] "); vmsg(fmt, va); fprintf(stderr, ": %s\n", strerror(errno)); exit(code); }
void klgh_errx(int code, const char *fmt, kl_va *va) { fprintf(stderr, "[guest errx] "); vmsg(fmt, va); fprintf(stderr, "\n"); exit(code); }
void klgh_warn(const char *fmt, kl_va *va) { fprintf(stderr, "[guest warn] "); vmsg(fmt, va); fprintf(stderr, "\n"); }
static int g___vasprintf_chk(char **o, int flag, const char *fmt, void *va) {
    (void)flag;
    int (*f)(char **, const char *, void *) = (int (*)(char **, const char *, void *))bionic("vasprintf");
    return f(o, fmt, va);
}
static int g_zero_int(void) { return 0; }
static int g_enotsup(void) { errno = ENOTSUP; return -1; }
static void *g_null_enosys(void) { errno = ENOSYS; return NULL; }
// pthread barriers: Darwin has none. Built from a mutex and a cond, allocated
// behind the guest's (glibc: 32-byte) pthread_barrier_t.
struct g_barrier { pthread_mutex_t m; pthread_cond_t c; unsigned count, waiting, phase; };
static int g_barrier_init(void **b, const void *attr, unsigned count) {
    (void)attr;
    if (!count) return EINVAL;
    struct g_barrier *x = calloc(1, sizeof *x);
    pthread_mutex_init(&x->m, NULL); pthread_cond_init(&x->c, NULL);
    x->count = count; *b = x; return 0;
}
static int g_barrier_wait(void **b) {
    struct g_barrier *x = *b;
    pthread_mutex_lock(&x->m);
    unsigned ph = x->phase;
    if (++x->waiting == x->count) {
        x->waiting = 0; x->phase++;
        pthread_cond_broadcast(&x->c);
        pthread_mutex_unlock(&x->m);
        return -1;                                  // PTHREAD_BARRIER_SERIAL_THREAD
    }
    while (ph == x->phase) pthread_cond_wait(&x->c, &x->m);
    pthread_mutex_unlock(&x->m);
    return 0;
}
static int g_barrier_destroy(void **b) {
    struct g_barrier *x = *b;
    if (x) { pthread_mutex_destroy(&x->m); pthread_cond_destroy(&x->c); free(x); *b = NULL; }
    return 0;
}
// regex: glibc's flags and regmatch_t (int offsets) differ from Darwin's
// (REG_NEWLINE 4 vs 8, REG_NOSUB 8 vs 4; regoff_t is 64-bit here). The
// guest's regex_t (64 bytes on glibc) holds a pointer to a Darwin regex_t.
#include <regex.h>
static int g_regcomp(void *preg, const char *pat, int cflags) {
    int f = 0;
    if (cflags & 1) f |= REG_EXTENDED;
    if (cflags & 2) f |= REG_ICASE;
    if (cflags & 4) f |= REG_NEWLINE;
    if (cflags & 8) f |= REG_NOSUB;
    regex_t *r = calloc(1, sizeof *r);
    int rc = regcomp(r, pat, f);
    if (rc) { free(r); return rc; }
    memset(preg, 0, 64);
    *(regex_t **)preg = r;
    ((size_t *)preg)[6] = r->re_nsub;             // glibc re_nsub lives at +48
    return 0;
}
static int g_regexec(const void *preg, const char *s, size_t n, int32_t (*pm)[2], int eflags) {
    regex_t *r = *(regex_t *const *)preg;
    int f = 0;
    if (eflags & 1) f |= REG_NOTBOL;
    if (eflags & 2) f |= REG_NOTEOL;
    regmatch_t m[32];
    size_t k = n < 32 ? n : 32;
    int rc = regexec(r, s, k, k ? m : NULL, f);
    for (size_t i = 0; rc == 0 && i < k; i++) { pm[i][0] = (int32_t)m[i].rm_so; pm[i][1] = (int32_t)m[i].rm_eo; }
    return rc;
}
static void g_regfree(void *preg) { regex_t *r = *(regex_t **)preg; if (r) { regfree(r); free(r); } *(regex_t **)preg = NULL; }
// clock_nanosleep(clock, flags, req, rem): TIMER_ABSTIME is 1 on Linux.
static int g_clock_nanosleep(int clk, int flags, const struct timespec *req, struct timespec *rem) {
    struct timespec d = *req;
    if (flags & 1) {
        struct timespec now; clock_gettime(clk == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
        d.tv_sec = req->tv_sec - now.tv_sec; d.tv_nsec = req->tv_nsec - now.tv_nsec;
        if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; d.tv_sec--; }
        if (d.tv_sec < 0) return 0;
    }
    return nanosleep(&d, rem) == 0 ? 0 : errno;
}
static char *g_progname = "hlvr", *g_progname_full = "hlvr";
static int *g___h_errno_location(void) { return &h_errno; }
static ssize_t g___getdelim(char **l, size_t *n, int d, void *fp) { return getdelim(l, n, d, kl_host_file(fp)); }
static char *g_fgets_unlocked(char *b, int n, void *fp) { return fgets(b, n, kl_host_file(fp)); }
static size_t g___fpending(void *fp) { (void)fp; return 0; }
static int g___fsetlocking(void *fp, int t) { (void)fp; (void)t; return 1; }
static void g___explicit_bzero_chk(void *p, size_t n, size_t dl) { (void)dl; memset_s(p, n, 0, n); }
static int g___open64_2(const char *p, int f) { int (*o)(const char *, int, ...) = (int (*)(const char *, int, ...))bionic("open"); return o(p, f); }
static int g___openat_2(int d, const char *p, int f) { int (*o)(int, const char *, int, ...) = (int (*)(int, const char *, int, ...))bionic("openat"); return o(d, p, f); }
static char *g___realpath_chk(const char *p, char *r, size_t n) { (void)n; return realpath(p, r); }
static char *g___xpg_basename(char *p) { char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static long long g_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
static unsigned long long g_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
// posix_fallocate: grow (never shrink) the file to cover [o, o+l).
static int g_posix_fallocate(int fd, off_t o, off_t l) {
    struct stat st;
    if (fstat(fd, &st) != 0) return errno;
    if (st.st_size >= o + l) return 0;
    return ftruncate(fd, o + l) == 0 ? 0 : errno;
}
static int g_fallocate(int fd, int mode, off_t o, off_t l) { (void)mode; return g_posix_fallocate(fd, o, l) ? -1 : 0; }
static int g_eaccess(const char *p, int m) { return access(p, m); }
static void *g_sched_cpualloc(size_t n) { return calloc(1, (n + 63) / 64 * 8); }
static void g___longjmp_chk(jmp_buf b, int v) { longjmp(b, v); }

// ------------------------------------------------------------------ SysV semaphores
// tier0's CThreadSyncObject builds named semaphores on semget/semctl/semop.
// Forwarding to Darwin was doubly wrong: semctl is VARIADIC (glibc guests pass
// the 4th argument in x3, Darwin reads it from the stack — so IPC_STAT wrote a
// Darwin semid_ds through a stale stack word, smashing the caller's saved
// fp/lr), and the command numbers differ. A sandboxed app cannot use SysV IPC
// anyway, so this is an in-process implementation with Linux semantics.
#define G_IPC_CREAT 01000
#define G_IPC_EXCL  02000
#define G_IPC_NOWAIT 04000
struct g_semset { int used, key, nsems; unsigned short vals[64]; int pids[64]; long otime, ctime; int mode; };
static struct g_semset g_sems[256];
static pthread_mutex_t g_sem_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_sem_cond = PTHREAD_COND_INITIALIZER;
static int g_semget(int key, int nsems, int flags) {
    if (nsems < 0 || nsems > 64) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&g_sem_lock);
    int id = -1;
    for (int i = 0; key != 0 && i < 256; i++)
        if (g_sems[i].used && g_sems[i].key == key) { id = i; break; }
    if (id >= 0) {
        if ((flags & G_IPC_CREAT) && (flags & G_IPC_EXCL)) { pthread_mutex_unlock(&g_sem_lock); errno = EEXIST; return -1; }
        if (nsems > g_sems[id].nsems) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
        pthread_mutex_unlock(&g_sem_lock);
        return id;
    }
    if (key != 0 && !(flags & G_IPC_CREAT)) { pthread_mutex_unlock(&g_sem_lock); errno = ENOENT; return -1; }
    for (int i = 0; i < 256; i++) if (!g_sems[i].used) { id = i; break; }
    if (id < 0) { pthread_mutex_unlock(&g_sem_lock); errno = ENOSPC; return -1; }
    memset(&g_sems[id], 0, sizeof g_sems[id]);
    g_sems[id].used = 1; g_sems[id].key = key; g_sems[id].nsems = nsems; g_sems[id].mode = flags & 0777;
    g_sems[id].ctime = time(NULL);
    pthread_mutex_unlock(&g_sem_lock);
    return id;
}
// Linux/arm64 struct semid_ds (88 bytes).
struct g_semid_ds { int key, uid, gid, cuid, cgid; unsigned mode; unsigned short seq, pad; unsigned long u1, u2;
                    long otime, ctime; unsigned long nsems, r3, r4; };
static int g_semctl(int id, int num, int cmd, uintptr_t arg) {       // arg: the union semun, in x3
    pthread_mutex_lock(&g_sem_lock);
    if (id < 0 || id >= 256 || !g_sems[id].used) { pthread_mutex_unlock(&g_sem_lock); errno = EINVAL; return -1; }
    struct g_semset *S = &g_sems[id];
    int r = 0;
    switch (cmd) {
    case 0:  S->used = 0; pthread_cond_broadcast(&g_sem_cond); break;              // IPC_RMID
    case 1:  break;                                                                   // IPC_SET
    case 2: {                                                                         // IPC_STAT
        struct g_semid_ds *d = (struct g_semid_ds *)arg;
        if (d) { memset(d, 0, sizeof *d); d->key = S->key; d->uid = d->cuid = (int)getuid(); d->gid = d->cgid = (int)getgid();
                 d->mode = (unsigned)S->mode; d->otime = S->otime; d->ctime = S->ctime; d->nsems = (unsigned long)S->nsems; }
        break;
    }
    case 11: r = (num >= 0 && num < S->nsems) ? S->pids[num] : (errno = EINVAL, -1); break;   // GETPID
    case 12: r = (num >= 0 && num < S->nsems) ? S->vals[num] : (errno = EINVAL, -1); break;   // GETVAL
    case 13: { unsigned short *a = (unsigned short *)arg; for (int i = 0; a && i < S->nsems; i++) a[i] = S->vals[i]; break; }  // GETALL
    case 14: case 15: r = 0; break;                                                   // GETNCNT/GETZCNT
    case 16: if (num >= 0 && num < S->nsems) { S->vals[num] = (unsigned short)(int)arg; S->ctime = time(NULL); pthread_cond_broadcast(&g_sem_cond); }
             else { errno = EINVAL; r = -1; } break;                                   // SETVAL
    case 17: { unsigned short *a = (unsigned short *)arg; for (int i = 0; a && i < S->nsems; i++) S->vals[i] = a[i]; pthread_cond_broadcast(&g_sem_cond); break; }
    default: errno = EINVAL; r = -1;
    }
    pthread_mutex_unlock(&g_sem_lock);
    return r;
}
struct g_sembuf { unsigned short num; short op; short flg; };
static int g_semtimedop(int id, struct g_sembuf *ops, size_t n, const struct timespec *to) {
    struct timespec deadline;
    if (to) { clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += to->tv_sec; deadline.tv_nsec += to->tv_nsec;
              if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; } }
    pthread_mutex_lock(&g_sem_lock);
    for (;;) {
        if (id < 0 || id >= 256 || !g_sems[id].used) { pthread_mutex_unlock(&g_sem_lock); errno = EIDRM; return -1; }
        struct g_semset *S = &g_sems[id];
        int ok = 1, nowait = 0;
        for (size_t i = 0; i < n; i++) {                   // all-or-nothing, as Linux does
            if (ops[i].num >= S->nsems) { pthread_mutex_unlock(&g_sem_lock); errno = EFBIG; return -1; }
            int v = S->vals[ops[i].num];
            if ((ops[i].op < 0 && v + ops[i].op < 0) || (ops[i].op == 0 && v != 0)) { ok = 0; nowait |= ops[i].flg & G_IPC_NOWAIT; }
        }
        if (ok) {
            for (size_t i = 0; i < n; i++) { S->vals[ops[i].num] = (unsigned short)(S->vals[ops[i].num] + ops[i].op); S->pids[ops[i].num] = getpid(); }
            S->otime = time(NULL);
            pthread_cond_broadcast(&g_sem_cond);
            pthread_mutex_unlock(&g_sem_lock);
            return 0;
        }
        if (nowait) { pthread_mutex_unlock(&g_sem_lock); errno = EAGAIN; return -1; }
        int w = to ? pthread_cond_timedwait(&g_sem_cond, &g_sem_lock, &deadline) : pthread_cond_wait(&g_sem_cond, &g_sem_lock);
        if (w == ETIMEDOUT) { pthread_mutex_unlock(&g_sem_lock); errno = EAGAIN; return -1; }
    }
}
static int g_semop(int id, struct g_sembuf *ops, size_t n) { return g_semtimedop(id, ops, n, NULL); }

// KL_SPEW=1: tier0's DevWarning/DevMsg print here (formatted) instead of going
// through a logging channel that may be filtered out.
extern void klg_devwarning(void);
void klgh_devwarning(const char *fmt, kl_va *va) {
    char m[512] __attribute__((aligned(16)));
    char out[1024];
    if (fmt && kl_va_marshal(fmt, va, m, sizeof m, KL_VA_PRINTF) != (size_t)-1) {
        vsnprintf(out, sizeof out, fmt, (va_list)m);
        fprintf(stderr, "[spew] %s%s", out, (*out && out[strlen(out) - 1] == '\n') ? "" : "\n");
    }
}

// mprotect with Linux's 4 KB granularity on a 16 KB-page host. Source 2's
// reserve/commit allocators (Panorama's arena, tier0's Plat_VirtualAlloc)
// commit in 4 KB steps; Darwin refuses an address that is not 16 KB aligned,
// the commit fails, and the guest takes its out-of-memory path. Granting
// access rounds OUTWARD (a few extra accessible bytes are harmless); revoking
// it (PROT_NONE) rounds INWARD, so a live neighbour never loses access.
static int g_mprotect(void *addr, size_t len, int prot) {
    const uintptr_t pg = (uintptr_t)getpagesize();
    uintptr_t a = (uintptr_t)addr, e = a + len;
    if ((a & (pg - 1)) || (e & (pg - 1))) {
        if (prot) { a &= ~(pg - 1); e = (e + pg - 1) & ~(pg - 1); }
        else      { a = (a + pg - 1) & ~(pg - 1); e &= ~(pg - 1); if (e <= a) return 0; }
    }
    int (*f)(void *, size_t, int) = (int (*)(void *, size_t, int))bionic("mprotect");
    return f ? f((void *)a, e - a, prot) : mprotect((void *)a, e - a, prot);
}

// Raw syscalls Klepton's table does not know. tgkill(pid, tid, 0) is a
// liveness probe (signal 0 delivers nothing): libvideo asks it of its own
// worker threads, and ENOSYS read as "that thread is dead" — it tore the
// worker's state down and faulted on it. Probes are answered "alive"; a real
// signal still goes to Klepton, which refuses it by name.
static long g_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    if (n == 131 && a3 == 0) return 0;                      // tgkill(pid, tid, 0)
    if (n == 130 && a2 == 0) return 0;                      // tkill(tid, 0)
    long (*f)(long, long, long, long, long, long, long) =
        (long (*)(long, long, long, long, long, long, long))bionic("syscall");
    return f(n, a1, a2, a3, a4, a5, a6);
}

static long g___fdelt_chk(long d) { if (d < 0 || d >= 1024) abort(); return d / 64; }
int klgh_printf_chk(int flag, const char *fmt, kl_va *va) { (void)flag; return klh_printf(fmt, va); }
int klgh_fprintf_chk(void *f, int flag, const char *fmt, kl_va *va) { (void)flag; return klh_fprintf(f, fmt, va); }
static void narrow_fmt(const wchar_t *w, char *out, size_t cap) {
    size_t i = 0;
    for (; w[i] && i + 1 < cap; i++) out[i] = (w[i] < 128) ? (char)w[i] : '?';
    out[i] = 0;
}
int klgh_swscanf(const wchar_t *s, const wchar_t *fmt, kl_va *va) {
    char nf[512];
    narrow_fmt(fmt, nf, sizeof nf);
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(nf, va, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vswscanf(s, fmt, (va_list)m);
}
static int g_vswscanf(const wchar_t *s, const wchar_t *fmt, kl_va *va) { return klgh_swscanf(s, fmt, va); }
static int g_vfscanf(void *f, const char *fmt, kl_va *va) {
    char m[512] __attribute__((aligned(16)));
    if (kl_va_marshal(fmt, va, m, sizeof m, KL_VA_SCANF) == (size_t)-1) return -1;
    return vfscanf(kl_host_file(f), fmt, (va_list)m);
}
static int g___vfprintf_chk(void *f, int flag, const char *fmt, void *va) {
    (void)flag;
    int (*vf)(void *, const char *, void *) = (int (*)(void *, const char *, void *))bionic("vfprintf");
    return vf(f, fmt, va);
}
static size_t g___mbstowcs_chk(wchar_t *d, const char *s, size_t n, size_t dl) { (void)dl; return mbstowcs(d, s, n); }

// ------------------------------------------------------------------ misc
static void g___assert_fail(const char *e, const char *f, unsigned l, const char *fn) {
    fprintf(stderr, "[guest] assertion failed: %s (%s:%u %s)\n", e, f, l, fn ? fn : "?");
    abort();
}
static char g_libc_single_threaded = 0;
static void *g_libc_stack_end;
static long g_timezone; static int g_daylight;
static char *g_tzname[2] = { "GMT", "GMT" };
static void tz_refresh(void) {
    tzset();
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    g_timezone = -lt.tm_gmtoff; g_daylight = lt.tm_isdst > 0;
}
static unsigned long g___getauxval(unsigned long t) {
    unsigned long (*f)(unsigned long) = (unsigned long (*)(unsigned long))bionic("getauxval");
    return f ? f(t) : 0;
}
static int g_get_nprocs(void) { return (int)sysconf(_SC_NPROCESSORS_ONLN); }
static int g_getdtablesize(void) { return (int)sysconf(_SC_OPEN_MAX); }
static int g_sched_getcpu(void) { return 0; }
static int g_sched_getparam(pid_t p, struct sched_param *s) { (void)p; s->sched_priority = 0; return 0; }
static int g_sched_getscheduler(pid_t p) { (void)p; return 0; }          // SCHED_OTHER
static int g_sched_setscheduler(pid_t p, int pol, const struct sched_param *s) { (void)p; (void)pol; (void)s; return 0; }
static int g_affinity(pthread_t t, size_t n, void *set) {
    (void)t;
    memset(set, 0, n);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < cpus && (size_t)(i / 8) < n; i++) ((uint8_t *)set)[i / 8] |= (uint8_t)(1u << (i % 8));
    return 0;
}
static int g_setaffinity(pthread_t t, size_t n, const void *set) { (void)t; (void)n; (void)set; return 0; }
static int g_enosys(void) { errno = ENOSYS; return -1; }
static int g_eperm(void) { errno = EPERM; return -1; }
static void *g_mremap(void *o, size_t os, size_t ns, int fl, ...) { (void)o; (void)os; (void)ns; (void)fl; errno = ENOMEM; return (void *)-1; }
static int g_posix_fadvise(int fd, off_t o, off_t l, int a) { (void)fd; (void)o; (void)l; (void)a; return 0; }

static void g_malloc_stats(void) {}
static void *g_dlmopen(long ns, const char *f, int fl) {
    (void)ns;
    void *(*o)(const char *, int) = (void *(*)(const char *, int))bionic("dlopen");
    return o(f, fl);
}
static char *g_secure_getenv(const char *n) { return getenv(n); }
static char *g_gettext(const char *m) { return (char *)m; }
static char *g_dgettext(const char *d, const char *m) { (void)d; return (char *)m; }
static char *g_bindtextdomain(const char *d, const char *dir) { (void)dir; return (char *)d; }
static char *g_bind_textdomain_codeset(const char *d, const char *c) { (void)d; return (char *)c; }
static void *g_catopen(const char *n, int f) { (void)n; (void)f; return (void *)-1; }
static char *g_catgets(void *c, int s, int m, const char *d) { (void)c; (void)s; (void)m; return (char *)d; }
static int g_catclose(void *c) { (void)c; return 0; }
struct g_qsort_ctx { int (*cmp)(const void *, const void *, void *); void *arg; };
static int g_qsort_thunk(void *c, const void *a, const void *b) {
    struct g_qsort_ctx *x = c; return x->cmp(a, b, x->arg);
}
static void g_qsort_r(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *, void *), void *arg) {
    struct g_qsort_ctx c = { cmp, arg };
    qsort_r(base, n, sz, &c, g_qsort_thunk);
}
static int g_mkostemps(char *t, int suffix, int flags) { (void)flags; return mkstemps(t, suffix); }
static int g_mkostemp(char *t, int flags) { (void)flags; return mkstemp(t); }
static char *g_ctermid(char *s) { static char b[] = "/dev/tty"; if (s) { strcpy(s, b); return s; } return b; }
static int g_getcontext(void *u) { (void)u; fprintf(stderr, "[klepton] getcontext: not implemented\n"); errno = ENOSYS; return -1; }
static void g_makecontext(void *u, void (*f)(void), int argc, ...) { (void)u; (void)f; (void)argc; fprintf(stderr, "[klepton] makecontext: not implemented\n"); abort(); }
static int g_setcontext(const void *u) { (void)u; fprintf(stderr, "[klepton] setcontext: not implemented\n"); abort(); }
static int g_sigtimedwait(const void *set, void *info, const struct timespec *t) { (void)set; (void)info; if (t) nanosleep(t, NULL); errno = EAGAIN; return -1; }
static char *g_asctime(const struct tm *t) { return asctime(t); }

// ------------------------------------------------------------------ Linux desktop stubs
// X11 / xcb / udev / libdrm / libuuid: desktop-Linux plumbing with no meaning in
// a visionOS app. Answering "no display / no device" keeps the guest on its
// headless paths; uuid is real because callers persist it.
static void *g_null(void) { return NULL; }
static int g_zero(void) { return 0; }
static void g_uuid_generate(uint8_t out[16]) {
    arc4random_buf(out, 16);
    out[6] = (out[6] & 0x0f) | 0x40;        // version 4
    out[8] = (out[8] & 0x3f) | 0x80;        // RFC 4122 variant
}

// ------------------------------------------------------------------ process
static int g_argc; static char **g_argv; static char g_exe[1024];
void kl_glibc_set_process(int argc, char **argv, const char *exe_path) {
    g_argc = argc; g_argv = argv; snprintf(g_exe, sizeof g_exe, "%s", exe_path);
    extern void kl_proc_set_exe(const char *);
    kl_proc_set_exe(exe_path);
}
extern char **environ;
// The guest executable's _start calls this; ours ignores the stack-derived
// argc/argv (_start runs on a host stack) and uses the real ones.
static int g___libc_start_main(int (*mainfn)(int, char **, char **), int argc, char **argv,
                               void (*init)(void), void (*fini)(void), void (*rtld_fini)(void), void *stack_end) {
    (void)argc; (void)argv; (void)fini; (void)rtld_fini; (void)stack_end;
    if (init) init();
    fprintf(stderr, "[klepton] __libc_start_main: entering guest main\n");
    int rc = mainfn(g_argc, g_argv, environ);
    fprintf(stderr, "[klepton] guest main returned %d\n", rc);
    exit(rc);
}
static ssize_t g_readlink(const char *p, char *buf, size_t n) {
    if (p && (!strcmp(p, "/proc/self/exe") || !strcmp(p, "/proc/curproc/file")) && g_exe[0]) {
        size_t l = strlen(g_exe);
        if (l > n) l = n;
        memcpy(buf, g_exe, l);
        return (ssize_t)l;
    }
    ssize_t (*r)(const char *, char *, size_t) = (ssize_t (*)(const char *, char *, size_t))bionic("readlink");
    return r ? r(p, buf, n) : readlink(p, buf, n);
}

// ------------------------------------------------------------------ dlsym intercepts
// Source 2 reports fatal errors through SDL's message box, which on Linux forks
// a helper and shows nothing here. Answer the guest's dlsym of those two with
// functions that print the message, so a fatal error names itself.
// An app can see the box (kl_glibc_msgbox_hook: show it, save it). With
// KL_MSGBOX_PARK=1 the calling guest thread then stops here for good: Source 2
// deliberately crashes right after a fatal box, which on the headset closed the
// window before the text could be read.
void (*kl_glibc_msgbox_hook)(const char *title, const char *msg);
static int g_sdl_simple_box(uint32_t flags, const char *title, const char *msg, void *win) {
    (void)win;
    fprintf(stderr, "\n[guest message box] flags=%#x title=\"%s\"\n%s\n\n", flags, title ? title : "", msg ? msg : "");
    fflush(NULL);
    if (kl_glibc_msgbox_hook) kl_glibc_msgbox_hook(title ? title : "", msg ? msg : "");
    if (kl_env_on("KL_MSGBOX_PARK", 0)) {
        fprintf(stderr, "[guest message box] parking this thread (KL_MSGBOX_PARK)\n");
        fflush(NULL);
        for (;;) sleep(3600);
    }
    return 0;
}
struct g_sdl_msgbox { uint32_t flags; void *window; const char *title, *message; };
static int g_sdl_box(const struct g_sdl_msgbox *d, int *button) {
    if (button) *button = -1;
    return d ? g_sdl_simple_box(d->flags, d->title, d->message, NULL) : -1;
}
// ------------------------------------------------------------------ SDL2 -> host SDL2
// Source 2's tier0 dlopens libSDL2-2.0.so.0 and dlsyms ~130 SDL functions;
// panorama and rendersystemgl link libSDL2.so directly. Both are answered by the
// HOST SDL2 (Cocoa windows, Vulkan surfaces through MoltenVK) instead of the
// Linux build, which has no video backend here. SDL's structs are plain C with
// identical LP64/AAPCS64 layout on both, and none of the functions the engine
// uses are variadic, so the calls forward as-is. A few are answered here.
static char g_sdl_marker;
static void *g_sdl_host;
static void *sdl_host(void) {
    if (!g_sdl_host) {
        const char *p = getenv("KL_SDL2_DYLIB");
        g_sdl_host = dlopen(p ? p : "/opt/homebrew/lib/libSDL2-2.0.0.dylib", RTLD_NOW | RTLD_GLOBAL);
        if (!g_sdl_host) fprintf(stderr, "[glibc] host SDL2 not found: %s\n", dlerror());
        else {
            fprintf(stderr, "[glibc] SDL2 served by the host library\n");
            // On iOS-family platforms SDL refuses to initialise video ("Application
            // didn't initialize properly, did you include SDL_main.h") unless the
            // app declares its main ready — ours is SwiftUI's, not SDL_main.
            void (*ready)(void) = (void (*)(void))dlsym(g_sdl_host, "SDL_SetMainReady");
            if (ready) ready();
        }
    }
    return g_sdl_host;
}
static int is_sdl_soname(const char *p) {
    const char *b = p ? strrchr(p, '/') : NULL; b = b ? b + 1 : p;
    return b && (!strcmp(b, "libSDL2-2.0.so.0") || !strcmp(b, "libSDL2.so") || !strcmp(b, "libSDL2-2.0.so"));
}
static int g_sdl_vulkan_load(const char *path) {
    (void)path;                                  // a Linux loader name: use MoltenVK
    int (*f)(const char *) = (int (*)(const char *))dlsym(sdl_host(), "SDL_Vulkan_LoadLibrary");
    char mvk[1100];
    const char *dir = getenv("KL_MVK_DIR");
    snprintf(mvk, sizeof mvk, "%s/libMoltenVK.dylib", dir ? dir : ".");
    int r = f ? f(mvk) : -1;
    fprintf(stderr, "[glibc] SDL_Vulkan_LoadLibrary(%s) -> %d (guest asked for %s)\n", mvk, r, path ? path : "NULL");
    return r;
}
static const char *g_sdl_platform(void) { return "Linux"; }
static void *sdl_sym(const char *name) {
    if (!strcmp(name, "SDL_ShowSimpleMessageBox")) return (void *)g_sdl_simple_box;
    if (!strcmp(name, "SDL_ShowMessageBox")) return (void *)g_sdl_box;
    if (!strcmp(name, "SDL_Vulkan_LoadLibrary")) return (void *)g_sdl_vulkan_load;
    if (!strcmp(name, "SDL_GetPlatform")) return (void *)g_sdl_platform;
    void *p = sdl_host() ? dlsym(g_sdl_host, name) : NULL;
    if (!p) fprintf(stderr, "[glibc] SDL: host has no %s\n", name);
    return p;
}
static void *g_dlopen(const char *path, int flags) {
    if (is_sdl_soname(path)) return sdl_host() ? (void *)&g_sdl_marker : NULL;
    void *(*o)(const char *, int) = (void *(*)(const char *, int))bionic("dlopen");
    return o(path, flags);
}
static int g_dlclose(void *h) {
    if (h == &g_sdl_marker) return 0;
    int (*c)(void *) = (int (*)(void *))bionic("dlclose");
    return c ? c(h) : 0;
}
static void *g_dlsym(void *h, const char *name) {
    if (h == &g_sdl_marker && name) return sdl_sym(name);
    if (name && !strcmp(name, "SDL_ShowSimpleMessageBox")) return (void *)g_sdl_simple_box;
    if (name && !strcmp(name, "SDL_ShowMessageBox")) return (void *)g_sdl_box;
    void *(*d)(void *, const char *) = (void *(*)(void *, const char *))bionic("dlsym");
    void *r = d(h, name);
    // glibc's dlsym(handle, ...) searches the handle's whole dependency tree
    // (breadth first), not just the one object: Panorama asks libpangoft2's
    // handle for FcConfigCreate and FT_Load_Glyph, which live two levels down.
    if (!r && name && h && h != (void *)-1 && h != RTLD_DEFAULT) {
        kl_image *queue[64]; int qh = 0, qt = 0;
        for (int i = 0; ; i++) {                // is h a guest image?
            kl_image *img = kl_registry_at(i, NULL);
            if (!img) break;
            if ((void *)img == h) { queue[qt++] = img; break; }
        }
        while (qh < qt && !r) {
            kl_image *img = queue[qh++];
            if (qh > 1) r = kl_sym(img, name);
            if (r) break;
            char needed[32][128];
            int nn = kl_list_needed(kl_image_path(img), needed, 32);
            for (int k = 0; k < nn && qt < 64; k++) {
                kl_image *dep = kl_find_image(needed[k]);
                int seen = 0;
                for (int j = 0; j < qt; j++) seen |= queue[j] == dep;
                if (dep && !seen) queue[qt++] = dep;
            }
        }
    }
    if (getenv("KL_TRACE_DLSYM")) fprintf(stderr, "[glibc] dlsym(%p, %s) -> %p\n", h, name ? name : "", r);
    return r;
}

// ------------------------------------------------------------------ eventfd I/O
// Klepton's eventfd is a self-connected datagram socket. Linux gives read and
// write on it counter semantics (read returns and clears the sum) or, with
// EFD_SEMAPHORE, semaphore semantics (read returns 1 and decrements by 1).
// Semaphore mode stores one datagram per unit; counter mode one per write.
extern int kl_eventfd_kind(int fd);
extern void kl_eventfd_forget(int fd);
// The kind table is keyed by fd NUMBER, and a number outlives its eventfd when
// it is closed by any path that is not close() (dup2 over it, fclose, a
// library's own close). A pipe that later reuses the number must not be
// driven with recv(), so confirm the fd really is a datagram socket first.
static int efd_kind(int fd) { return kl_eventfd_kind(fd); }
extern void kl_eventfd_fresh(int fd);
// Calls that return a new descriptor clear any stale eventfd mark on it.
#define FRESH(call) ({ long _r = (long)(call); if (_r >= 0) kl_eventfd_fresh((int)_r); _r; })
typedef long (*fn6)(long, long, long, long, long, long);
static long fwd(const char *n, long a, long b, long c, long d) { return ((fn6)bionic(n))(a, b, c, d, 0, 0); }
static int g_open(const char *p, int f, long mode) { return (int)FRESH(fwd("open", (long)p, f, mode, 0)); }
static int g_openat(int dfd, const char *p, int f, long mode) { return (int)FRESH(fwd("openat", dfd, (long)p, f, mode)); }
static int g_socket(int d, int t, int p) { return (int)FRESH(fwd("socket", d, t, p, 0)); }
static int g_dup(int fd) { return (int)FRESH(fwd("dup", fd, 0, 0, 0)); }
static int g_dup2(int a, int b) { kl_eventfd_fresh(b); return (int)FRESH(fwd("dup2", a, b, 0, 0)); }
static int g_dup3(int a, int b, int f) { kl_eventfd_fresh(b); return (int)FRESH(fwd("dup3", a, b, f, 0)); }
static int g_accept(int s, void *a, void *l) { return (int)FRESH(fwd("accept", s, (long)a, (long)l, 0)); }
static int g_pipe(int fds[2]) { int r = (int)fwd("pipe", (long)fds, 0, 0, 0); if (!r) { kl_eventfd_fresh(fds[0]); kl_eventfd_fresh(fds[1]); } return r; }
static int g_pipe2(int fds[2], int f) { int r = (int)fwd("pipe2", (long)fds, f, 0, 0); if (!r) { kl_eventfd_fresh(fds[0]); kl_eventfd_fresh(fds[1]); } return r; }
extern void kl_eventfd_add(int fd, uint64_t n);
extern int  kl_eventfd_take(int fd, int semaphore, uint64_t *out);
extern int  kl_eventfd_nonblock(int fd);
static ssize_t g_write(int fd, const void *buf, size_t n) {
    int k = efd_kind(fd);
    if (k && n == 8) {
        uint64_t v; memcpy(&v, buf, 8);
        if (v == UINT64_MAX) { errno = EINVAL; return -1; }
        kl_eventfd_add(fd, v);
        return 8;
    }
    return write(fd, buf, n);
}
static ssize_t g_read(int fd, void *buf, size_t n) {
    int k = efd_kind(fd);
    if (k && n >= 8) {
        for (;;) {
            uint64_t v;
            if (kl_eventfd_take(fd, k == 2, &v)) { memcpy(buf, &v, 8); return 8; }
            if (kl_eventfd_nonblock(fd)) { errno = EAGAIN; return -1; }
            struct pollfd p = { .fd = fd, .events = POLLIN };
            if (poll(&p, 1, -1) < 0 && errno != EINTR) return -1;   // marker arrives with the count
        }
    }
    ssize_t (*r)(int, void *, size_t) = (ssize_t (*)(int, void *, size_t))bionic("read");
    return r(fd, buf, n);
}
static int g_close(int fd) {
    kl_eventfd_forget(fd);
    int (*c)(int) = (int (*)(int))bionic("close");
    return c ? c(fd) : close(fd);
}

// ------------------------------------------------------------------ sysconf
// glibc numbers its _SC_ names differently from Bionic (_SC_PAGESIZE is 30
// here, 0x27 there), so Klepton's Bionic sysconf answers the wrong question.
static long g_sysconf(int name) {
    switch (name) {
    case 0:   return sysconf(_SC_ARG_MAX);
    case 1:   return sysconf(_SC_CHILD_MAX);
    case 2:   return 100;                                   // _SC_CLK_TCK
    case 3:   return sysconf(_SC_NGROUPS_MAX);
    case 4:   return sysconf(_SC_OPEN_MAX);
    case 5:   return sysconf(_SC_STREAM_MAX);
    case 6:   return sysconf(_SC_TZNAME_MAX);
    case 29:  return 200809L;                               // _SC_VERSION
    case 30:  return getpagesize();                         // _SC_PAGESIZE
    case 43:  return sysconf(_SC_LINE_MAX);
    case 60:  return 1024;                                  // _SC_IOV_MAX
    case 69:  case 70: return 1024;                         // _SC_GET{GR,PW}_R_SIZE_MAX
    case 71:  return 256;                                   // _SC_LOGIN_NAME_MAX
    case 72:  return 32;                                    // _SC_TTY_NAME_MAX
    case 73:  return 4;                                     // _SC_THREAD_DESTRUCTOR_ITERATIONS
    case 74:  return 512;                                   // _SC_THREAD_KEYS_MAX
    case 75:  return 16384;                                 // _SC_THREAD_STACK_MIN
    case 76:  return -1;                                    // _SC_THREAD_THREADS_MAX
    case 83:  return sysconf(_SC_NPROCESSORS_CONF);
    case 84:  return sysconf(_SC_NPROCESSORS_ONLN);
    case 85:  return sysconf(_SC_PHYS_PAGES);
    case 86: {                                              // _SC_AVPHYS_PAGES
        long p = sysconf(_SC_PHYS_PAGES);
        return p > 0 ? p / 2 : p;
    }
    case 180: return 255;                                   // _SC_HOST_NAME_MAX
    case 188: return 65536;                                 // _SC_LEVEL1_DCACHE_SIZE
    case 189: return 4;                                     // _SC_LEVEL1_DCACHE_ASSOC
    case 190: return 64;                                    // _SC_LEVEL1_DCACHE_LINESIZE
    case 185: return 65536;                                 // _SC_LEVEL1_ICACHE_SIZE
    case 187: return 64;                                    // _SC_LEVEL1_ICACHE_LINESIZE
    case 191: return 4 * 1024 * 1024;                       // _SC_LEVEL2_CACHE_SIZE
    case 193: return 128;                                   // _SC_LEVEL2_CACHE_LINESIZE
    default:
        fprintf(stderr, "[glibc] sysconf(%d): not mapped\n", name);
        errno = EINVAL;
        return -1;
    }
}

// ------------------------------------------------------------------ Steam interfaces
// ISteamNetworkingSockets/Utils come from the in-process loopback stand-in
// (kl_steamnet.cpp); every other interface request goes to Valve's own
// libsteam_api, which answers without a Steam client exactly as it would on a
// PC with Steam closed.
extern void *kl_steamnet_interface(const char *version);
extern void *kl_steam_placeholder(const char *version);
static void *real_steam(const char *name) {
    kl_image *s = kl_find_image("libsteam_api.so");
    return s ? kl_sym(s, name) : NULL;
}
static void *g_find_user_iface(int32_t user, const char *ver) {
    if (getenv("KL_TRACE_STEAMNET")) fprintf(stderr, "[glibc] FindOrCreateUserInterface(%d, %s)\n", user, ver ? ver : "");
    void *p = kl_steamnet_interface(ver);
    if (!p) p = kl_steam_placeholder(ver);
    if (p) return p;
    void *(*f)(int32_t, const char *) = (void *(*)(int32_t, const char *))real_steam("SteamInternal_FindOrCreateUserInterface");
    return f ? f(user, ver) : NULL;
}
static void *g_find_server_iface(int32_t user, const char *ver) {
    void *p = kl_steamnet_interface(ver);
    if (p) return p;
    void *(*f)(int32_t, const char *) = (void *(*)(int32_t, const char *))real_steam("SteamInternal_FindOrCreateGameServerInterface");
    return f ? f(user, ver) : NULL;
}

// ------------------------------------------------------------------ fatal signals
// Source 2 installs Breakpad, which takes SIGSEGV/SIGBUS/SIGILL/SIGABRT/
// SIGFPE/SIGTRAP and exits quietly after trying to write a minidump. Keep those
// with Klepton's fault reporter (it names the guest frame); the guest's
// sigaction succeeds but is not installed. KL_GUEST_FAULT_HANDLERS=1 opts out.
static int fatal_linux_sig(int s) { return s == 4 || s == 5 || s == 6 || s == 7 || s == 8 || s == 11; }
static void *g_signal(int sig, void *h) {
    if (fatal_linux_sig(sig) && !getenv("KL_GUEST_FAULT_HANDLERS")) return NULL;   // SIG_DFL
    void *(*f)(int, void *) = (void *(*)(int, void *))bionic("signal");
    return f ? f(sig, h) : NULL;
}
static int g_sigaction(int sig, const void *act, void *old) {
    if (act && fatal_linux_sig(sig) && !getenv("KL_GUEST_FAULT_HANDLERS")) {
        if (old) memset(old, 0, 152);                        // glibc/arm64 struct sigaction
        return 0;
    }
    int (*f)(int, const void *, void *) = (int (*)(int, const void *, void *))bionic("sigaction");
    return f(sig, act, old);
}

// A fault in a thread that has the fault signal BLOCKED kills the process with
// no handler at all. Engines block "all signals" in workers; keep the
// synchronous fault signals deliverable (Linux sigset bit = signal - 1).
static void unblock_fatal(uint64_t *set) {
    static const int sigs[] = {4, 5, 6, 7, 8, 11};
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) set[0] &= ~(1ull << (sigs[i] - 1));
}
static int g_sigprocmask(int how, const uint64_t *set, uint64_t *old) {
    uint64_t copy[16];
    const uint64_t *use = set;
    if (set && how != 1) { memcpy(copy, set, 8); unblock_fatal(copy); use = copy; }  // 1 = SIG_UNBLOCK
    int (*f)(int, const void *, void *) = (int (*)(int, const void *, void *))bionic("sigprocmask");
    return f(how, use, old);
}
static int g_pthread_sigmask(int how, const uint64_t *set, uint64_t *old) {
    uint64_t copy[16];
    const uint64_t *use = set;
    if (set && how != 1) { memcpy(copy, set, 8); unblock_fatal(copy); use = copy; }
    int (*f)(int, const void *, void *) = (int (*)(int, const void *, void *))bionic("pthread_sigmask");
    return f(how, use, old);
}

// SteamInternal_ContextInit(initData): initData = { void (*fill)(void *ctx);
// uintptr_t counter; ctx }. Valve's version runs `fill` only once SteamAPI_Init
// has succeeded, so without a Steam client every context stays empty. Run it
// once: `fill` calls SteamInternal_FindOrCreateUserInterface per interface,
// and those answer as above.
struct g_ctxinit { void (*fill)(void *); uintptr_t counter; char ctx[]; };
static void *g_create_interface(const char *ver) {
    void *p = kl_steam_placeholder(ver);
    if (p) return p;
    void *(*f)(const char *) = (void *(*)(const char *))real_steam("SteamInternal_CreateInterface");
    return f ? f(ver) : NULL;
}
static void *g_context_init(struct g_ctxinit *d) {
    if (d->counter != 1) { d->counter = 1; d->fill(d->ctx); }
    return d->ctx;
}

typedef struct { const char *name; void *fn; } glibc_ent;
#define H(n) { #n, (void *)n }
#define G(n) { #n, (void *)g_##n }
#define A(n, bion) { #n, NULL }   /* resolved through bionic() at lookup time */
static const glibc_ent g_impl[] = {
    { "__tls_get_addr", (void *)kl_tls_get_addr },
    { "__libc_start_main", (void *)g___libc_start_main }, { "readlink", (void *)g_readlink },
    { "dlsym", (void *)g_dlsym }, { "sigaction", (void *)g_sigaction }, { "signal", (void *)g_signal },
    { "bsd_signal", (void *)g_signal }, { "__sysv_signal", (void *)g_signal },
    { "sigprocmask", (void *)g_sigprocmask }, { "pthread_sigmask", (void *)g_pthread_sigmask },
    { "SteamInternal_FindOrCreateUserInterface", (void *)g_find_user_iface },
    { "SteamInternal_ContextInit", (void *)g_context_init },
    { "SteamInternal_CreateInterface", (void *)g_create_interface },
    { "SteamInternal_FindOrCreateGameServerInterface", (void *)g_find_server_iface }, { "dlopen", (void *)g_dlopen }, { "dlclose", (void *)g_dlclose },
    { "sysconf", (void *)g_sysconf }, { "__sysconf", (void *)g_sysconf },
    { "read", (void *)g_read }, { "write", (void *)g_write }, { "close", (void *)g_close },
    { "open", (void *)g_open }, { "open64", (void *)g_open }, { "openat", (void *)g_openat }, { "openat64", (void *)g_openat },
    { "socket", (void *)g_socket }, { "dup", (void *)g_dup }, { "dup2", (void *)g_dup2 }, { "dup3", (void *)g_dup3 },
    { "accept", (void *)g_accept }, { "pipe", (void *)g_pipe }, { "pipe2", (void *)g_pipe2 },
    { "SDL_ShowSimpleMessageBox", (void *)g_sdl_simple_box }, { "SDL_ShowMessageBox", (void *)g_sdl_box },
    { "__ctype_b_loc", (void *)g___ctype_b_loc },
    { "__ctype_tolower_loc", (void *)g___ctype_tolower_loc },
    { "__ctype_toupper_loc", (void *)g___ctype_toupper_loc },
    { "__newlocale", (void *)g___newlocale }, { "newlocale", (void *)g___newlocale },
    { "__duplocale", (void *)g___duplocale }, { "duplocale", (void *)g___duplocale },
    { "__freelocale", (void *)g___freelocale }, { "freelocale", (void *)g___freelocale },
    { "__uselocale", (void *)g___uselocale }, { "uselocale", (void *)g___uselocale },
    { "nl_langinfo", (void *)g_nl_langinfo }, { "__nl_langinfo_l", (void *)g___nl_langinfo_l },
    { "__strtod_l", (void *)g___strtod_l }, { "__strtof_l", (void *)g___strtof_l },
    { "strtod_l", (void *)g___strtod_l }, { "strtof_l", (void *)g___strtof_l },
    { "strtold_l", (void *)g_strtold_l }, { "__strtold_l", (void *)g_strtold_l },
    { "strtol_l", (void *)g_strtol_l }, { "__strtol_l", (void *)g_strtol_l },
    { "strtoul_l", (void *)g_strtoul_l }, { "__strtoul_l", (void *)g_strtoul_l },
    { "strtoll_l", (void *)g_strtoll_l }, { "strtoull_l", (void *)g_strtoull_l },
    { "strcoll_l", (void *)g___strcoll_l }, { "strxfrm_l", (void *)g___strxfrm_l },
    { "wcscoll_l", (void *)g___wcscoll_l }, { "wcsxfrm_l", (void *)g___wcsxfrm_l },
    { "towlower_l", (void *)g___towlower_l }, { "towupper_l", (void *)g___towupper_l },
    { "wctype_l", (void *)g___wctype_l }, { "iswctype_l", (void *)g___iswctype_l },
    { "strftime_l", (void *)g___strftime_l }, { "wcsftime_l", (void *)g___wcsftime_l },
    { "nl_langinfo_l", (void *)g___nl_langinfo_l },
    { "toupper_l", (void *)g_toupper_l }, { "tolower_l", (void *)g_tolower_l },
    { "__toupper_l", (void *)g_toupper_l }, { "__tolower_l", (void *)g_tolower_l },
    { "isalpha_l", (void *)g_isalpha_l }, { "isdigit_l", (void *)g_isdigit_l },
    { "isspace_l", (void *)g_isspace_l }, { "isupper_l", (void *)g_isupper_l },
    { "islower_l", (void *)g_islower_l }, { "isalnum_l", (void *)g_isalnum_l },
    { "isxdigit_l", (void *)g_isxdigit_l },
    { "strcasecmp_l", (void *)g_strcasecmp_l }, { "strncasecmp_l", (void *)g_strncasecmp_l },
    { "__strcasecmp_l", (void *)g_strcasecmp_l }, { "__strncasecmp_l", (void *)g_strncasecmp_l },
    { "__strcoll_l", (void *)g___strcoll_l }, { "__strxfrm_l", (void *)g___strxfrm_l },
    { "__wcscoll_l", (void *)g___wcscoll_l }, { "__wcsxfrm_l", (void *)g___wcsxfrm_l },
    { "__towlower_l", (void *)g___towlower_l }, { "__towupper_l", (void *)g___towupper_l },
    { "__wctype_l", (void *)g___wctype_l }, { "__iswctype_l", (void *)g___iswctype_l },
    { "__strftime_l", (void *)g___strftime_l }, { "__wcsftime_l", (void *)g___wcsftime_l },
    { "__xstat", (void *)g___xstat }, { "__xstat64", (void *)g___xstat },
    { "__lxstat", (void *)g___lxstat }, { "__lxstat64", (void *)g___lxstat },
    { "__fxstat", (void *)g___fxstat }, { "__fxstat64", (void *)g___fxstat },
    { "__xmknod", (void *)g___xmknod },
    { "_dl_find_object", (void *)g__dl_find_object },
    { "__printf_chk", (void *)klg_printf_chk }, { "__fprintf_chk", (void *)klg_fprintf_chk },
    { "__vfprintf_chk", (void *)g___vfprintf_chk },
    { "__isoc23_swscanf", (void *)klg_swscanf }, { "__isoc99_swscanf", (void *)klg_swscanf },
    { "__isoc23_vswscanf", (void *)g_vswscanf }, { "__isoc23_vfscanf", (void *)g_vfscanf },
    { "__isoc99_vfscanf", (void *)g_vfscanf },
    { "__mbstowcs_chk", (void *)g___mbstowcs_chk },
    { "__assert_fail", (void *)g___assert_fail },
    { "__libc_single_threaded", &g_libc_single_threaded },
    { "__libc_stack_end", &g_libc_stack_end },
    { "__timezone", &g_timezone }, { "timezone", &g_timezone },
    { "daylight", &g_daylight }, { "__daylight", &g_daylight }, { "tzname", g_tzname },
    { "__getauxval", (void *)g___getauxval },
    { "get_nprocs", (void *)g_get_nprocs }, { "getdtablesize", (void *)g_getdtablesize },
    { "sched_getcpu", (void *)g_sched_getcpu }, { "sched_getparam", (void *)g_sched_getparam },
    { "sched_getscheduler", (void *)g_sched_getscheduler }, { "sched_setscheduler", (void *)g_sched_setscheduler },
    { "pthread_getaffinity_np", (void *)g_affinity }, { "pthread_setaffinity_np", (void *)g_setaffinity },
    { "pthread_cancel", (void *)g_enosys }, { "pthread_setcanceltype", (void *)g_enosys },
    { "execlp", (void *)g_enosys }, { "execvp", (void *)g_enosys }, { "execvpe", (void *)g_enosys },
    { "mount", (void *)g_eperm }, { "shmget", (void *)g_enosys }, { "shmat", (void *)g_enosys },
    { "shmdt", (void *)g_enosys }, { "shmctl", (void *)g_enosys },
    { "mremap", (void *)g_mremap }, { "posix_fadvise", (void *)g_posix_fadvise },
    { "posix_fadvise64", (void *)g_posix_fadvise }, { "posix_fallocate", (void *)g_posix_fallocate },
    { "posix_fallocate64", (void *)g_posix_fallocate }, { "malloc_stats", (void *)g_malloc_stats },
    { "dlmopen", (void *)g_dlmopen }, { "secure_getenv", (void *)g_secure_getenv },
    { "gettext", (void *)g_gettext }, { "dgettext", (void *)g_dgettext },
    { "bindtextdomain", (void *)g_bindtextdomain }, { "bind_textdomain_codeset", (void *)g_bind_textdomain_codeset },
    { "catopen", (void *)g_catopen }, { "catgets", (void *)g_catgets }, { "catclose", (void *)g_catclose },
    { "qsort_r", (void *)g_qsort_r }, { "mkostemp", (void *)g_mkostemp }, { "mkostemps64", (void *)g_mkostemps }, { "mkostemp64", (void *)g_mkostemp },
    { "ctermid", (void *)g_ctermid }, { "getcontext", (void *)g_getcontext },
    { "makecontext", (void *)g_makecontext }, { "setcontext", (void *)g_setcontext },
    { "sigtimedwait", (void *)g_sigtimedwait }, { "asctime", (void *)g_asctime },
    { "XOpenDisplay", (void *)g_null }, { "XFetchName", (void *)g_zero }, { "XFlush", (void *)g_zero },
    { "XFree", (void *)g_zero }, { "XQueryPointer", (void *)g_zero }, { "XQueryTree", (void *)g_zero },
    { "XRaiseWindow", (void *)g_zero }, { "xcb_connect", (void *)g_null },
    { "udev_new", (void *)g_null }, { "udev_unref", (void *)g_null },
    { "udev_enumerate_new", (void *)g_null }, { "udev_enumerate_unref", (void *)g_null },
    { "udev_enumerate_add_match_subsystem", (void *)g_zero }, { "udev_enumerate_scan_devices", (void *)g_zero },
    { "udev_enumerate_get_list_entry", (void *)g_null }, { "udev_list_entry_get_next", (void *)g_null },
    { "udev_list_entry_get_name", (void *)g_null }, { "udev_device_new_from_syspath", (void *)g_null },
    { "udev_device_get_property_value", (void *)g_null }, { "udev_device_unref", (void *)g_null },
    { "drmGetVersion", (void *)g_null }, { "drmFreeVersion", (void *)g_zero },
    { "uuid_generate", (void *)g_uuid_generate },
    { "__syslog_chk", (void *)klg_syslog_chk }, { "__fdelt_chk", (void *)g___fdelt_chk },
    { "recvmmsg", (void *)g_enosys }, { "sendmmsg", (void *)g_enosys },
    { "zError", (void *)zError },
    { "fstatfs", (void *)g_fstatfs }, { "fstatfs64", (void *)g_fstatfs },
    { "free_sized", (void *)g_free_sized }, { "syscall", (void *)g_syscall }, { "mprotect", (void *)g_mprotect },
    { "semget", (void *)g_semget }, { "semctl", (void *)g_semctl }, { "semop", (void *)g_semop },
    { "semtimedop", (void *)g_semtimedop },
    { "__asprintf_chk", (void *)klg_asprintf_chk }, { "__vasprintf_chk", (void *)g___vasprintf_chk },
    { "err", (void *)klg_err }, { "errx", (void *)klg_errx }, { "warn", (void *)klg_warn },
    { "pthread_attr_setinheritsched", (void *)g_zero_int },
    { "pthread_barrier_init", (void *)g_barrier_init }, { "pthread_barrier_wait", (void *)g_barrier_wait },
    { "pthread_barrier_destroy", (void *)g_barrier_destroy },
    { "regcomp", (void *)g_regcomp }, { "regexec", (void *)g_regexec }, { "regfree", (void *)g_regfree },
    { "clock_nanosleep", (void *)g_clock_nanosleep },
    { "__progname", &g_progname }, { "__progname_full", &g_progname_full },
    { "program_invocation_name", &g_progname_full }, { "program_invocation_short_name", &g_progname },
    { "__environ", &environ }, { "__h_errno_location", (void *)g___h_errno_location },
    { "__getdelim", (void *)g___getdelim }, { "fgets_unlocked", (void *)g_fgets_unlocked },
    { "__fpending", (void *)g___fpending }, { "__fsetlocking", (void *)g___fsetlocking },
    { "__explicit_bzero_chk", (void *)g___explicit_bzero_chk },
    { "__open64_2", (void *)g___open64_2 }, { "__openat_2", (void *)g___openat_2 },
    { "__realpath_chk", (void *)g___realpath_chk }, { "__xpg_basename", (void *)g___xpg_basename },
    { "__isoc23_strtoll_l", (void *)g_strtoll_l }, { "__isoc23_strtoull_l", (void *)g_strtoull_l },
    { "__isoc23_strtoimax", (void *)strtoimax }, { "__isoc23_strtoumax", (void *)strtoumax },
    { "fallocate64", (void *)g_fallocate }, { "eaccess", (void *)g_eaccess },
    { "__sched_cpualloc", (void *)g_sched_cpualloc }, { "__sched_cpufree", (void *)free },
    { "__longjmp_chk", (void *)g___longjmp_chk }, { "__gcov_dump", (void *)g_zero_int },
    { "__gcov_flush", (void *)g_zero_int }, { "__lsan_disable", (void *)g_zero_int },
    { "__lsan_enable", (void *)g_zero_int }, { "__lsan_ignore_object", (void *)g_zero_int },
    { "statx", (void *)g_enosys }, { "close_range", (void *)g_enosys }, { "copy_file_range", (void *)g_enosys },
    { "splice", (void *)g_enosys }, { "setns", (void *)g_enosys }, { "unshare", (void *)g_enosys },
    { "scandirat", (void *)g_enosys }, { "pidfd_open", (void *)g_enosys }, { "fanotify_init", (void *)g_enosys },
    { "fanotify_mark", (void *)g_enosys }, { "umount", (void *)g_eperm }, { "umount2", (void *)g_eperm },
    { "fsopen", (void *)g_enosys }, { "fsmount", (void *)g_enosys }, { "fsconfig", (void *)g_enosys },
    { "fspick", (void *)g_enosys }, { "move_mount", (void *)g_enosys }, { "open_tree", (void *)g_enosys },
    { "mount_setattr", (void *)g_enosys }, { "nftw64", (void *)g_enosys }, { "fts64_open", (void *)g_null_enosys },
    { "posix_spawn", (void *)g_enosys }, { "posix_spawnp", (void *)g_enosys },
    { "getxattr", (void *)g_enotsup }, { "lgetxattr", (void *)g_enotsup }, { "fgetxattr", (void *)g_enotsup },
    { "setxattr", (void *)g_enotsup }, { "lsetxattr", (void *)g_enotsup }, { "fsetxattr", (void *)g_enotsup },
    { "listxattr", (void *)g_enotsup }, { "llistxattr", (void *)g_enotsup }, { "flistxattr", (void *)g_enotsup },
    { "removexattr", (void *)g_enotsup },
    { "setmntent", (void *)g_null_enosys }, { "getmntent_r", (void *)g_null_enosys }, { "endmntent", (void *)g_zero_int },
    { "hasmntopt", (void *)g_null_enosys }, { "textdomain", (void *)g_gettext },
    { "gzopen64", (void *)gzopen }, { "gzopen", (void *)gzopen }, { "gzclose", (void *)gzclose },
    { "gzwrite", (void *)gzwrite }, { "gzflush", (void *)gzflush }, { "gzerror", (void *)gzerror },
    { "deflateSetHeader", (void *)deflateSetHeader }, { "inflateGetHeader", (void *)inflateGetHeader }, { "free_aligned_sized", (void *)g_free_aligned_sized },
    { "__uflow", (void *)g___uflow }, { "dcgettext", (void *)g_dcgettext },
    { "initstate_r", (void *)g_initstate_r }, { "random_r", (void *)g_random_r },
    { "_setjmp", (void *)_setjmp }, { "_longjmp", (void *)_longjmp }, { "__sigsetjmp", (void *)sigsetjmp },
    { "fegetround", (void *)fegetround }, { "fesetround", (void *)fesetround },
    // HOST: Darwin's function, same ABI.
    H(asctime_r), H(bcmp), H(floor), H(lround), H(lroundf), H(nextafterf), H(cabsf), H(cexpf),
    H(wcscasecmp), H(wcsncasecmp), H(wcsdup), H(wcsstr), H(wcslcat), H(wcslcpy),
    H(backtrace), H(backtrace_symbols), H(iconv), H(iconv_open), H(iconv_close),
    H(putenv), H(dirfd), H(tmpnam), H(getloadavg), H(futimes), H(faccessat),
    H(memmem), H(wcwidth), H(wcswidth), H(wctomb), H(ttyname), H(lchmod), H(posix_madvise),
    { "in6addr_loopback", (void *)&in6addr_loopback }, H(creat), { "creat64", (void *)creat }, { "mkstemps64", (void *)mkstemps },
    { "_Exit", (void *)_exit }, { "fileno_unlocked", (void *)fileno },
    { "__mbrlen", (void *)mbrlen }, { "__xpg_strerror_r", NULL },
};
// ALIAS: glibc name -> the Bionic name Klepton serves.
static const struct { const char *glibc, *bionic; } g_alias[] = {
    { "__errno_location", "__errno" }, { "__xpg_strerror_r", "strerror_r" },
 { "fcntl64", "fcntl" }, { "mmap64", "mmap" },
    { "readdir64", "readdir" }, { "scandir64", "scandir" }, { "alphasort64", "alphasort" },
    { "statfs64", "statfs" }, { "statvfs64", "statvfs" }, { "getrlimit64", "getrlimit" },
    { "setrlimit64", "setrlimit" }, { "truncate64", "truncate" }, { "tmpfile64", "tmpfile" },
    { "freopen64", "freopen" }, { "mkstemp64", "mkstemp" }, { "sendfile64", "sendfile" },

    { "__isoc23_sscanf", "sscanf" }, { "__isoc99_sscanf", "sscanf" },
    { "__isoc23_fscanf", "fscanf" }, { "__isoc99_fscanf", "fscanf" },
    { "__isoc23_vsscanf", "vsscanf" }, { "__isoc99_vsscanf", "vsscanf" },
    { "__isoc23_strtol", "strtol" }, { "__isoc23_strtoll", "strtoll" },
    { "__isoc23_strtoul", "strtoul" }, { "__isoc23_strtoull", "strtoull" },
    { "__isoc23_wcstoll", "wcstoll" }, { "__isoc23_wcstoull", "wcstoull" },
};

// ------------------------------------------------------------------ import tracer
// KL_TRACE_IMPORTS=1 wraps EVERY guest import in a forwarding stub that logs its
// name, but only while kl_imp_trace_on is set: kl_run_init turns it on for the
// constructor named by KL_TRACE_IMPORTS_INIT=<image substring>:<index>. That
// names the last calls before a crash that leaves no frame chain.
volatile int kl_imp_trace_on;
extern char kl_imp_trace_tramp[];
void kl_imp_trace_log(const char *name) {
    if (kl_imp_trace_on) { char b[200]; int n = snprintf(b, sizeof b, "  [imp] %s\n", name); if (n > 0) write(2, b, (size_t)n); }
}
static void *resolve_plain(const char *name);
#include <mach/mach.h>
#include <mach/vm_map.h>
static int is_exec(void *p) {
    vm_address_t a = (vm_address_t)p; vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;
    return a <= (vm_address_t)p && (info.protection & VM_PROT_EXECUTE);
}
static void *trace_wrap(const char *name, void *real) {
    if (!real || !is_exec(real)) return real;            // data imports stay data
    struct { const char *name; void *real; } *d = malloc(sizeof *d);
    d->name = strdup(name); d->real = real;
    return kl_trace_stub(d->name, d, kl_imp_trace_tramp);
}

void *kl_glibc_lookup(const char *name) {
    if (g_in_lookup) return NULL;
    // KL_TRACE_IMPORTS=<a,b,...>: only images whose path contains one of these.
    extern const char *kl_binding_image_path;
    const char *want = getenv("KL_TRACE_IMPORTS");
    int trace = 0;
    if (want && kl_binding_image_path) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", want);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) trace |= strstr(kl_binding_image_path, t) != NULL;
    }
    if (trace && getenv("KL_TRACE_IMPORTS_ALWAYS")) kl_imp_trace_on = 1;
    if (trace && strncmp(name, "SDL_", 4)) {
        void *p = resolve_plain(name);
        if (!p) { g_in_lookup = 1; p = kl_shim_lookup(name); g_in_lookup = 0; }
        if (!p) p = kl_guest_sym_global(name);
        return trace_wrap(name, p);
    }
    return resolve_plain(name);
}

// KL_GUEST_BIGALLOC_MMAP=<bytes> (test knob, memory): the guest's malloc family serves requests of <bytes> or more from
// their own mmap and gives them back with munmap at free, so a freed big buffer (e.g. a texture streaming read) leaves
// the footprint at once instead of staying in libmalloc's large-block cache (vmmap "Malloc Large (empty)"). Blocks are
// page-aligned and tracked in a table; frees of anything else go to the host as before. Unset: nothing changes (the
// names are not bound here).
#include <sys/mman.h>
#include <malloc/malloc.h>
static size_t g_bm_th; static int g_bm_init;
static size_t bm_th(void) {
    if (!g_bm_init) { const char *e = getenv("KL_GUEST_BIGALLOC_MMAP"); g_bm_th = e ? (size_t)strtoull(e, NULL, 0) : 0; g_bm_init = 1;
                      if (g_bm_th) fprintf(stderr, "[bigmm] guest allocations of %zu bytes or more come from mmap\n", g_bm_th); }
    return g_bm_th;
}
enum { kBmSlots = 1 << 15 };
static struct { void *p; size_t len; } g_bm[kBmSlots];
static pthread_mutex_t g_bm_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t g_bm_live, g_bm_count, g_bm_total;
static unsigned bm_h(void *p) { return (unsigned)(((uintptr_t)p >> 14) * 2654435761u) & (kBmSlots - 1); }
static int bm_paged(void *p) { return !((uintptr_t)p & ((uintptr_t)getpagesize() - 1)); }
static void *bm_alloc(size_t n) {
    size_t pg = (size_t)getpagesize(), len = (n + pg - 1) & ~(pg - 1);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    int ok = 0;
    pthread_mutex_lock(&g_bm_mu);
    unsigned h = bm_h(p);
    for (unsigned k = 0; k < kBmSlots; k++, h = (h + 1) & (kBmSlots - 1))
        if (!g_bm[h].p) { g_bm[h].p = p; g_bm[h].len = len; g_bm_live += len; g_bm_count++; g_bm_total++; ok = 1; break; }
    size_t total = g_bm_total, live = g_bm_live;
    pthread_mutex_unlock(&g_bm_mu);
    if (!ok) { munmap(p, len); return NULL; }   // table full: the caller falls back to the host malloc
    if (total <= 5 || (total & (total - 1)) == 0) fprintf(stderr, "[bigmm] #%zu: %zu KB (live %.0f MB)\n", total, n >> 10, live / 1048576.0);
    return p;
}
static size_t bm_find(void *p) {
    size_t len = 0;
    pthread_mutex_lock(&g_bm_mu);
    unsigned h = bm_h(p);
    for (unsigned k = 0; k < kBmSlots && g_bm[h].p; k++, h = (h + 1) & (kBmSlots - 1)) if (g_bm[h].p == p) { len = g_bm[h].len; break; }
    pthread_mutex_unlock(&g_bm_mu);
    return len;
}
static size_t bm_take(void *p) {   // removes p from the table; returns its mapped length (0: not ours)
    size_t len = 0;
    pthread_mutex_lock(&g_bm_mu);
    unsigned h = bm_h(p);
    for (unsigned k = 0; k < kBmSlots && g_bm[h].p; k++, h = (h + 1) & (kBmSlots - 1))
        if (g_bm[h].p == p) {
            len = g_bm[h].len; g_bm_live -= len; g_bm_count--;
            // Backward-shift delete keeps the probe chains intact: walk the rest of the run and move back into the hole
            // each entry that the hole would cut off from its home slot. An entry that may stay (its home lies between
            // the hole and it) is stepped over, not the end of the walk: one further along can still need the hole.
            // Stopping at the first such entry (as this did) left later entries behind an empty slot, unfindable: their
            // free went to the host free (which ignores a pointer it doesn't own) and a realloc to the host path, which
            // copied malloc_size() = 0 bytes into the new block (the old contents silently lost) or, shrinking, got NULL.
            unsigned i = h, j = h;
            for (unsigned n = 1; n < kBmSlots; n++) {
                j = (j + 1) & (kBmSlots - 1);
                if (!g_bm[j].p) break;
                unsigned want = bm_h(g_bm[j].p);
                if (((j - want) & (kBmSlots - 1)) >= ((j - i) & (kBmSlots - 1))) { g_bm[i] = g_bm[j]; i = j; }
            }
            g_bm[i].p = NULL; g_bm[i].len = 0;
            break;
        }
    pthread_mutex_unlock(&g_bm_mu);
    return len;
}
static void *bmw_malloc(size_t n) { if (n >= g_bm_th) { void *p = bm_alloc(n); if (p) return p; } return malloc(n); }
static void bmw_free(void *p) {
    if (!p) return;
    if (bm_paged(p)) { size_t len = bm_take(p); if (len) { munmap(p, len); return; } }
    free(p);
}
static void *bmw_calloc(size_t c, size_t n) {
    if (n && c > SIZE_MAX / n) return NULL;
    if (c * n >= g_bm_th) { void *p = bm_alloc(c * n); if (p) return p; }   // fresh anonymous pages are zero
    return calloc(c, n);
}
static void *bmw_realloc(void *p, size_t n) {
    if (!p) return bmw_malloc(n);
    if (!n) { bmw_free(p); return NULL; }
    size_t old = bm_paged(p) ? bm_find(p) : 0;
    if (old) {
        if (n <= old && n >= g_bm_th) return p;
        void *q = bmw_malloc(n); if (!q) return NULL;
        memcpy(q, p, n < old ? n : old); bmw_free(p); return q;
    }
    if (n >= g_bm_th) {
        size_t os = malloc_size(p); void *q = bm_alloc(n);
        if (q) { memcpy(q, p, n < os ? n : os); free(p); return q; }
    }
    return realloc(p, n);
}
static int bmw_posix_memalign(void **pp, size_t a, size_t n) {
    if (n >= g_bm_th && a <= (size_t)getpagesize()) { void *p = bm_alloc(n); if (p) { *pp = p; return 0; } }
    return posix_memalign(pp, a, n);
}
static void *bmw_memalign(size_t a, size_t n) { void *p = NULL; if (a < sizeof(void *)) a = sizeof(void *); return bmw_posix_memalign(&p, a, n) == 0 ? p : NULL; }
static void *bmw_aligned_alloc(size_t a, size_t n) { return bmw_memalign(a, n); }
static size_t bmw_usable(void *p) { if (!p) return 0; if (bm_paged(p)) { size_t l = bm_find(p); if (l) return l; } return malloc_size(p); }
// C23 sized frees: the plain ones (g_free_sized) call the host free() directly, which would abort on a mapped block
static void bmw_free_sized(void *p, size_t n) { (void)n; bmw_free(p); }
static void bmw_free_aligned_sized(void *p, size_t a, size_t n) { (void)a; (void)n; bmw_free(p); }
static void *bm_lookup(const char *name) {
    static const struct { const char *n; void *f; } t[] = {
        { "malloc", (void *)bmw_malloc }, { "free", (void *)bmw_free }, { "calloc", (void *)bmw_calloc },
        { "realloc", (void *)bmw_realloc }, { "posix_memalign", (void *)bmw_posix_memalign },
        { "memalign", (void *)bmw_memalign }, { "aligned_alloc", (void *)bmw_aligned_alloc },
        { "malloc_usable_size", (void *)bmw_usable }, { "cfree", (void *)bmw_free },
        { "free_sized", (void *)bmw_free_sized }, { "free_aligned_sized", (void *)bmw_free_aligned_sized },
    };
    if (!bm_th()) return NULL;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, name)) return t[i].f;
    return NULL;
}

static void *resolve_plain(const char *name) {
    static int init;
    if (!init) { ctype_init(); locale_init(); tz_refresh(); init = 1;
                 extern int kl_eventfd_semaphore_ok; kl_eventfd_semaphore_ok = 1;
                 extern const char *kl_vk_extra_instance_exts[8];
                 kl_vk_extra_instance_exts[0] = "VK_KHR_xlib_surface";
                 kl_vk_extra_instance_exts[1] = "VK_KHR_xcb_surface";
                 kl_vk_extra_instance_exts[2] = "VK_KHR_wayland_surface";
                 extern int kl_vk_native_wsi;
                 kl_vk_native_wsi = !getenv("KL_VK_HEADLESS");
                 g_libc_stack_end = pthread_get_stackaddr_np(pthread_self()); }
    if (is_cxx_abi(name)) {
        void *p = g_cxx ? kl_sym(g_cxx, name) : NULL;
        if (!p && g_gcc) p = kl_sym(g_gcc, name);
        if (p) return p;
    }
    if (!strncmp(name, "SDL_", 4)) { void *p = sdl_sym(name); if (p) return p; }
    { void *bm = bm_lookup(name); if (bm) return bm; }
    if (getenv("KL_SPEW") && (!strcmp(name, "_Z10DevWarningPKcz") || !strcmp(name, "_Z6DevMsgPKcz") ||
                              !strcmp(name, "Warning") || !strcmp(name, "_Z7WarningPKcz")))
        return (void *)klg_devwarning;
    if (!strncmp(name, "SteamInternal_FindOrCreate", 26) && getenv("KL_NO_STEAMNET")) return NULL;
    for (unsigned i = 0; i < sizeof g_alias / sizeof g_alias[0]; i++)
        if (!strcmp(g_alias[i].glibc, name)) {
            void *p = bionic(g_alias[i].bionic);
            if (p) return p;
            break;
        }
    for (unsigned i = 0; i < sizeof g_impl / sizeof g_impl[0]; i++)
        if (g_impl[i].fn && !strcmp(g_impl[i].name, name)) return g_impl[i].fn;
    return NULL;
}
