//
//  ffp_internal.h — shared declarations for FFPatchLoader
//
//  FFPatchLoader: PPL-safe IFix codepatch loader for Unity games (iOS 14 → 27)
//  NO __TEXT writes. NO inline hooking. NO memory patching of game code.
//  Two PPL-safe mechanisms only:
//    1) dyld __interpose section  (compile-time, honored at launch binding)
//    2) __DATA_CONST GOT rebinding (regular data pages — not PPL sealed)
//

#ifndef FFP_INTERNAL_H
#define FFP_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/mach_init.h>

#define FFP_VERSION "v1.0.0-documents-loader"

// ---- canonical paths (built from $HOME in ensure_init) ----
extern char g_home[1024];          // <container>
extern char g_docs[1024];          // <container>/Documents
extern char g_canonical[1024];     // <container>/Documents/assembly-csharp-patch.bytes
extern char g_remote[1024];        // <container>/Documents/remote_patch.bytes
extern char g_codepatch_dir[1024]; // <container>/Documents/codepatch
extern char g_codepatch[1024];     // <container>/Documents/codepatch/assembly-csharp-patch.bytes
extern char g_logpath[1024];       // <container>/Documents/ffpatch_loader.log
extern bool g_ready;               // ensure_init done

// ---- original (real) libc function pointers ----
typedef int    (*open_fn)     (const char *, int, ...);
typedef int    (*openat_fn)   (int, const char *, int, ...);
typedef FILE * (*fopen_fn)    (const char *, const char *);
typedef int    (*access_fn)   (const char *, int);
typedef int    (*faccessat_fn)(int, const char *, int, int);
typedef int    (*stat_fn)     (const char *, struct stat *);
typedef int    (*lstat_fn)    (const char *, struct stat *);

extern open_fn      o_open;
extern openat_fn    o_openat;
extern fopen_fn     o_fopen;
extern access_fn    o_access;
extern faccessat_fn o_faccessat;
extern stat_fn      o_stat;
extern lstat_fn     o_lstat;

// ---- interposed entry points (exported — referenced by __interpose pairs) ----
int    ffp_open(const char *path, int flags, ...);
int    ffp_openat(int dirfd, const char *path, int flags, ...);
FILE * ffp_fopen(const char *path, const char *mode);
int    ffp_access(const char *path, int mode);
int    ffp_faccessat(int dirfd, const char *path, int mode, int flags);
int    ffp_stat(const char *path, struct stat *buf);
int    ffp_lstat(const char *path, struct stat *buf);

// ---- helpers (ffpatch.c) ----
void   ensure_init(void);
void   ffp_log(const char *fmt, ...);
bool   ffp_cistr(const char *hay, const char *needle); // case-insensitive substring
bool   path_is_patch(const char *p);

// raw mach trap (defined in ffpatch.c, used by gotbind.c) — mach_vm_protect = trap -14
kern_return_t custom_mach_vm_protect(mach_port_name_t target,
                                     vm_address_t address,
                                     vm_size_t size,
                                     boolean_t set_maximum,
                                     vm_prot_t new_protection);

// ---- GOT rebinding (gotbind.c) ----
void ffp_rebind_all_gots(void);

#endif /* FFP_INTERNAL_H */
