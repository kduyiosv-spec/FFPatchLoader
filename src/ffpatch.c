//
//  ffpatch.c — FFPatchLoader main: interposition + patch file management
//
//  How it works
//  ------------
//  The target game embeds Tencent's IFix hot-patch engine (IFix.ILFixInterfaceBridge /
//  IFix.WrappersManagerImpl). At startup the game's own loader opens a UnityFS
//  AssetBundle ("assembly-csharp-patch.bytes"), pulls the TextAsset out of it and
//  feeds the bytes into the IFix VM. Everything in the patch (menu, ESP, aimbot...)
//  then runs as interpreted IL *inside the game's own VM* — zero native patching,
//  zero codesign conflicts, immune to iOS 27 PPL.
//
//  The only problem on iOS: the bundle file lives inside the sealed app bundle.
//  This loader solves it:
//    * Scans <HOME>/Documents (+ Inbox) for a patch file the user dropped in
//      (any name starting with "assembly-csharp-patch" or "remote_patch",
//       e.g. "assembly-csharp-patch.tYoVpinydaigU2jEIIrSUmNoYr8~3D").
//    * Copies it (newest wins) to three physical locations:
//         Documents/assembly-csharp-patch.bytes          (canonical)
//         Documents/remote_patch.bytes                   (game hot-update name)
//         Documents/codepatch/assembly-csharp-patch.bytes
//    * Interposes open/openat/fopen/access/faccessat/stat/lstat and redirects
//      READ-mode access to any "assembly-csharp-patch*" / "remote_patch*" path
//      (including the in-bundle streaming path) to the canonical file — so the
//      game's loader finds the mod patch even when it asks for the sealed copy.
//    * Belt & suspenders: also rebinds __DATA_CONST GOT slots in UnityFramework
//      and the main executable in case interposition missed pre-main binds.
//
//  Writes are NEVER redirected (game hot-update downloads pass through).
//  Every patch-path access is logged to Documents/ffpatch_loader.log.
//

#include "ffp_internal.h"
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/syslog.h>
#include <limits.h>
#include <syslog.h>

// ====================================================================
// Globals
// ====================================================================

char g_home[1024];
char g_docs[1024];
char g_canonical[1024];
char g_remote[1024];
char g_codepatch_dir[1024];
char g_codepatch[1024];
char g_logpath[1024];
bool g_ready = false;

open_fn      o_open;
openat_fn    o_openat;
fopen_fn     o_fopen;
access_fn    o_access;
faccessat_fn o_faccessat;
stat_fn      o_stat;
lstat_fn     o_lstat;

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_logmtx = PTHREAD_MUTEX_INITIALIZER;

// stats for the delayed summary
static volatile long g_n_open_match    = 0;
static volatile long g_n_stat_match    = 0;
static volatile long g_n_redirect      = 0;

// ====================================================================
// raw mach_vm_protect — mach trap -14 (avoids mach/mach_vm.h #error on iOS SDK)
// proven pattern carried over from the PTG project (v77+).
// ====================================================================

__attribute__((naked, noinline))
kern_return_t custom_mach_vm_protect(mach_port_name_t target,
                                     vm_address_t address,
                                     vm_size_t size,
                                     boolean_t set_maximum,
                                     vm_prot_t new_protection) {
    __asm__ volatile(
        "mov x16, #0xFFF2\n"
        "movk x16, #0xFFFF, lsl #16\n"
        "movk x16, #0xFFFF, lsl #32\n"
        "movk x16, #0xFFFF, lsl #48\n"
        "svc #0x80\n"
        "ret\n"
    );
}

// ====================================================================
// Logging — syslog + Documents/ffpatch_loader.log (truncated each launch)
// ====================================================================

static void log_v(const char *fmt, va_list ap) {
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, ap);

    syslog(LOG_WARNING, "%s", line); // shows as the process name in Console

    if (!g_ready || !g_logpath[0]) return; // pre-init: syslog only

    pthread_mutex_lock(&g_logmtx);
    // NB: call the REAL fopen — never our interposed one
    FILE *f = (o_fopen != NULL) ? o_fopen(g_logpath, "a") : NULL;
    if (f) {
        char ts[32];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(f, "[%s] %s\n", ts, line);
        fclose(f);
    }
    pthread_mutex_unlock(&g_logmtx);
}

