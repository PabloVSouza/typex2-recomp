/* shadertest: translate a collection of D3D shaders offline.
 *
 *   shadertest <glsl|essl|msl> <tok dir> <out dir>
 *
 * <tok dir> holds shaders saved by a game run with X2_SHADER_DUMP=<dir>
 * (<hash>.vs.tok / <hash>.ps.tok: D3D9 tokens, or x2's HLSL blob). Each is
 * translated and written to <out dir>/<hash>.<vs|ps>.<ext>; comparing two out
 * dirs shows what a translator change did to every shader of every game.
 * msl (macOS): every result is also compiled by the Metal compiler, pixel
 * shaders with and without the alpha test.
 * The tokens are game data: keep them out of the repository. */
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/src/gfx/sm3_glsl.h"

#ifdef __APPLE__
bool shadertest_metal_compile(const char *src, bool atest, char *err, size_t errlen);
#endif

static bool is_hlsl(const uint32_t *tok, uint32_t n) { return n > 3 && (tok[1] & 0xFFFF) == 0xFFFE && tok[2] == 0x4C484258u; }

int main(int argc, char **argv) {
    ShTarget target;
    if (argc == 4 && !strcmp(argv[1], "glsl")) target = SH_GLSL;
    else if (argc == 4 && !strcmp(argv[1], "essl")) target = SH_ESSL;
    else if (argc == 4 && !strcmp(argv[1], "msl")) target = SH_MSL;
    else {
        fprintf(stderr, "usage: shadertest <glsl|essl|msl> <tok dir> <out dir>\n");
        return 2;
    }
    DIR *d = opendir(argv[2]);
    if (!d) {
        perror(argv[2]);
        return 1;
    }
    int total = 0, incomplete = 0, rejected = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 8 || strcmp(e->d_name + len - 4, ".tok")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[2], e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint32_t *tok = malloc((size_t)size + 4);
        size_t got = fread(tok, 1, (size_t)size, f);
        fclose(f);
        uint32_t n = (uint32_t)(got / 4);
        bool pixel = strstr(e->d_name, ".ps.") != NULL;
        total++;
        char *out = NULL, err[512] = "";
        Sm3Info info;
        memset(&info, 0, sizeof info);
        bool ok;
        if (is_hlsl(tok, n)) {
            ok = hlsl_translate((const char *)&tok[11], (const char *)&tok[3], pixel, target, &out, &info, err,
                                sizeof err);
        } else {
            ok = sm3_translate(tok, n, target, &out, &info, err, sizeof err);
        }
        if (!ok) {
            incomplete++;
            fprintf(stderr, "%s: %s\n", e->d_name, err);
        }
#ifdef __APPLE__
        if (target == SH_MSL && out) {
            for (int atest = 0; atest <= (pixel ? 1 : 0); atest++) {
                if (!shadertest_metal_compile(out, atest, err, sizeof err)) {
                    rejected++;
                    fprintf(stderr, "%s%s: Metal: %s\n", e->d_name, atest ? " (X2_ATEST)" : "", err);
                    break;
                }
            }
        }
#endif
        snprintf(path, sizeof path, "%s/%.*s.%s", argv[3], (int)(len - 4), e->d_name, argv[1]);
        if (out && (f = fopen(path, "wb"))) {
            fputs(out, f);
            fclose(f);
        }
        free(out);
        free(tok);
    }
    closedir(d);
    printf("%d shaders, %d incomplete%s", total, incomplete, target == SH_MSL ? "" : "\n");
    if (target == SH_MSL) printf(", %d rejected by Metal\n", rejected);
    return incomplete || rejected ? 1 : 0;
}
