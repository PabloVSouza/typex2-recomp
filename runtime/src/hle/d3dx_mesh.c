/* D3DX meshes: D3DXLoadMeshFromXInMemory and the ID3DXMesh it returns.
 *
 * The .X reader handles the binary encoding ("xof 0303bin 0032" / "0064"): a
 * stream of tokens where a data object is its template's name, an optional
 * object name and a brace-delimited body of integer lists, float lists,
 * strings, child objects and references. The body is kept as a flat list of
 * typed values, read back in the order of the standard templates (Mesh,
 * MeshNormals, MeshTextureCoords, MeshVertexColors, MeshMaterialList,
 * Material, TextureFilename, DeclData, Frame, FrameTransformMatrix);
 * template definitions and other objects are skipped.
 *
 * Like D3DX, the meshes of a file are merged into one, each moved by the
 * transforms of the frames around it; a vertex whose faces give it different
 * normals is split; polygons become triangle fans; the vertex layout is
 * position, normal, diffuse colour and texture coordinates for what the file
 * has, then the DeclData elements. The triangles are sorted by material and
 * the mesh carries the attribute table DrawSubset uses.
 *
 * The mesh's vertex and index buffers are the device's own, created and
 * locked through its COM interface, so they behave like any others. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../com.h"

u32 x2_d3dx_buffer(u32 size, u32 *data); /* d3dx9.c: an ID3DXBuffer */

/* realloc that stops the game when memory runs out (the arrays below only grow, never to 0) */
static void *grow(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        x2_log("d3dx9: out of memory loading a mesh (%zu bytes)", n);
        abort();
    }
    return q;
}

/* ---- binary .X tokens ----------------------------------------------------- */
enum {
    T_NAME = 1, T_STRING = 2, T_INTEGER = 3, T_GUID = 5, T_INTEGER_LIST = 6, T_FLOAT_LIST = 7,
    T_OBRACE = 10, T_CBRACE = 11, T_COMMA = 19, T_SEMICOLON = 20, T_TEMPLATE = 31,
};

typedef struct {
    const u8 *p, *end;
    bool f64; /* floats are doubles ("0064") */
    bool err;
} Rd;

typedef struct {
    int tok;
    const u8 *data; /* payload: name / string bytes, list items, guid */
    u32 n;          /* name / string length, list count */
} Tok;

static u32 rd_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

static bool need(Rd *r, size_t n) {
    if ((size_t)(r->end - r->p) < n) { r->err = true; return false; }
    return true;
}
/* the next token, or 0 at the end / on an error */
static int next_tok(Rd *r, Tok *t) {
    memset(t, 0, sizeof *t);
    if (r->err || r->p >= r->end || !need(r, 2)) return 0;
    t->tok = r->p[0] | r->p[1] << 8;
    r->p += 2;
    switch (t->tok) {
        case T_NAME:
        case T_STRING:
            if (!need(r, 4)) return 0;
            t->n = rd_u32(r->p);
            r->p += 4;
            if (!need(r, t->n + (t->tok == T_STRING ? 2u : 0u))) return 0;
            t->data = r->p;
            r->p += t->n + (t->tok == T_STRING ? 2 : 0); /* a string carries its terminator token */
            break;
        case T_INTEGER:
            if (!need(r, 4)) return 0;
            t->data = r->p, t->n = 1;
            r->p += 4;
            break;
        case T_GUID:
            if (!need(r, 16)) return 0;
            t->data = r->p;
            r->p += 16;
            break;
        case T_INTEGER_LIST:
        case T_FLOAT_LIST: {
            if (!need(r, 4)) return 0;
            t->n = rd_u32(r->p);
            r->p += 4;
            size_t sz = t->tok == T_FLOAT_LIST && r->f64 ? 8 : 4;
            if (t->n > 0x10000000u || !need(r, sz * t->n)) return 0;
            t->data = r->p;
            r->p += sz * t->n;
            break;
        }
        default: break; /* punctuation and type keywords carry nothing */
    }
    return t->tok;
}

/* ---- data objects ---------------------------------------------------------- */
typedef struct {
    bool is_float;
    union { u32 i; float f; };
} Val;

typedef struct XObj {
    char type[48], name[64];
    bool ref; /* a reference ({ name }) to an object defined elsewhere */
    Val *v;
    u32 nv, capv;
    char *str; /* the first string value (TextureFilename) */
    struct XObj **kids;
    u32 nkids, capkids;
} XObj;

static void obj_free(XObj *o) {
    if (!o) return;
    for (u32 i = 0; i < o->nkids; i++) obj_free(o->kids[i]);
    free(o->kids);
    free(o->v);
    free(o->str);
    free(o);
}
static void push_val(XObj *o, Val v) {
    if (o->nv == o->capv) {
        o->capv = o->capv ? o->capv * 2 : 64;
        o->v = grow(o->v, o->capv * sizeof *o->v);
    }
    o->v[o->nv++] = v;
}
static void push_kid(XObj *o, XObj *k) {
    if (o->nkids == o->capkids) {
        o->capkids = o->capkids ? o->capkids * 2 : 4;
        o->kids = grow(o->kids, o->capkids * sizeof *o->kids);
    }
    o->kids[o->nkids++] = k;
}
static void copy_name(char *dst, size_t cap, const Tok *t) {
    size_t n = t->n < cap - 1 ? t->n : cap - 1;
    memcpy(dst, t->data, n);
    dst[n] = 0;
}

