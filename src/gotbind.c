//
//  gotbind.c — __DATA_CONST GOT rebinding (PPL-safe fallback)
//
//  Why this exists:
//    dyld interposition normally catches every bind of `open`/`fopen`/`stat`/...
//    (interpose entries are registered before fixups are applied to any image,
//     regardless of LC_LOAD_DYLIB order). But if UnityFramework was already
//    bound by the time we registered — or if a future dyld/OS change breaks
//    interpose timing — its GOT slots still point straight at libSystem.
//
//    GOT slots live in __DATA_CONST — regular data pages, NOT sealed by PPL
//    (PPL only guards executable pages). Flipping them RW with vm_protect,
//    writing our function pointer, and restoring RO is allowed even on
//    iOS 26/27 TrollStore devices where __TEXT patching is impossible.
//
//    A slot is only rewritten when it still points at the REAL libSystem
//    function (i.e., interposition missed it). Slots that already point at
//    our interposer are left alone.
//

#include "ffp_internal.h"
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <sys/syslog.h>

// rebind table: symbol name, our replacement, resolved real address
typedef struct rebind_sym {
    const char *name;
    void       *replacement;
    void       *real;      // resolved via dlsym(RTLD_NEXT, name)
    long        patched;   // slots rewritten (stats)
} rebind_sym_t;

static rebind_sym_t g_syms[] = {
    { "open",      (void *)ffp_open,      NULL, 0 },
    { "openat",    (void *)ffp_openat,    NULL, 0 },
    { "fopen",     (void *)ffp_fopen,     NULL, 0 },
    { "access",    (void *)ffp_access,    NULL, 0 },
    { "faccessat", (void *)ffp_faccessat, NULL, 0 },
    { "stat",      (void *)ffp_stat,      NULL, 0 },
    { "lstat",     (void *)ffp_lstat,     NULL, 0 },
};
static const size_t g_nsyms = sizeof(g_syms) / sizeof(g_syms[0]);
static bool g_syms_resolved = false;

static pthread_mutex_t g_bindmtx = PTHREAD_MUTEX_INITIALIZER;

static void resolve_syms(void) {
    for (size_t i = 0; i < g_nsyms; i++) {
        void *p = dlsym(RTLD_NEXT, g_syms[i].name);
        if (p == g_syms[i].replacement) {
            // would loop forever — refuse
            g_syms[i].real = NULL;
            ffp_log("GOT rebind: dlsym(%s) resolved to OUR interposer — skipping",
                    g_syms[i].name);
            continue;
        }
        g_syms[i].real = p;
    }
    g_syms_resolved = true;
}

// ---- page protection helpers (raw mach trap — no mach_vm.h needed) ----

