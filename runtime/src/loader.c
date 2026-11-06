/* Maps the original game.exe into guest memory and binds its imports to
 * host implementations. The recompiled code is matched to the executable by
 * image base/size/entry (from the generated x2_image). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core.h"


X2Module x2_module;

static u32 rd32(const u8 *f, size_t n, size_t off) {
    if (off + 4 > n) x2_fatal("%s: truncated PE header", x2_module.exe_path);
    u32 v;
    memcpy(&v, f + off, 4);
    return v;
}
static u16 rd16(const u8 *f, size_t n, size_t off) {
    if (off + 2 > n) x2_fatal("%s: truncated PE header", x2_module.exe_path);
    u16 v;
    memcpy(&v, f + off, 2);
    return v;
}

void x2_load_image(const char *exe_path) {
    X2Module *m = &x2_module;
    snprintf(m->exe_path, sizeof m->exe_path, "%s", exe_path);
    snprintf(m->game_dir, sizeof m->game_dir, "%s", exe_path);
    char *slash = strrchr(m->game_dir, '/');
    if (slash) *slash = 0;
    else strcpy(m->game_dir, ".");

    FILE *fp = fopen(exe_path, "rb");
    if (!fp) x2_fatal("cannot open %s: %s", exe_path, strerror(errno));
    fseek(fp, 0, SEEK_END);
    size_t n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    u8 *f = malloc(n);
    if (fread(f, 1, n, fp) != n) x2_fatal("cannot read %s", exe_path);
    fclose(fp);

    if (rd16(f, n, 0) != 0x5A4D) x2_fatal("%s: not an MZ executable", exe_path);
    u32 nt = rd32(f, n, 0x3C);
    if (rd32(f, n, nt) != 0x4550) x2_fatal("%s: not a PE executable", exe_path);
    u32 fh = nt + 4, oh = fh + 20;
    u16 nsec = rd16(f, n, fh + 2), opt_size = rd16(f, n, fh + 16);
    m->entry = rd32(f, n, oh + 16);
    m->base = rd32(f, n, oh + 28);
    m->size = rd32(f, n, oh + 56);
    m->entry += m->base;
    u32 hdr_size = rd32(f, n, oh + 60);
    m->stack_reserve = rd32(f, n, oh + 72);
    u32 tls_rva = rd32(f, n, oh + 96 + 9 * 8);

    if (m->base != x2_game->image->base || m->size != x2_game->image->size || m->entry != x2_game->image->entry)
        x2_fatal("%s does not match the recompiled image (base %08x/%08x size %x/%x entry %08x/%08x)", exe_path, m->base,
                 x2_game->image->base, m->size, x2_game->image->size, m->entry, x2_game->image->entry);

    if (x2_vm_alloc(m->base, m->size, "image") != m->base) x2_fatal("cannot map image at %08x", m->base);
    memcpy(X2P(m->base), f, hdr_size < n ? hdr_size : n);
    u32 sh = oh + opt_size;
    for (u16 i = 0; i < nsec; i++, sh += 40) {
        u32 vsize = rd32(f, n, sh + 8), rva = rd32(f, n, sh + 12);
        u32 raw_size = rd32(f, n, sh + 16), raw_off = rd32(f, n, sh + 20);
        u32 len = raw_size < vsize || !vsize ? raw_size : vsize;
        if (raw_off + len > n) len = raw_off < n ? (u32)(n - raw_off) : 0;
        memcpy(X2P(m->base + rva), f + raw_off, len);
    }
    free(f);

    if (tls_rva) {
        u32 t = m->base + tls_rva;
        m->tls_start = RD32(t);
        m->tls_end = RD32(t + 4);
        m->tls_index_addr = RD32(t + 8);
        m->tls_callbacks = RD32(t + 12);
        m->tls_zero = RD32(t + 16);
        if (m->tls_index_addr) WR32(m->tls_index_addr, 0);
    }
    x2_log("loaded %s: base %08x size %x entry %08x", exe_path, m->base, m->size, m->entry);
}

void x2_resolve_imports(void) {
    unsigned missing = 0;
    for (unsigned i = 0; i < x2_game->nimports; i++) {
        const X2ImportEntry *e = &x2_game->imports[i];
        const X2Hle *h = x2_hle_find(e->dll, e->name);
        char full[256];
        snprintf(full, sizeof full, "%s!%s", e->dll, e->name);
        u32 t = x2_thunk_new(strdup(full), h ? h->fn : NULL, h ? h->nargs : 0);
        WR32(e->slot, t);
        if (!h) {
            missing++;
            X2_TRACE("unimplemented import %s", full);
        }
    }
    x2_log("bound %u imports (%u unimplemented)", x2_game->nimports, missing);
}

static const X2HleModule hle_modules[] = {
    {"kernel32.dll", x2_hle_kernel32}, {"kernel32.dll", x2_hle_k32_sync}, {"kernel32.dll", x2_hle_k32_file},
    {"user32.dll", x2_hle_user32},     {"gdi32.dll", x2_hle_gdi32},
    {"advapi32.dll", x2_hle_advapi32}, {"ole32.dll", x2_hle_ole32},     {"winmm.dll", x2_hle_winmm},
    {"shlwapi.dll", x2_hle_shlwapi},   {"d3d9.dll", x2_hle_d3d9},       {"d3dx9_35.dll", x2_hle_d3dx9},
    {"d3dx9_37.dll", x2_hle_d3dx9},    {"shell32.dll", x2_hle_shell32}, {"imm32.dll", x2_hle_imm32},
};

const X2Hle *x2_hle_find(const char *dll, const char *name) {
    for (size_t i = 0; i < sizeof hle_modules / sizeof *hle_modules; i++) {
        if (strcasecmp(hle_modules[i].dll, dll)) continue;
        for (const X2Hle *h = hle_modules[i].funcs; h->name; h++)
            if (!strcmp(h->name, name)) return h;
    }
    return NULL;
}