void ffp_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v(fmt, ap);
    va_end(ap);
}

// ====================================================================
// Case-insensitive substring search (self-contained, no strcasstr)
// ====================================================================

bool ffp_cistr(const char *hay, const char *needle) {
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

// Does this path reference the IFix codepatch bundle?
bool path_is_patch(const char *p) {
    if (!p || !*p) return false;
    // "assembly-csharp-patch" covers: streaming path, persistent path,
    // weird download suffixes, any casing.
    if (ffp_cistr(p, "assembly-csharp-patch")) return true;
    // "remote_patch" — the game's own hot-update file name
    if (ffp_cistr(p, "remote_patch")) return true;
    return false;
}

// Should a READ-mode access to `path` be redirected to the canonical file?
static bool want_redirect(const char *path) {
    if (!g_ready || !g_canonical[0]) return false;
    if (!path_is_patch(path)) return false;
    // never redirect our own canonical copies onto themselves
    if (strcmp(path, g_canonical) == 0) return false;
    if (g_remote[0] && strcmp(path, g_remote) == 0) return false;
    if (g_codepatch[0] && strcmp(path, g_codepatch) == 0) return false;
    return true;
}

// ====================================================================
// ensure_init — resolve real libc fns + build canonical paths (once)
// ====================================================================

static void ensure_init_once(void) {
    // real function pointers. RTLD_NEXT from this dylib → libSystem.
    // Sanity guard: must NOT resolve to our own interposers (would loop).
    o_open      = (open_fn)dlsym(RTLD_NEXT, "open");
    o_openat    = (openat_fn)dlsym(RTLD_NEXT, "openat");
    o_fopen     = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    o_access    = (access_fn)dlsym(RTLD_NEXT, "access");
    o_faccessat = (faccessat_fn)dlsym(RTLD_NEXT, "faccessat");
    o_stat      = (stat_fn)dlsym(RTLD_NEXT, "stat");
    o_lstat     = (lstat_fn)dlsym(RTLD_NEXT, "lstat");

    if ((void *)o_open == (void *)ffp_open ||
        (void *)o_fopen == (void *)ffp_fopen) {
        syslog(LOG_ERR, "[ffpatch] FATAL: dlsym(RTLD_NEXT) resolved to our own interposer — interposition disabled");
        o_open = o_openat = NULL;
        o_fopen = NULL;
        return;
    }

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(g_home, sizeof(g_home), "%s", home);
        snprintf(g_docs, sizeof(g_docs), "%s/Documents", home);
        snprintf(g_canonical, sizeof(g_canonical), "%s/Documents/assembly-csharp-patch.bytes", home);
        snprintf(g_remote, sizeof(g_remote), "%s/Documents/remote_patch.bytes", home);
        snprintf(g_codepatch_dir, sizeof(g_codepatch_dir), "%s/Documents/codepatch", home);
        snprintf(g_codepatch, sizeof(g_codepatch), "%s/Documents/codepatch/assembly-csharp-patch.bytes", home);
        snprintf(g_logpath, sizeof(g_logpath), "%s/Documents/ffpatch_loader.log", home);
    }
    g_ready = true;
}

void ensure_init(void) {
    pthread_once(&g_once, ensure_init_once);
}

// ====================================================================
// Interposed entry points
// ====================================================================

int ffp_open(const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int); // read even if O_CREAT not set (harmless on arm64)
    va_end(ap);

    if (!g_ready) ensure_init();

    bool rd_only = ((flags & (O_WRONLY | O_RDWR)) == 0);
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_open_match, 1);
        if (rd_only && want_redirect(path)) {
            redir = true;
            use = g_canonical;
            __sync_fetch_and_add(&g_n_redirect, 1);
        }
        if (g_ready && g_logpath[0]) {
            ffp_log("open(\"%s\", 0x%x) -> %s%s", path, flags,
                    redir ? "REDIRECT " : "passthru ", redir ? g_canonical : "");
        }
    }

    if (!o_open) { errno = ENOSYS; return -1; }
    if (redir) return o_open(use, flags, (mode_t)mode);
    return o_open(path, flags, (mode_t)mode);
}