/* a data object whose type name was just read: [name] { [guid] body } */
static XObj *parse_obj(Rd *r, const Tok *type, int depth) {
    if (depth > 64) { r->err = true; return NULL; }
    XObj *o = calloc(1, sizeof *o);
    copy_name(o->type, sizeof o->type, type);
    Tok t;
    int k = next_tok(r, &t);
    if (k == T_NAME) {
        copy_name(o->name, sizeof o->name, &t);
        k = next_tok(r, &t);
    }
    if (k != T_OBRACE) { r->err = true; return o; }
    for (;;) {
        k = next_tok(r, &t);
        if (!k || k == T_CBRACE) break;
        switch (k) {
            case T_GUID: case T_COMMA: case T_SEMICOLON: break;
            case T_INTEGER: push_val(o, (Val){.i = rd_u32(t.data)}); break;
            case T_INTEGER_LIST:
                for (u32 i = 0; i < t.n; i++) push_val(o, (Val){.i = rd_u32(t.data + 4 * i)});
                break;
            case T_FLOAT_LIST:
                for (u32 i = 0; i < t.n; i++) {
                    float f;
                    if (r->f64) {
                        double d;
                        memcpy(&d, t.data + 8 * i, 8);
                        f = (float)d;
                    } else memcpy(&f, t.data + 4 * i, 4);
                    push_val(o, (Val){.is_float = true, .f = f});
                }
                break;
            case T_STRING:
                if (!o->str) {
                    o->str = malloc(t.n + 1);
                    memcpy(o->str, t.data, t.n);
                    o->str[t.n] = 0;
                }
                break;
            case T_NAME: push_kid(o, parse_obj(r, &t, depth + 1)); break;
            case T_OBRACE: { /* a reference: { name [guid] } */
                XObj *ref = calloc(1, sizeof *ref);
                ref->ref = true;
                while ((k = next_tok(r, &t)) && k != T_CBRACE)
                    if (k == T_NAME) copy_name(ref->name, sizeof ref->name, &t);
                push_kid(o, ref);
                break;
            }
            default: break;
        }
        if (r->err) break;
    }
    return o;
}

/* the whole file: template definitions are skipped, data objects kept */
static XObj *parse_file(Rd *r) {
    XObj *root = calloc(1, sizeof *root);
    snprintf(root->type, sizeof root->type, "(file)");
    Tok t;
    int k;
    while ((k = next_tok(r, &t))) {
        if (k == T_TEMPLATE) {
            int depth = 0;
            while ((k = next_tok(r, &t))) {
                if (k == T_OBRACE) depth++;
                else if (k == T_CBRACE && --depth == 0) break;
            }
        } else if (k == T_NAME) push_kid(root, parse_obj(r, &t, 0));
        if (r->err) break;
    }
    return root;
}

/* ---- reading values in template order ------------------------------------------ */
typedef struct {
    const XObj *o;
    u32 at;
    bool short_; /* ran out of values */
} Cur;
static u32 ci(Cur *c) {
    if (c->at >= c->o->nv) { c->short_ = true; return 0; }
    Val v = c->o->v[c->at++];
    return v.is_float ? (u32)v.f : v.i;
}
static float cf(Cur *c) {
    if (c->at >= c->o->nv) { c->short_ = true; return 0; }
    Val v = c->o->v[c->at++];
    return v.is_float ? v.f : (float)v.i;
}

static const XObj *find_named(const XObj *o, const char *name) {
    if (o->name[0] && !o->ref && !strcmp(o->name, name)) return o;
    for (u32 i = 0; i < o->nkids; i++) {
        const XObj *r = find_named(o->kids[i], name);
        if (r) return r;
    }
    return NULL;
}
static const XObj *kid(const XObj *o, const char *type) {
    for (u32 i = 0; i < o->nkids; i++)
        if (!strcmp(o->kids[i]->type, type)) return o->kids[i];
    return NULL;
}

/* ---- the merged mesh ---------------------------------------------------------- */
typedef struct {
    float diffuse[4], power, specular[3], emissive[3];
    char *texture;
} Mat;

typedef struct {
    float pos[3], nrm[3], uv[2];
    u32 color;
    u32 src; /* index of its decl data row */
} Vtx;

typedef struct {
    Vtx *v;
    u32 nv, capv;
    u32 *tri; /* 3 per triangle */
    u32 *attr;
    u32 ntri, captri;
    Mat *mat;
    u32 nmat;
    bool has_nrm, has_uv, has_color;
    /* DeclData: elements (type, method, usage, index) and one row of dwords per source vertex */
    u32 decl[16][4], ndecl, decl_dwords;
    u32 *decl_rows;
    u32 nrows, caprows;
    bool decl_conflict;
} Build;