static bool page_set_protection(void *addr, bool writable) {
    uintptr_t page = (uintptr_t)addr & ~((uintptr_t)getpagesize() - 1);
    vm_prot_t prot = writable ? (VM_PROT_READ | VM_PROT_WRITE) : VM_PROT_READ;
    kern_return_t kr = custom_mach_vm_protect(mach_task_self_,
                                              (vm_address_t)page,
                                              (vm_size_t)getpagesize(),
                                              0, prot);
    if (kr != KERN_SUCCESS && writable) {
        // retry with VM_PROT_COPY (COW break — ElleKit's trick)
        kr = custom_mach_vm_protect(mach_task_self_,
                                    (vm_address_t)page,
                                    (vm_size_t)getpagesize(),
                                    0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    }
    return kr == KERN_SUCCESS;
}

// ---- section scanning ----

static bool is_pointer_section(const section_64 *sect) {
    // __got (chained-fixup GOT) / __nl_symbol_ptr / __la_symbol_ptr
    if (strcmp(sect->sectname, "__got") == 0 ||
        strcmp(sect->sectname, "__nl_symbol_ptr") == 0 ||
        strcmp(sect->sectname, "__la_symbol_ptr") == 0)
        return true;
    // or any section typed as pointer pointers
    uint32_t type = sect->flags & SECTION_TYPE;
    return (type == S_NON_LAZY_POINTER || type == S_LAZY_SYMBOL_POINTERS);
}

static void scan_section(const section_64 *sect, intptr_t slide,
                         const char *image_name, bool needs_flip,
                         long *out_flips_failed) {
    if (sect->size == 0 || (sect->size % 8) != 0) return;
    if (!is_pointer_section(sect)) return;

    void **base = (void **)(sect->addr + slide);
    size_t count = sect->size / 8;

    for (size_t i = 0; i < count; i++) {
        void *val = base[i];
        if (val == NULL) continue;

        for (size_t s = 0; s < g_nsyms; s++) {
            if (g_syms[s].real == NULL) continue;
            if (val != g_syms[s].real) continue; // already interposed → skip

            // hit — flip page RW, write, restore RO
            if (needs_flip) {
                if (!page_set_protection(&base[i], true)) {
                    (*out_flips_failed)++;
                    ffp_log("GOT rebind: RW flip FAILED for %s slot %p (PPL?)",
                            g_syms[s].name, (void *)&base[i]);
                    continue;
                }
                base[i] = g_syms[s].replacement;
                page_set_protection(&base[i], false);
            } else {
                base[i] = g_syms[s].replacement;
            }
            g_syms[s].patched++;
            ffp_log("GOT rebind: %s in %s @ %p -> ffp_%s",
                    g_syms[s].name, image_name, (void *)&base[i], g_syms[s].name);
        }
    }
}

static void rebind_image(const struct mach_header_64 *mh, intptr_t slide,
                         const char *image_name) {
    if (mh == NULL || mh->magic != MH_MAGIC_64) return;
    if ((uintptr_t)mh == (uintptr_t)&_mh_dylib_header) return; // our own dylib

    if (mh->sizeofcmds == 0 || mh->ncmds > 4096) return;

    const uint8_t *p = (const uint8_t *)mh + sizeof(struct mach_header_64);
    const uint8_t *end = p + mh->sizeofcmds;

    for (uint32_t c = 0; c < mh->ncmds && p + sizeof(struct load_command) <= end; c++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmdsize < sizeof(struct load_command) || (lc->cmdsize % 8) != 0) break;
        if (p + lc->cmdsize > end) break;

        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            const segment_command_64 *seg = (const segment_command_64 *)p;
            bool is_data_const = (strncmp(seg->segname, "__DATA_CONST", 16) == 0);
            bool is_data       = (strncmp(seg->segname, "__DATA", 16) == 0);

            if (is_data_const || is_data) {
                const section_64 *sect =
                    (const section_64 *)(p + sizeof(segment_command_64));
                uint32_t nsects = seg->nsects;
                if ((sizeof(segment_command_64) + (size_t)nsects * sizeof(section_64)) <= lc->cmdsize) {
                    long flips_failed = 0;
                    for (uint32_t s = 0; s < nsects; s++) {
                        scan_section(&sect[s], slide, image_name, is_data_const,
                                     &flips_failed);
                    }
                    if (flips_failed > 0) {
                        ffp_log("GOT rebind: %s: %ld slots skipped (protection flip refused)",
                                image_name, flips_failed);
                    }
                }
            }
        }
        p += lc->cmdsize;
    }
}

static bool is_system_image(const char *name) {
    if (!name || !*name) return true;
    if (strncmp(name, "/usr/lib/", 9) == 0) return true;
    if (strncmp(name, "/System/", 8) == 0) return true;
    if (strncmp(name, "/Developer/", 11) == 0) return true;
    if (strncmp(name, "/usr/share/", 11) == 0) return true;
    if (strncmp(name, "/private/var/db/", 17) == 0) return true;
    return false;
}

void ffp_rebind_all_gots(void) {
    pthread_mutex_lock(&g_bindmtx);
    if (!g_syms_resolved) resolve_syms();

    uint32_t count = _dyld_image_count();

    for (uint32_t i = 0; i < count; i++) {
        const struct mach_header *mh = _dyld_get_image_header(i);
        const char *name = _dyld_get_image_name(i);
        if (is_system_image(name)) continue; // shared cache: sealed & irrelevant
        intptr_t slide = _dyld_get_image_vmaddr_slide(i); // deprecated but stable
        rebind_image((const struct mach_header_64 *)mh, slide, name ? name : "?");
    }

    // report totals when they change
    static long reported[8];
    bool changed = false;
    for (size_t s = 0; s < g_nsyms && s < 8; s++) {
        if (g_syms[s].patched != reported[s]) {
            changed = true;
            reported[s] = g_syms[s].patched;
        }
    }
    pthread_mutex_unlock(&g_bindmtx);

    if (changed) {
        char buf[512];
        size_t off = 0;
        off += snprintf(buf + off, sizeof(buf) - off, "GOT rebind totals:");
        for (size_t s = 0; s < g_nsyms && off + 24 < sizeof(buf); s++) {
            off += snprintf(buf + off, sizeof(buf) - off, " %s=%ld",
                            g_syms[s].name, g_syms[s].patched);
        }
        ffp_log("%s", buf);
    }
}
