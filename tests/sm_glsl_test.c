/* The GLSL shader translator (runtime/portable/sm_glsl.c) over a folder of D3D9 shaders: each
 * .vs, .ps or .bin file of raw tokens becomes a .vert or .frag in the output folder, which
 * tools/sm_glsl_test.sh compiles to SPIR-V with glslangValidator. Prints the translation failures.
 *
 *   build/sm_glsl_test <folder> <out folder> [--opts]
 *
 * --opts also writes each shader with the options a draw can ask for, named <name>.opt.vert and
 * <name>.opt.frag: alpha test, every 2D sampler compared (shadow) and, in a second pixel variant,
 * read as depth values, every vertex input fed as an integer kind in turn, and an extra varying.
 * The pixel shaders' inputs are checked against the varying locations, and the vertex shaders'
 * outputs too, so that any pair links.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_glsl.h"

static char g_out[1 << 20];

static int write_file(const char* dir, const char* name, const char* ext, const char* text, size_t len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    FILE* f = fopen(path, "w");
    if (!f)
        return perror(path), 0;
    fwrite(text, 1, len, f);
    fclose(f);
    return 1;
}

static int translate(const uint32_t* tok, size_t nt, const SmOpts* o, const char* dir, const char* name,
    const char* variant, SmInfo* info)
{
    char err[256], ext[32];
    size_t len = sm_glsl_translate(tok, nt, o, info, g_out, sizeof g_out, err, sizeof err);
    if (!len)
    {
        printf("FAIL %s%s: %s\n", name, variant, err);
        return 0;
    }
    snprintf(ext, sizeof ext, "%s.%s", variant, info->pixel ? "frag" : "vert");
    return write_file(dir, name, ext, g_out, len);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <shader folder> <out folder> [--opts]\n", argv[0]);
        return 2;
    }
    int opts = argc > 3 && !strcmp(argv[3], "--opts");
    DIR* d = opendir(argv[1]);
    if (!d)
        return perror(argv[1]), 2;
    struct dirent* e;
    unsigned ok = 0, bad = 0, nvs = 0, nps = 0;
    while ((e = readdir(d)))
    {
        size_t n = strlen(e->d_name);
        if (n < 4 || (strcmp(e->d_name + n - 3, ".vs") && strcmp(e->d_name + n - 3, ".ps") &&
                         strcmp(e->d_name + n - 4, ".bin")))
            continue;
        char path[1024], name[256];
        snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        snprintf(name, sizeof name, "%s", e->d_name);
        *strrchr(name, '.') = 0;
        FILE* f = fopen(path, "rb");
        if (!f)
            continue;
        static uint32_t tok[1 << 18];
        size_t nt = fread(tok, 4, sizeof tok / 4, f);
        fclose(f);
        SmInfo info;
        SmOpts o = { 0 };
        int good = translate(tok, nt, &o, argv[2], name, "", &info);
        if (good)
        {
            *(info.pixel ? &nps : &nvs) += 1;
            /* the interface: every varying has a location */
            const SmDecl* list = info.pixel ? info.in : info.out;
            unsigned nl = info.pixel ? info.nin : info.nout;
            for (unsigned i = 0; i < nl; ++i)
            {
                const SmDecl* s = &list[i];
                int builtin = (s->usage == SM_POSITION && s->index == 0) || s->usage == SM_PSIZE;
                if (!builtin && sm_glsl_location(s->usage, s->index) < 0)
                    printf("FAIL %s: semantic %u/%u has no location\n", name, s->usage, s->index), good = 0;
            }
        }
        if (good && opts)
        {
            SmInfo vi;
            memset(&o, 0, sizeof o);
            if (info.pixel)
            {
                o.alpha_func = 5;
                for (unsigned s = 0; s < 16; ++s)
                    if (info.sampler[s] == 2)
                        o.shadow_mask |= (uint16_t)(1u << s);
                o.no_depth_out = 1;
                good &= translate(tok, nt, &o, argv[2], name, ".opt", &vi);
                memset(&o, 0, sizeof o);
                o.alpha_func = 1;
                for (unsigned s = 0; s < 16; ++s)
                    if (info.sampler[s] == 2)
                        o.depth_mask |= (uint16_t)(1u << s);
                good &= translate(tok, nt, &o, argv[2], name, ".opt2", &vi);
            }
            else
            {
                for (unsigned r = 0; r < 16; ++r)
                    o.attr_int[r] = (uint8_t)(1 + r % 4);
                o.vs_extra[o.nvs_extra++] = (SmDecl){ SM_TEXCOORD, 9, 0, 1 };
                o.vs_extra[o.nvs_extra++] = (SmDecl){ SM_COLOR, 1, 0, 1 };
                good &= translate(tok, nt, &o, argv[2], name, ".opt", &vi);
            }
        }
        good ? ok++ : bad++;
    }
    closedir(d);
    printf("translated %u (%u vertex, %u pixel), failed %u\n", ok, nvs, nps, bad);
    return bad ? 1 : 0;
}