static u32 add_vtx(Build *b, Vtx v) {
    if (b->nv == b->capv) {
        b->capv = b->capv ? b->capv * 2 : 256;
        b->v = grow(b->v, b->capv * sizeof *b->v);
    }
    b->v[b->nv] = v;
    return b->nv++;
}
static void add_tri(Build *b, u32 a, u32 c1, u32 c2, u32 attr) {
    if (b->ntri == b->captri) {
        b->captri = b->captri ? b->captri * 2 : 256;
        b->tri = grow(b->tri, 3 * b->captri * sizeof *b->tri);
        b->attr = grow(b->attr, b->captri * sizeof *b->attr);
    }
    b->tri[3 * b->ntri] = a, b->tri[3 * b->ntri + 1] = c1, b->tri[3 * b->ntri + 2] = c2;
    b->attr[b->ntri++] = attr;
}

static u32 decl_type_size(u32 t) {
    static const u8 sz[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
    return t < sizeof sz ? sz[t] : 0;
}

static void read_material(const XObj *m, Mat *out) {
    Cur c = {m, 0, false};
    for (int i = 0; i < 4; i++) out->diffuse[i] = cf(&c);
    out->power = cf(&c);
    for (int i = 0; i < 3; i++) out->specular[i] = cf(&c);
    for (int i = 0; i < 3; i++) out->emissive[i] = cf(&c);
    const XObj *tf = kid(m, "TextureFilename");
    if (tf && tf->str) out->texture = strdup(tf->str);
}

static void xform_pt(const float *m, float *p) {
    float x = p[0], y = p[1], z = p[2];
    for (int k = 0; k < 3; k++) p[k] = x * m[k] + y * m[4 + k] + z * m[8 + k] + m[12 + k];
}
static void xform_n(const float *m, float *n) {
    float x = n[0], y = n[1], z = n[2];
    for (int k = 0; k < 3; k++) n[k] = x * m[k] + y * m[4 + k] + z * m[8 + k];
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0) n[0] /= l, n[1] /= l, n[2] /= l;
}