int ffp_openat(int dirfd, const char *path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);

    if (!g_ready) ensure_init();

    bool rd_only = ((flags & (O_WRONLY | O_RDWR)) == 0);
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_open_match, 1);
        // absolute or AT_FDCWD-relative: safe to redirect
        if (rd_only && want_redirect(path) && (path[0] == '/' || dirfd == AT_FDCWD)) {
            redir = true;
            use = g_canonical;
            __sync_fetch_and_add(&g_n_redirect, 1);
        }
        if (g_ready && g_logpath[0]) {
            ffp_log("openat(\"%s\", 0x%x) -> %s", path, flags, redir ? "REDIRECT" : "passthru");
        }
    }

    if (!o_openat) { errno = ENOSYS; return -1; }
    if (redir) return o_openat(dirfd, use, flags, (mode_t)mode);
    return o_openat(dirfd, path, flags, (mode_t)mode);
}

FILE *ffp_fopen(const char *path, const char *mode) {
    if (!g_ready) ensure_init();

    bool rd_only = (mode && mode[0] == 'r' && !strchr(mode, '+'));
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_open_match, 1);
        if (rd_only && want_redirect(path)) {
            redir = true;
            use = g_canonical;
            __sync_fetch_and_add(&g_n_redirect, 1);
        }
        if (g_ready && g_logpath[0]) {
            ffp_log("fopen(\"%s\", \"%s\") -> %s", path, mode ? mode : "?", redir ? "REDIRECT" : "passthru");
        }
    }

    if (!o_fopen) { errno = ENOSYS; return NULL; }
    return redir ? o_fopen(use, mode) : o_fopen(path, mode);
}

int ffp_access(const char *path, int mode) {
    if (!g_ready) ensure_init();
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_stat_match, 1);
        if (want_redirect(path)) { redir = true; use = g_canonical; __sync_fetch_and_add(&g_n_redirect, 1); }
        if (g_ready && g_logpath[0]) ffp_log("access(\"%s\") -> %s", path, redir ? "REDIRECT" : "passthru");
    }

    if (!o_access) { errno = ENOSYS; return -1; }
    return redir ? o_access(use, mode) : o_access(path, mode);
}

int ffp_faccessat(int dirfd, const char *path, int mode, int flags) {
    if (!g_ready) ensure_init();
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_stat_match, 1);
        if (want_redirect(path) && (path[0] == '/' || dirfd == AT_FDCWD)) { redir = true; use = g_canonical; __sync_fetch_and_add(&g_n_redirect, 1); }
        if (g_ready && g_logpath[0]) ffp_log("faccessat(\"%s\") -> %s", path, redir ? "REDIRECT" : "passthru");
    }

    if (!o_faccessat) { errno = ENOSYS; return -1; }
    return redir ? o_faccessat(dirfd, use, mode, flags) : o_faccessat(dirfd, path, mode, flags);
}

int ffp_stat(const char *path, struct stat *buf) {
    if (!g_ready) ensure_init();
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_stat_match, 1);
        if (want_redirect(path)) { redir = true; use = g_canonical; __sync_fetch_and_add(&g_n_redirect, 1); }
        if (g_ready && g_logpath[0]) ffp_log("stat(\"%s\") -> %s", path, redir ? "REDIRECT" : "passthru");
    }

    if (!o_stat) { errno = ENOSYS; return -1; }
    return redir ? o_stat(use, buf) : o_stat(path, buf);
}