/* one Mesh object, moved by `world`, appended to the build */
static bool add_mesh(Build *b, const XObj *root, const XObj *mesh, const float *world) {
    Cur c = {mesh, 0, false};
    u32 nv = ci(&c);
    if (nv > 0x1000000u) return false;
    float *pos = malloc((size_t)(nv ? nv : 1) * 12);
    for (u32 i = 0; i < 3 * nv; i++) pos[i] = cf(&c);
    u32 nf = ci(&c);
    if (c.short_ || nf > 0x1000000u) { free(pos); return false; }
    u32 *fstart = malloc((size_t)(nf + 1) * 4), *fidx = NULL, nidx = 0, capidx = 0;
    for (u32 f = 0; f < nf; f++) {
        u32 n = ci(&c);
        fstart[f] = nidx;
        for (u32 j = 0; j < n && !c.short_; j++) {
            if (nidx == capidx) {
                capidx = capidx ? capidx * 2 : 1024;
                fidx = grow(fidx, capidx * 4);
            }
            u32 v = ci(&c);
            fidx[nidx++] = v < nv ? v : 0;
        }
    }
    fstart[nf] = nidx;
    bool ok = !c.short_;

    /* normals: their own array, indexed per face corner */
    const XObj *mn = kid(mesh, "MeshNormals");
    float *nrm = NULL;
    u32 nn = 0, *nidxs = NULL;
    if (mn) {
        Cur d = {mn, 0, false};
        nn = ci(&d);
        if (nn < 0x1000000u) {
            nrm = malloc((size_t)(nn ? nn : 1) * 12);
            for (u32 i = 0; i < 3 * nn; i++) nrm[i] = cf(&d);
            u32 nfn = ci(&d);
            nidxs = malloc((size_t)(nidx ? nidx : 1) * 4);
            for (u32 f = 0; f < nfn && f < nf; f++) {
                u32 n = ci(&d);
                for (u32 j = 0; j < n; j++) {
                    u32 k = ci(&d);
                    if (j < fstart[f + 1] - fstart[f]) nidxs[fstart[f] + j] = k < nn ? k : 0;
                }
            }
            if (d.short_ || nfn < nf) free(nrm), free(nidxs), nrm = NULL, nidxs = NULL;
        }
    }
    const XObj *mt = kid(mesh, "MeshTextureCoords");
    float *uv = NULL;
    if (mt) {
        Cur d = {mt, 0, false};
        u32 n = ci(&d);
        uv = calloc(nv ? nv : 1, 8);
        for (u32 i = 0; i < n; i++) {
            float u = cf(&d), v = cf(&d);
            if (i < nv) uv[2 * i] = u, uv[2 * i + 1] = v;
        }
    }
    const XObj *mc = kid(mesh, "MeshVertexColors");
    u32 *col = NULL;
    if (mc) {
        Cur d = {mc, 0, false};
        u32 n = ci(&d);
        col = malloc((size_t)(nv ? nv : 1) * 4);
        for (u32 i = 0; i < nv; i++) col[i] = 0xFFFFFFFFu;
        for (u32 i = 0; i < n && !d.short_; i++) {
            u32 vi = ci(&d);
            float rgba[4];
            for (int k = 0; k < 4; k++) {
                float x = cf(&d);
                rgba[k] = x < 0 ? 0 : x > 1 ? 1 : x;
            }
            if (vi < nv)
                col[vi] = (u32)(rgba[3] * 255 + 0.5f) << 24 | (u32)(rgba[0] * 255 + 0.5f) << 16 |
                          (u32)(rgba[1] * 255 + 0.5f) << 8 | (u32)(rgba[2] * 255 + 0.5f);
        }
    }
    /* DeclData: extra per-vertex elements; every mesh of the file must agree on them */
    const XObj *dd = kid(mesh, "DeclData");
    u32 row0 = b->nrows;
    if (dd) {
        Cur d = {dd, 0, false};
        u32 ne = ci(&d), el[16][4] = {{0}}, stride = 0;
        for (u32 i = 0; i < ne; i++)
            for (int k = 0; k < 4; k++) {
                u32 x = ci(&d);
                if (i < 16) el[i][k] = x;
            }
        if (ne > 16) ne = 16;
        for (u32 i = 0; i < ne; i++) stride += decl_type_size(el[i][0]);
        stride /= 4;
        if (b->ndecl == 0 && b->nrows == 0) {
            memcpy(b->decl, el, sizeof el);
            b->ndecl = ne;
            b->decl_dwords = stride;
        } else if (ne != b->ndecl || memcmp(el, b->decl, ne * sizeof el[0])) b->decl_conflict = true;
        u32 ndw = ci(&d);
        if (stride && stride == b->decl_dwords) {
            if (b->nrows + nv > b->caprows) {
                b->caprows = (b->nrows + nv) * 2;
                b->decl_rows = grow(b->decl_rows, (size_t)b->caprows * stride * 4);
            }
            for (u32 i = 0; i < nv * stride; i++) b->decl_rows[(size_t)row0 * stride + i] = i < ndw ? ci(&d) : 0;
            b->nrows += nv;
        }
    }
    if (b->ndecl && !dd) b->decl_conflict = true;

    /* materials: inline or references to top-level ones */
    u32 mat0 = b->nmat, nmats = 0, *fmat = NULL, nfm = 0;
    const XObj *ml = kid(mesh, "MeshMaterialList");
    if (ml) {
        Cur d = {ml, 0, false};
        nmats = ci(&d);
        nfm = ci(&d);
        if (nfm > nf) nfm = nf;
        fmat = malloc((size_t)(nfm ? nfm : 1) * 4);
        for (u32 i = 0; i < nfm; i++) fmat[i] = ci(&d);
        u32 got = 0;
        for (u32 i = 0; i < ml->nkids && got < nmats; i++) {
            const XObj *m = ml->kids[i];
            if (m->ref) m = find_named(root, m->name);
            if (!m || strcmp(m->type, "Material")) continue;
            b->mat = grow(b->mat, (b->nmat + 1) * sizeof *b->mat);
            memset(&b->mat[b->nmat], 0, sizeof *b->mat);
            read_material(m, &b->mat[b->nmat++]);
            got++;
        }
        for (; got < nmats; got++) { /* missing materials: plain white */
            b->mat = grow(b->mat, (b->nmat + 1) * sizeof *b->mat);
            memset(&b->mat[b->nmat], 0, sizeof *b->mat);
            for (int k = 0; k < 4; k++) b->mat[b->nmat].diffuse[k] = 1;
            b->nmat++;
        }
    }
    if (nmats == 0) { /* no material list: one default material */
        b->mat = grow(b->mat, (b->nmat + 1) * sizeof *b->mat);
        memset(&b->mat[b->nmat], 0, sizeof *b->mat);
        for (int k = 0; k < 4; k++) b->mat[b->nmat].diffuse[k] = 1;
        b->nmat++;
        nmats = 1;
    }

    if (nrm) b->has_nrm = true;
    if (uv) b->has_uv = true;
    if (col) b->has_color = true;
    /* the file's vertices first, then the copies a second normal needs */
    u32 base = b->nv;
    u32 *nrm_of = malloc((size_t)(nv ? nv : 1) * 4);
    for (u32 i = 0; i < nv; i++) {
        Vtx v = {0};
        memcpy(v.pos, pos + 3 * i, 12);
        xform_pt(world, v.pos);
        if (uv) v.uv[0] = uv[2 * i], v.uv[1] = uv[2 * i + 1];
        v.color = col ? col[i] : 0xFFFFFFFFu;
        v.src = dd ? row0 + i : 0;
        add_vtx(b, v);
        nrm_of[i] = 0xFFFFFFFFu;
    }
    typedef struct { u32 v, n, out; } Dup;
    Dup *dups = NULL;
    u32 ndup = 0;
    for (u32 f = 0; f < nf && ok; f++) {
        u32 s = fstart[f], n = fstart[f + 1] - s;
        u32 corner[64];
        if (n < 3) continue;
        if (n > 64) n = 64;
        for (u32 j = 0; j < n; j++) {
            u32 v = fidx[s + j], out = base + v;
            if (nrm) {
                u32 ni = nidxs[s + j];
                if (nrm_of[v] == 0xFFFFFFFFu) {
                    nrm_of[v] = ni;
                    memcpy(b->v[out].nrm, nrm + 3 * ni, 12);
                    xform_n(world, b->v[out].nrm);
                } else if (nrm_of[v] != ni) {
                    u32 k = 0;
                    while (k < ndup && !(dups[k].v == v && dups[k].n == ni)) k++;
                    if (k == ndup) {
                        Vtx copy = b->v[out];
                        memcpy(copy.nrm, nrm + 3 * ni, 12);
                        xform_n(world, copy.nrm);
                        dups = grow(dups, (ndup + 1) * sizeof *dups);
                        dups[ndup++] = (Dup){v, ni, add_vtx(b, copy)};
                    }
                    out = dups[k].out;
                }
            }
            corner[j] = out;
        }
        u32 m = nfm ? fmat[f < nfm ? f : nfm - 1] : 0; /* a short list repeats its last index */
        u32 attr = mat0 + (m < nmats ? m : 0);
        for (u32 j = 1; j + 1 < n; j++) add_tri(b, corner[0], corner[j], corner[j + 1], attr);
    }
    free(dups);
    free(nrm_of);
    free(fmat);
    free(col);
    free(uv);
    free(nrm);
    free(nidxs);
    free(fidx);
    free(fstart);
    free(pos);
    return ok;
}

static void mat_mul4(float *o, const float *a, const float *b) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            t[r * 4 + k] = a[r * 4] * b[k] + a[r * 4 + 1] * b[4 + k] + a[r * 4 + 2] * b[8 + k] + a[r * 4 + 3] * b[12 + k];
    memcpy(o, t, sizeof t);
}

/* every Mesh under `o`, with the frame transforms on the way down */
static bool walk(Build *b, const XObj *root, const XObj *o, const float *parent, int depth) {
    if (depth > 64) return false;
    float world[16];
    memcpy(world, parent, sizeof world);
    if (!strcmp(o->type, "Frame")) {
        const XObj *ft = kid(o, "FrameTransformMatrix");
        if (ft) {
            Cur c = {ft, 0, false};
            float local[16];
            for (int i = 0; i < 16; i++) local[i] = cf(&c);
            mat_mul4(world, local, parent);
        }
    }
    if (!strcmp(o->type, "Mesh") && !o->ref) return add_mesh(b, root, o, world);
    for (u32 i = 0; i < o->nkids; i++)
        if (!walk(b, root, o->kids[i], world, depth + 1)) return false;
    return true;
}

/* ---- ID3DXMesh ---------------------------------------------------------------- */
typedef struct {
    u32 attrib, face_start, face_count, vertex_start, vertex_count;
} AttrRange;

typedef struct {
    X2ComObj com;
    u32 dev, vb, ib, decl; /* guest COM objects */
    u32 options, fvf, stride, nfaces, nverts;
    u8 elems[65 * 8]; /* D3DVERTEXELEMENT9 list with its end marker */
    u32 nelems;
    u32 attr_buf; /* guest DWORD per face */
    AttrRange *table;
    u32 ntable;
} Mesh;

static u32 vt_mesh;

/* call method `idx` of guest COM object `obj` with `this` and the arguments (counted, up to 15) */
#define NARGS(...)           ((int)(sizeof((u32[]){__VA_ARGS__}) / sizeof(u32)))
#define VCALL(obj, idx, ...) x2_guest_call(c, RD32(RD32(obj) + 4 * (idx)), 1 + NARGS(__VA_ARGS__), obj, __VA_ARGS__)
#define VCALL0(obj, idx) x2_guest_call(c, RD32(RD32(obj) + 4 * (idx)), 1, obj)
enum { COM_ADDREF = 1, COM_RELEASE = 2 };
enum { DEV_CREATE_VB = 26, DEV_CREATE_IB = 27, DEV_DRAW_INDEXED = 82, DEV_CREATE_DECL = 86, DEV_SET_DECL = 87,
       DEV_SET_FVF = 89, DEV_SET_STREAM = 100, DEV_SET_INDICES = 104 };
enum { BUF_LOCK = 11, BUF_UNLOCK = 12 };

static void mesh_destroy(X2ComObj *o) {
    Mesh *m = (Mesh *)o;
    (void)m; /* the device objects are released by Release below (needs a cpu) */
    free(m->table);
    free(m);
}