int ffp_lstat(const char *path, struct stat *buf) {
    if (!g_ready) ensure_init();
    bool redir = false;
    const char *use = path;

    if (path && path_is_patch(path)) {
        __sync_fetch_and_add(&g_n_stat_match, 1);
        if (want_redirect(path)) { redir = true; use = g_canonical; __sync_fetch_and_add(&g_n_redirect, 1); }
        if (g_ready && g_logpath[0]) ffp_log("lstat(\"%s\") -> %s", path, redir ? "REDIRECT" : "passthru");
    }

    if (!o_lstat) { errno = ENOSYS; return -1; }
    return redir ? o_lstat(use, buf) : o_lstat(path, buf);
}

// ====================================================================
// dyld interpose section — the ORIGINAL libc symbol is in the pair;
// dyld routes every image's bind of that symbol to our replacement.
// ====================================================================

typedef struct ffp_interpose_pair {
    const void *replacement;
    const void *original;
} ffp_interpose_pair;

#define FFP_INTERPOSE_DECL(_var) \
    __attribute__((used)) static const ffp_interpose_pair _var \
    __attribute__((section("__DATA,__interpose")))

FFP_INTERPOSE_DECL(_ffp_ip_open)      = { (const void *)ffp_open,      (const void *)open };
FFP_INTERPOSE_DECL(_ffp_ip_openat)    = { (const void *)ffp_openat,    (const void *)openat };
FFP_INTERPOSE_DECL(_ffp_ip_fopen)     = { (const void *)ffp_fopen,     (const void *)fopen };
FFP_INTERPOSE_DECL(_ffp_ip_access)    = { (const void *)ffp_access,    (const void *)access };
FFP_INTERPOSE_DECL(_ffp_ip_faccessat) = { (const void *)ffp_faccessat, (const void *)faccessat };
FFP_INTERPOSE_DECL(_ffp_ip_stat)      = { (const void *)ffp_stat,      (const void *)stat };
FFP_INTERPOSE_DECL(_ffp_ip_lstat)     = { (const void *)ffp_lstat,     (const void *)lstat };

// ====================================================================
// Documents scan + patch file canonicalization
// ====================================================================

typedef struct cand {
    char path[1024];
    time_t mtime;
    off_t  size;
} cand_t;

static bool is_output_path(const char *p) {
    return (g_canonical[0]  && strcmp(p, g_canonical)  == 0) ||
           (g_remote[0]     && strcmp(p, g_remote)     == 0) ||
           (g_codepatch[0]  && strcmp(p, g_codepatch)  == 0) ||
           (g_logpath[0]    && strcmp(p, g_logpath)    == 0);
}

static void scan_dir_for_candidates(const char *dirpath, cand_t *best) {
    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (n[0] == '.') continue; // hidden files / . / ..
        if (!(ffp_cistr(n, "assembly-csharp-patch") || ffp_cistr(n, "remote_patch")))
            continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dirpath, n);
        if (is_output_path(full)) continue;

        // NB: must use the REAL stat — our own interposed stat would
        // redirect candidate source files onto the canonical copy.
        struct stat st;
        if (!o_stat || o_stat(full, &st) != 0) continue;
        if (!S_ISREG(st.st_mode) || st.st_size < 8) continue;

        if (!best->path[0] || st.st_mtime > best->mtime) {
            snprintf(best->path, sizeof(best->path), "%s", full);
            best->mtime = st.st_mtime;
            best->size  = st.st_size;
        }
    }
    closedir(d);
}

static bool copy_file(const char *src, const char *dst) {
    if (!o_open || !o_fopen) return false;
    FILE *in = o_fopen(src, "rb");
    if (!in) return false;
    FILE *out = o_fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return false; }
    }
    fclose(in);
    fclose(out);
    chmod(dst, 0644);
    return true;
}

static bool same_content(const char *a, const char *b) {
    struct stat sa, sb;
    if (!o_stat) return false;
    if (o_stat(a, &sa) != 0) return false;
    if (o_stat(b, &sb) != 0) return false;
    if (sa.st_size != sb.st_size || sa.st_mtime > sb.st_mtime) return false;
    return true;
}