static void M_Release(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    if (m->com.refs == 1) {
        if (m->vb) VCALL0(m->vb, COM_RELEASE);
        if (m->ib) VCALL0(m->ib, COM_RELEASE);
        if (m->decl) VCALL0(m->decl, COM_RELEASE);
        if (m->attr_buf) x2_heap_free(0, m->attr_buf);
        m->vb = m->ib = m->decl = m->attr_buf = 0;
    }
    x2_com_Release(c);
}
static void M_DrawSubset(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    u32 id = ARG(1);
    for (u32 i = 0; i < m->ntable; i++) {
        AttrRange *a = &m->table[i];
        if (a->attrib != id || !a->face_count) continue;
        VCALL(m->dev, DEV_SET_STREAM, 0, m->vb, 0, m->stride);
        VCALL(m->dev, DEV_SET_INDICES, m->ib);
        if (m->fvf)
            VCALL(m->dev, DEV_SET_FVF, m->fvf);
        else
            VCALL(m->dev, DEV_SET_DECL, m->decl);
        VCALL(m->dev, DEV_DRAW_INDEXED, 4 /* TRIANGLELIST */, 0, a->vertex_start, a->vertex_count, 3 * a->face_start,
              a->face_count);
    }
    RET(0);
}
static void M_GetNumFaces(X2Cpu *c) { RET(COM_THIS(Mesh)->nfaces); }
static void M_GetNumVertices(X2Cpu *c) { RET(COM_THIS(Mesh)->nverts); }
static void M_GetFVF(X2Cpu *c) { RET(COM_THIS(Mesh)->fvf); }
static void M_GetDeclaration(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    memcpy(X2P(ARG(1)), m->elems, (m->nelems + 1) * 8);
    RET(0);
}
static void M_GetNumBytesPerVertex(X2Cpu *c) { RET(COM_THIS(Mesh)->stride); }
static void M_GetOptions(X2Cpu *c) { RET(COM_THIS(Mesh)->options); }
static void M_GetDevice(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    VCALL0(m->dev, COM_ADDREF);
    WR32(ARG(1), m->dev);
    RET(0);
}
static void M_GetVertexBuffer(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    VCALL0(m->vb, COM_ADDREF);
    WR32(ARG(1), m->vb);
    RET(0);
}
static void M_GetIndexBuffer(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    VCALL0(m->ib, COM_ADDREF);
    WR32(ARG(1), m->ib);
    RET(0);
}
static void M_LockVertexBuffer(X2Cpu *c) { /* (flags, **data) */
    Mesh *m = COM_THIS(Mesh);
    RET(VCALL(m->vb, BUF_LOCK, 0, 0, ARG(2), ARG(1)));
}
static void M_UnlockVertexBuffer(X2Cpu *c) { RET(VCALL0(COM_THIS(Mesh)->vb, BUF_UNLOCK)); }
static void M_LockIndexBuffer(X2Cpu *c) {
    Mesh *m = COM_THIS(Mesh);
    RET(VCALL(m->ib, BUF_LOCK, 0, 0, ARG(2), ARG(1)));
}
static void M_UnlockIndexBuffer(X2Cpu *c) { RET(VCALL0(COM_THIS(Mesh)->ib, BUF_UNLOCK)); }
static void M_GetAttributeTable(X2Cpu *c) { /* (table, *size) */
    Mesh *m = COM_THIS(Mesh);
    if (ARG(1))
        for (u32 i = 0; i < m->ntable; i++) memcpy(X2P(ARG(1) + 20 * i), &m->table[i], 20);
    if (ARG(2)) WR32(ARG(2), m->ntable);
    RET(0);
}
static void M_LockAttributeBuffer(X2Cpu *c) {
    WR32(ARG(2), COM_THIS(Mesh)->attr_buf);
    RET(0);
}
static void M_UnlockAttributeBuffer(X2Cpu *c) { RET(0); }

static u32 mesh_vtable(void) {
    if (vt_mesh) return vt_mesh;
#define M(name, fn, n) {#name, fn, n}
#define U(name, n) {#name, NULL, n}
    static const X2Method m[] = {
        M(QueryInterface, x2_com_QueryInterface_self, 3), M(AddRef, x2_com_AddRef, 1), M(Release, M_Release, 1),
        M(DrawSubset, M_DrawSubset, 2), M(GetNumFaces, M_GetNumFaces, 1), M(GetNumVertices, M_GetNumVertices, 1),
        M(GetFVF, M_GetFVF, 1), M(GetDeclaration, M_GetDeclaration, 2),
        M(GetNumBytesPerVertex, M_GetNumBytesPerVertex, 1), M(GetOptions, M_GetOptions, 1),
        M(GetDevice, M_GetDevice, 2), U(CloneMeshFVF, 5), U(CloneMesh, 5),
        M(GetVertexBuffer, M_GetVertexBuffer, 2), M(GetIndexBuffer, M_GetIndexBuffer, 2),
        M(LockVertexBuffer, M_LockVertexBuffer, 3), M(UnlockVertexBuffer, M_UnlockVertexBuffer, 1),
        M(LockIndexBuffer, M_LockIndexBuffer, 3), M(UnlockIndexBuffer, M_UnlockIndexBuffer, 1),
        M(GetAttributeTable, M_GetAttributeTable, 3), U(ConvertPointRepsToAdjacency, 3),
        U(ConvertAdjacencyToPointReps, 3), U(GenerateAdjacency, 3), U(UpdateSemantics, 2),
        M(LockAttributeBuffer, M_LockAttributeBuffer, 3), M(UnlockAttributeBuffer, M_UnlockAttributeBuffer, 1),
        U(Optimize, 7), U(OptimizeInplace, 6), U(SetAttributeTable, 3),
    };
#undef M
#undef U
    vt_mesh = x2_com_vtable("ID3DXMesh", m, sizeof m / sizeof *m);
    return vt_mesh;
}

/* ---- building the mesh's device objects --------------------------------------------- */
static void put_elem(Mesh *m, u32 offset, u32 type, u32 usage, u32 index) {
    u8 *e = m->elems + 8 * m->nelems++;
    e[0] = 0, e[1] = 0;
    e[2] = (u8)offset, e[3] = (u8)(offset >> 8);
    e[4] = (u8)type, e[5] = 0, e[6] = (u8)usage, e[7] = (u8)index;
}

static int cmp_attr(const void *a, const void *b) {
    const u32 *x = a, *y = b; /* (attr, original position) */
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}

/* the device objects of the build; 0 or an HRESULT */
static u32 make_mesh(X2Cpu *c, Build *b, u32 options, u32 dev, u32 *out) {
    Mesh *m = calloc(1, sizeof *m);
    m->dev = dev, m->options = options, m->nfaces = b->ntri, m->nverts = b->nv;
    /* layout */
    u32 off = 0;
    put_elem(m, off, 2 /* FLOAT3 */, 0 /* POSITION */, 0), off += 12;
    if (b->has_nrm) put_elem(m, off, 2, 3 /* NORMAL */, 0), off += 12;
    if (b->has_color) put_elem(m, off, 4 /* D3DCOLOR */, 10 /* COLOR */, 0), off += 4;
    if (b->has_uv) put_elem(m, off, 1 /* FLOAT2 */, 5 /* TEXCOORD */, 0), off += 8;
    u32 std = off;
    if (b->ndecl && !b->decl_conflict)
        for (u32 i = 0; i < b->ndecl && m->nelems < 64; i++) {
            put_elem(m, off, b->decl[i][0], b->decl[i][2], b->decl[i][3]);
            off += decl_type_size(b->decl[i][0]);
        }
    static const u8 end[8] = {0xFF, 0, 0, 0, 17 /* UNUSED */, 0, 0, 0};
    memcpy(m->elems + 8 * m->nelems, end, 8);
    m->stride = off;
    m->fvf = off == std ? 0x002u | (b->has_nrm ? 0x010u : 0) | (b->has_color ? 0x040u : 0) | (b->has_uv ? 0x100u : 0)
                        : 0;

    /* triangles sorted by material, the attribute table from the runs */
    u32 (*order)[2] = malloc((size_t)(b->ntri ? b->ntri : 1) * sizeof *order);
    for (u32 i = 0; i < b->ntri; i++) order[i][0] = b->attr[i], order[i][1] = i;
    qsort(order, b->ntri, sizeof *order, cmp_attr);
    for (u32 i = 0; i < b->ntri;) {
        u32 j = i, lo = 0xFFFFFFFFu, hi = 0;
        while (j < b->ntri && order[j][0] == order[i][0]) {
            for (int k = 0; k < 3; k++) {
                u32 v = b->tri[3 * order[j][1] + k];
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
            j++;
        }
        m->table = grow(m->table, (m->ntable + 1) * sizeof *m->table);
        m->table[m->ntable++] = (AttrRange){order[i][0], i, j - i, lo, hi - lo + 1};
        i = j;
    }

    bool idx32 = (options & 1) || b->nv > 0xFFFF;
    u32 pool = (options & 0x220) ? 1 /* MANAGED */ : (options & 0x110) ? 2 /* SYSTEMMEM */ : 0;
    u32 vusage = (options & 0x200) ? 0x200u : 0, iusage = (options & 0x800) ? 0x200u : 0;
    u32 tmp = x2_heap_alloc(0, 16, true); /* out pointers */
    u32 hr = VCALL(dev, DEV_CREATE_VB, m->stride * (b->nv ? b->nv : 1), vusage, m->fvf, pool, tmp, 0);
    m->vb = RD32(tmp);
    if (!hr) {
        hr = VCALL(dev, DEV_CREATE_IB, 3 * (b->ntri ? b->ntri : 1) * (idx32 ? 4u : 2u), iusage,
                   idx32 ? 102u /* INDEX32 */ : 101u /* INDEX16 */, pool, tmp + 4, 0);
        m->ib = RD32(tmp + 4);
    }
    if (!hr) {
        u32 el = x2_heap_alloc(0, (m->nelems + 1) * 8, false);
        memcpy(X2P(el), m->elems, (m->nelems + 1) * 8);
        hr = VCALL(dev, DEV_CREATE_DECL, el, tmp + 8);
        m->decl = RD32(tmp + 8);
        x2_heap_free(0, el);
    }
    if (!hr && !VCALL(m->vb, BUF_LOCK, 0, 0, tmp + 12, 0)) {
        u8 *p = X2P(RD32(tmp + 12));
        for (u32 i = 0; i < b->nv; i++) {
            const Vtx *v = &b->v[i];
            u8 *d = p + (size_t)i * m->stride;
            memcpy(d, v->pos, 12), d += 12;
            if (b->has_nrm) memcpy(d, v->nrm, 12), d += 12;
            if (b->has_color) memcpy(d, &v->color, 4), d += 4;
            if (b->has_uv) memcpy(d, v->uv, 8), d += 8;
            if (m->stride > std) {
                if (v->src < b->nrows) memcpy(d, b->decl_rows + (size_t)v->src * b->decl_dwords, m->stride - std);
                else memset(d, 0, m->stride - std);
            }
        }
        VCALL0(m->vb, BUF_UNLOCK);
    }
    if (!hr && !VCALL(m->ib, BUF_LOCK, 0, 0, tmp + 12, 0)) {
        u8 *p = X2P(RD32(tmp + 12));
        for (u32 i = 0; i < b->ntri; i++)
            for (int k = 0; k < 3; k++) {
                u32 v = b->tri[3 * order[i][1] + k];
                if (idx32) memcpy(p + 4 * (3 * i + k), &v, 4);
                else {
                    u16 s = (u16)v;
                    memcpy(p + 2 * (3 * i + k), &s, 2);
                }
            }
        VCALL0(m->ib, BUF_UNLOCK);
    }
    x2_heap_free(0, tmp);
    if (!hr) {
        m->attr_buf = x2_heap_alloc(0, 4 * (b->ntri ? b->ntri : 1), true);
        for (u32 i = 0; i < b->ntri; i++) WR32(m->attr_buf + 4 * i, order[i][0]);
    }
    free(order);
    if (hr) {
        if (m->vb) VCALL0(m->vb, COM_RELEASE);
        if (m->ib) VCALL0(m->ib, COM_RELEASE);
        if (m->decl) VCALL0(m->decl, COM_RELEASE);
        free(m->table);
        free(m);
        return hr;
    }
    m->com.destroy = mesh_destroy;
    *out = x2_com_wrap(&m->com, mesh_vtable(), "ID3DXMesh");
    return 0;
}

/* the materials buffer: D3DXMATERIAL { D3DMATERIAL9, char *texture } then the names */
static u32 make_materials(const Build *b) {
    u32 names = 0;
    for (u32 i = 0; i < b->nmat; i++)
        if (b->mat[i].texture) names += (u32)strlen(b->mat[i].texture) + 1;
    u32 data, buf = x2_d3dx_buffer(72 * b->nmat + names, &data);
    u32 s = data + 72 * b->nmat;
    for (u32 i = 0; i < b->nmat; i++) {
        const Mat *m = &b->mat[i];
        u32 p = data + 72 * i;
        float v[17] = {m->diffuse[0], m->diffuse[1], m->diffuse[2], m->diffuse[3], 0, 0, 0, 1, /* ambient: none */
                       m->specular[0], m->specular[1], m->specular[2], 1, m->emissive[0], m->emissive[1],
                       m->emissive[2], 1, m->power};
        memcpy(X2P(p), v, 68);
        if (m->texture) {
            u32 n = (u32)strlen(m->texture) + 1;
            memcpy(X2P(s), m->texture, n);
            WR32(p + 68, s);
            s += n;
        } else WR32(p + 68, 0);
    }
    return buf;
}

static void build_free(Build *b) {
    for (u32 i = 0; i < b->nmat; i++) free(b->mat[i].texture);
    free(b->mat);
    free(b->v);
    free(b->tri);
    free(b->attr);
    free(b->decl_rows);
}

/* (pMemory, SizeOfMemory, Options, pD3DDevice, ppAdjacency, ppMaterials,
 *  ppEffectInstances, pNumMaterials, ppMesh) */
void x2_d3dx_load_mesh_x(X2Cpu *c) {
    const u8 *p = X2P(ARG(0));
    u32 n = ARG(1), options = ARG(2), dev = ARG(3);
    u32 pp_adj = ARG(4), pp_mat = ARG(5), pp_fx = ARG(6), p_nmat = ARG(7), pp_mesh = ARG(8);
    if (pp_mesh) WR32(pp_mesh, 0);
    if (n < 16 || memcmp(p, "xof ", 4) || memcmp(p + 8, "bin ", 4) ||
        (memcmp(p + 12, "0032", 4) && memcmp(p + 12, "0064", 4))) {
        x2_log("d3dx9: D3DXLoadMeshFromXInMemory: unsupported file (%.16s)", n >= 16 ? (const char *)p : "");
        RET(0x88760B59u); /* D3DXERR_INVALIDDATA */
        return;
    }
    Rd r = {p + 16, p + n, !memcmp(p + 12, "0064", 4), false};
    XObj *root = parse_file(&r);
    Build b = {0};
    float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool ok = !r.err && walk(&b, root, root, id, 0) && b.ntri;
    obj_free(root);
    if (!ok) {
        x2_log("d3dx9: D3DXLoadMeshFromXInMemory: %s (%u bytes)", r.err ? "malformed file" : "no mesh", n);
        build_free(&b);
        RET(0x88760B59u);
        return;
    }
    if (b.decl_conflict) x2_log("d3dx9: D3DXLoadMeshFromXInMemory: meshes with different DeclData, extras dropped");
    u32 mesh = 0, hr = make_mesh(c, &b, options, dev, &mesh);
    if (!hr) {
        WR32(pp_mesh, mesh);
        if (p_nmat) WR32(p_nmat, b.nmat);
        if (pp_mat) WR32(pp_mat, make_materials(&b));
        if (pp_fx) { /* D3DXEFFECTINSTANCE per material: no effect file, no defaults */
            u32 data, buf = x2_d3dx_buffer(12 * b.nmat, &data);
            memset(X2P(data), 0, 12 * b.nmat);
            WR32(pp_fx, buf);
        }
        if (pp_adj) { /* no adjacency information: every edge open */
            u32 data, buf = x2_d3dx_buffer(12 * b.ntri, &data);
            memset(X2P(data), 0xFF, 12 * b.ntri);
            WR32(pp_adj, buf);
        }
        X2_TRACE("d3dx9: mesh %u vertices, %u faces, %u materials, stride %u, fvf %x", b.nv, b.ntri, b.nmat,
                 ((Mesh *)x2_com_get(mesh))->stride, ((Mesh *)x2_com_get(mesh))->fvf);
    }
    build_free(&b);
    RET(hr);
}