// Find + install the patch. Returns true when the canonical file is ready.
static bool install_patch_from_documents(void) {
    cand_t best = {0};
    scan_dir_for_candidates(g_docs, &best);

    char inbox[1100];
    snprintf(inbox, sizeof(inbox), "%s/Inbox", g_docs);
    scan_dir_for_candidates(inbox, &best);

    if (!best.path[0]) {
        ffp_log("NO patch file found in %s — drop 'assembly-csharp-patch.bytes' there (Filza / Files app)", g_docs);
        return false;
    }

    // sniff magic
    unsigned char magic[8] = {0};
    FILE *f = o_fopen ? o_fopen(best.path, "rb") : NULL;
    if (f) { fread(magic, 1, 7, f); fclose(f); }
    bool is_unityfs = (memcmp(magic, "UnityFS", 7) == 0);
    ffp_log("patch source: %s (%lld bytes, mtime %lld, magic %s)",
            best.path, (long long)best.size, (long long)best.mtime,
            is_unityfs ? "UnityFS ✓" : (magic[0] == 0x1f && magic[1] == 0x8b ? "gzip(!)" : "unknown(!)"));

    if (!copy_file(best.path, g_canonical)) {
        ffp_log("FAILED to write canonical copy %s (errno=%d %s)", g_canonical, errno, strerror(errno));
        return false;
    }
    ffp_log("canonical copy OK -> %s", g_canonical);

    if (!copy_file(best.path, g_remote))
        ffp_log("warn: could not write %s (errno=%d)", g_remote, errno);
    else
        ffp_log("hot-update copy OK -> %s", g_remote);

    mkdir(g_codepatch_dir, 0755); // ignore EEXIST
    if (!copy_file(best.path, g_codepatch))
        ffp_log("warn: could not write %s (errno=%d)", g_codepatch, errno);
    else
        ffp_log("codepatch copy OK -> %s", g_codepatch);

    return true;
}

// ====================================================================
// Delayed pass — late-loaded images + summary
// ====================================================================

static void *delayed_thread(void *arg) {
    (void)arg;
    sleep(5);

    // late-dropped patch file? note it (still needs a restart to be loaded by IFix)
    cand_t best = {0};
    scan_dir_for_candidates(g_docs, &best);
    if (best.path[0] && g_canonical[0] && o_stat) {
        struct stat sc;
        if (o_stat(g_canonical, &sc) == 0 && best.mtime > sc.st_mtime)
            ffp_log("newer patch file appeared AFTER launch: %s — RESTART the app to apply", best.path);
    }

    ffp_rebind_all_gots();

    ffp_log("---- 5s summary: opens(patch)=%ld stats(patch)=%ld redirects=%ld ----",
            g_n_open_match, g_n_stat_match, g_n_redirect);
    if (g_n_open_match == 0 && g_n_stat_match == 0) {
        ffp_log("WARNING: game never touched any patch path within 5s — "
                "it may load the codepatch via Resources.Load (packed archive). "
                "Send this log back for a v2 approach.");
    }
    return NULL;
}

// ====================================================================
// Constructor — runs before main(), before Unity engine init
// ====================================================================

__attribute__((constructor))
static void ffp_bootstrap(void) {
    ensure_init();

    // start a fresh log each launch
    if (g_logpath[0] && o_fopen) {
        FILE *f = o_fopen(g_logpath, "w");
        if (f) { fprintf(f, "FFPatchLoader %s log\n", FFP_VERSION); fclose(f); }
    }

    ffp_log("=== FFPatchLoader %s — PPL-safe IFix codepatch loader ===", FFP_VERSION);
    ffp_log("HOME=%s", g_home);

    if (!o_open || !o_fopen) {
        ffp_log("FATAL: original libc fns unresolved — loader inactive");
        return;
    }

    bool ok = install_patch_from_documents();

    // interposition is already active (compile-time __interpose section);
    // GOT rebinding covers any pre-main binds interposition may have missed.
    ffp_rebind_all_gots();

    ffp_log("bootstrap done (patch=%s) — interpose active, GOT rebind attempted", ok ? "READY" : "MISSING");

    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, delayed_thread, NULL);
    pthread_attr_destroy(&a);
}
