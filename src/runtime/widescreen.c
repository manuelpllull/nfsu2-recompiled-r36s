/*
 * widescreen.c - HUD, FOV, mirror, radar and FMV geometry for wide displays.
 *
 * A port of the HUD/FOV parts of ThirteenAG's NFSUnderground2.WidescreenFix
 * (WidescreenFixesPack, MIT): Frontend.ixx (FixHUD, cutscene borders, FMV
 * mode) and Rendering.ixx (FixFOV). That mod patches x86 bytes in memory; the
 * game here is lifted C, so each patch is re-expressed as one of:
 *   - a write to a float the code reads from .rdata/.data (still guest memory);
 *   - a host hook (Lifter hook_sites -> RECOMP_HOOK -> recomp_hook) right after
 *     an instruction that stored an immediate, rewriting what it stored, or
 *     where the mod's inline hook adjusted registers/memory.
 * The hook VAs are listed in config/hooks.txt and were located by the mod's
 * own byte patterns in SPEED2.EXE (scripts/widescreen_sites.py). The sites are those of the exe the
 * lifted code comes from (sha1 f4bb0d99..., the same code as work/SPEED2.analysis.exe); an earlier table
 * was for another build of the game and hooked unrelated instructions.
 *
 * HUD element offsets come from the mod's NFSUnderground2.WidescreenFix.dat,
 * read from <game>/scripts/ when present (the player supplies the mod).
 */
#include "recomp_types.h"
#include "nfs_exe_addr.h"   /* v10 addresses -> the lifted exe's (config/exe_functions.txt) */
#include "nfs_widescreen.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { float pos_x, pos_y, offset_x, offset_y; } hud_entry;
static hud_entry s_hud[256];
static unsigned s_hud_count;
static int s_enabled;
static float s_aspect = 4.0f / 3.0f;
static float s_hud_pos_x = 320.0f;      /* the mod's fHudPosX */
static float s_hud_offset = 0.0f;       /* fWidescreenHudOffset */
static float s_width = 640.0f, s_height = 480.0f;
static const int s_disable_cutscene_borders = 1;
/* Extra vertical move of the minimap (mobile layout: top-left instead of
 * bottom-left, clear of the thumbs). 0 keeps the mod's position. */
static float s_minimap_dy = 0.0f;
/* Game frame time for FPSLimit (0 = the game's own 1/60 s). */
static float s_frame_time = 0.0f;
/* FOV multipliers chosen per view at 0x5C7F64 and applied at its three
 * reads of the 1.0/0.5/1.0 constants (the mod repoints those operands). */
static double s_fov_scale[3] = {1.0, 1.0, 1.0};

static float rdf(uint32_t va) { uint32_t v = MEM32(va); float f; memcpy(&f, &v, 4); return f; }
static void wrf(uint32_t va, float f) { uint32_t v; memcpy(&v, &f, 4); MEM32(va) = v; }

static void load_dat(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof line, f) && s_hud_count < sizeof s_hud / sizeof s_hud[0]) {
        hud_entry e;
        if (!line[0] || line[0] == '#') continue;
        if (sscanf(line, "%*s %f %f %f %f", &e.pos_x, &e.pos_y, &e.offset_x, &e.offset_y) == 4)
            s_hud[s_hud_count++] = e;
    }
    fclose(f);
}

/* MiniMap_Pos_1..4 and MiniMap_Filter_Pos in the mod's .dat (names are not
 * loaded; the anchors are what the game draws them at). */
static int is_minimap(const hud_entry* e) {
    int x = (int)roundf(e->pos_x), y = (int)roundf(e->pos_y);
    return (x == -223 && (y == 70 || y == 71 || y == 142 || y == 143)) || (x == -143 && y == 196);
}

/* The mod's WidescreenHud(): move a known HUD element by its .dat offset. */
static void widescreen_hud(float* x, float* y) {
    if (s_hud_offset <= 0.0f) return;
    const float base_offset = (480.0f * (16.0f / 9.0f) - 640.0f) / 2.0f;
    int ix = (int)floorf(*x), iy = (int)floorf(*y);
    int rx = (int)roundf(*x), ry = (int)roundf(*y);
    for (unsigned i = 0; i < s_hud_count; i++) {
        const hud_entry* e = &s_hud[i];
        int ex = (int)roundf(e->pos_x), ey = (int)roundf(e->pos_y);
        if ((ex != ix && ex != rx) || (ey != iy && ey != ry)) continue;
        if (e->offset_x >= 0.0f) *x += s_hud_offset + (e->offset_x - base_offset);
        else *x -= s_hud_offset - (e->offset_x + base_offset);
        *y += e->offset_y;
        if (is_minimap(e)) *y += s_minimap_dy;
        return;
    }
}

static void adjust_mem_pair(uint32_t vx, uint32_t vy) {
    float x = rdf(vx), y = rdf(vy);
    widescreen_hud(&x, &y);
    wrf(vx, x); wrf(vy, y);
}

void nfs_widescreen_init(const char* game_root) {
    char path[1024];
    s_hud_count = 0;
    if (getenv("NFSU2_WIDESCREEN_DAT")) {
        load_dat(getenv("NFSU2_WIDESCREEN_DAT"));
    }
    if (!s_hud_count && game_root && game_root[0]) {
        snprintf(path, sizeof path, "%s/widescreen/NFSUnderground2.WidescreenFix.dat", game_root);
        load_dat(path);
    }
    if (!s_hud_count && game_root && game_root[0]) {
        snprintf(path, sizeof path, "%s/scripts/NFSUnderground2.WidescreenFix.dat", game_root);
        load_dat(path);
    }
    if (!s_hud_count && game_root && game_root[0]) {
        snprintf(path, sizeof path, "%s/../widescreen/NFSUnderground2.WidescreenFix.dat", game_root);
        load_dat(path);
    }
    if (!s_hud_count) {
        load_dat("widescreen/NFSUnderground2.WidescreenFix.dat");
    }
    if (!s_hud_count) {
        load_dat("scripts/NFSUnderground2.WidescreenFix.dat");
    }
    if (!s_hud_count) {
        load_dat("ports/r36s/package/widescreen/NFSUnderground2.WidescreenFix.dat");
    }
    s_enabled = 1;
}

void nfs_widescreen_set_resolution(unsigned width, unsigned height) {
    float hud_scale_x;
    if (!s_enabled || !width || !height) return;
    s_width = (float)width; s_height = (float)height;
    s_aspect = s_width / s_height;
    hud_scale_x = (1.0f / s_width * (s_height / 480.0f)) * 2.0f;
    s_hud_pos_x = 640.0f / (640.0f * hud_scale_x);
    /* HudAspectRatioConstraint = Auto: -CalculateWidescreenOffset(W, H, 640, 480). */
    s_hud_offset = -((640.0f / 2.0f) - (480.0f / 2.0f) * s_aspect);
    wrf(NFS_VA(0x0079AC0C), hud_scale_x);       /* fld [fHudScaleX]: 0x79AC0C in v10, 0x79AC10 in v12, 0x79AC18 in v12b.
                                                   Writing both addresses clobbered the neighbouring
                                                   constant of whichever exe was running. */
    wrf(NFS_VA(0x00797D50), s_hud_pos_x);       /* fsub [fHudPosX] */
    wrf(NFS_VA(0x00797D58), s_hud_pos_x * 2.0f); /* fmul [fHudPosX_x2] */
    /* Dyno result graph origin (float[2]). */
    wrf(0x007FA3C0u, 79.0f + ((480.0f * s_aspect) - 640.0f) / 2.0f);
    wrf(0x007FA3C4u, 60.0f);
}

int nfs_widescreen_hud_entries(void) { return (int)s_hud_count; }

void nfs_widescreen_set_minimap_offset(float dy) { s_minimap_dy = dy; }

/* FPSLimit (the mod's Misc.ixx): the game paces its main loop with a fixed
 * frame time of 1/60 s. SetFrameRate (0x576CB0) stores 1/fps in 0x865558 and
 * the loop at 0x57EB0A waits for it; the video-mode, smokeable and step
 * constants carry the same 1/60. They are data, so the lifted code reads
 * whatever is written here. */

void nfs_widescreen_set_frame_rate(unsigned fps) {
    s_frame_time = fps >= 30 ? 1.0f / (float)fps : 0.0f;
}

void nfs_widescreen_apply_frame_rate(void) {
    if (s_frame_time <= 0.0f) return;
    if (rdf(0x00865558u) != s_frame_time) {
        wrf(0x00865558u, s_frame_time);   /* main loop pacing (SetFrameRate) */
        wrf(0x007FB710u, s_frame_time);
        wrf(0x007875BCu, s_frame_time);   /* video mode frame time */
        wrf(0x007FC858u, s_frame_time);   /* smokeable frame time */
    }
}

static void set_esp_float(uint32_t offset, float value) { wrf(g_esp + offset, value); }

/* The four "centred full-screen quad" siblings - sub_005364E0 (EA Games / THX
 * logos), sub_005366E0 (NFSU2 title card and loading screens), sub_00536910 and
 * sub_00536BF0 - all place a quad at (320,240) and build its matrix with
 * sub_0051AD40.  They carry 4:3 art, so on a wider target they have to be
 * pillarboxed: full height, centred, black bars at the sides.
 *
 * On a non-4:3 target the projection compresses the whole Y axis of that matrix
 * by 1/w^2 (w = aspect/(4/3)): the game's own quad lands as 640x270 with its
 * centre on y=135 at 16:9 instead of 640x480 at 240 (measured at 854x480 and
 * 1280x720).  X is untouched (1:1, scaled only by H/480).  Pre-multiplying the
 * design Y - both the model extents and the position - by w^2 undoes it:
 *
 *     hx = 0.5            640 px wide (the pillarbox width on any target)
 *     hy = 0.5 * w2       0.889 at 16:9 -> the full height
 *     X  = s_hud_pos_x    240*aspect -> the screen centre
 *     Y  = 240 * w2       426.7 at 16:9
 *
 * At 4:3 w2 == 1, so every value equals the game's own 0.5 / 0.5 / 320 / 240
 * and the 4:3 picture is untouched.  NFSU2_MOVIE="hx,hy" and NFSU2_MOVIE_Y are
 * diagnostics. */
static float fmv_w2(void) {
    float w = s_aspect / (4.0f / 3.0f);
    return w * w;
}

/* The four model half-extents (x0,y0,x1,y1) the siblings pushed for
 * sub_00519E50; they are the top of the stack at the hook. */
static void fmv_extents(void) {
    static int init = 0; static float ohx = 0.0f, ohy = 0.0f;
    if (!init) { init = 1; const char* e = getenv("NFSU2_MOVIE"); if (e) sscanf(e, "%f,%f", &ohx, &ohy); }
    float hx = 0.5f, hy = 0.5f * fmv_w2();
    if (ohx > 0.0f) { hx = ohx; hy = ohy; }
    set_esp_float(0x0, -hx); set_esp_float(0x4, -hy);
    set_esp_float(0x8,  hx); set_esp_float(0xC,  hy);
}

/* The quad's position Y, corrected onto the centred 4:3 row. */
static void fmv_center_y(uint32_t offset) {
    static int init = 0; static float ov = -1.0f;
    if (!init) { init = 1; const char* y = getenv("NFSU2_MOVIE_Y"); if (y) ov = (float)atof(y); }
    set_esp_float(offset, ov >= 0.0f ? ov : 240.0f * fmv_w2());
}

/* Diagnostic: give each of the four centred-quad anchor hooks a distinct X so the
 * frame shows which lifted function draws what (NFSU2_WS_ANCHOR="a,b,c,d"). */
static float anchor_x(int idx) {
    static float v[4] = { -1, -1, -1, -1 };
    if (v[0] < 0) {
        const char* e = getenv("NFSU2_WS_ANCHOR");
        v[0] = v[1] = v[2] = v[3] = s_hud_pos_x;
        if (e) sscanf(e, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
    }
    return v[idx];
}

void recomp_hook(uint32_t va) {
    if (!s_enabled && va != NFS_VA(0x005D2962)) return;
    { static int t = -1; static uint32_t seen[64]; static int ns;
      if (t < 0) t = getenv("NFSU2_WS_TRACE") != NULL;
      if (t) { int k; for (k = 0; k < ns && seen[k] != va; ++k) ; if (k == ns && ns < 64) { seen[ns++] = va; printf("[WS] hook %08x esp=%08x\n", va, g_esp); fflush(stdout); } } }
    switch (va) {
    /* HUD anchors: `mov [esp+N], 320.0` -> fHudPosX (pattern offsets 4/7). */
    case NFS_VA(0x0051B05F):
        set_esp_float(0xA0, s_hud_pos_x);
        break;

    case NFS_VA(0x0053655C):
        set_esp_float(0x74, anchor_x(0));
        break;

    case NFS_VA(0x0053672D):
        set_esp_float(0x84, anchor_x(1));
        break;

    case NFS_VA(0x0053695D):
        set_esp_float(0x94, anchor_x(2));
        break;

    case NFS_VA(0x00536CA5):
        set_esp_float(0x74, anchor_x(3));
        break;

    case NFS_VA(0x0050B099):
        set_esp_float(0x1A0, s_hud_pos_x);
        break;

    case NFS_VA(0x0048B5D4):
        wrf(0x0082D2D8u, s_hud_pos_x);
        break;

    /* Rear-view mirror quad: 440/200 shifted with the HUD centre. */
    case NFS_VA(0x005CBDCD):
        set_esp_float(0x30, (s_hud_pos_x - 320.0f) + 440.0f);
        break;

    case NFS_VA(0x005CBDDD):
        set_esp_float(0x40, (s_hud_pos_x - 320.0f) + 200.0f);
        break;

    case NFS_VA(0x005CBDED):
        set_esp_float(0x50, (s_hud_pos_x - 320.0f) + 200.0f);
        break;

    case NFS_VA(0x005CBDFD):
        set_esp_float(0x60, (s_hud_pos_x - 320.0f) + 440.0f);
        break;

    /* Cutscene letterbox quads (DisableCutsceneBorders = 1). */
    case NFS_VA(0x005CBBC9):
        set_esp_float(0x38, s_disable_cutscene_borders ? 0.0f : s_width);
        break;

    case NFS_VA(0x005CBBD9):
        set_esp_float(0x48, s_disable_cutscene_borders ? 0.0f : s_width);
        break;

    case NFS_VA(0x005CBB5D):
        set_esp_float(0x2C, s_disable_cutscene_borders ? 0.0f : s_width);
        break;

    case NFS_VA(0x005CBB75):
        set_esp_float(0x3C, s_disable_cutscene_borders ? 0.0f : s_width);
        break;

    /* Radar mask covers the whole render target. */
    case NFS_VA(0x005C6F3E):
        set_esp_float(0x30, s_width * 2.0f);
        break;

    case NFS_VA(0x005C6F4E):
        set_esp_float(0x40, s_width * 2.0f);
        break;

    case NFS_VA(0x005C6F56):
        set_esp_float(0x44, s_height * 2.0f);
        break;

    case NFS_VA(0x005C6F66):
        set_esp_float(0x54, s_height * 2.0f);
        break;

    /* HudHook: the element position just stored from ecx/edx. */
    case NFS_VA(0x0051AE28):
        adjust_mem_pair(g_esp + 0x60, g_esp + 0x64);
        break;

    /* BlipsHook: radar blips, before `mov [edx], ecx`. */
    case NFS_VA(0x0051D2E5):
        adjust_mem_pair(g_ebx + 0x1C, g_ebx + 0x20);
        g_ecx = MEM32(g_ebx + 0x1C);
        break;

    /* HudHook2: ecx = adjusted X of [esi+1C]/[esi+20]. */
    case NFS_VA(0x004C6589): {
        float x, y; uint32_t v = g_ecx; memcpy(&x, &v, 4); y = rdf(g_esi + 0x20);
        widescreen_hud(&x, &y); memcpy(&v, &x, 4); g_ecx = v;
        break;
    }

    /* StopSignHook: [esi] was ST0 + [edx]; make it ST0 + adjusted [edx]. */
    case NFS_VA(0x00504D5F): {
        float x = rdf(g_edx), y = rdf(g_edx + 4), original = x;
        widescreen_hud(&x, &y);
        wrf(g_esi, rdf(g_esi) + (x - original));
        break;
    }

    /* FMV / loading-screen quads: the four model half-extents (x0,y0,x1,y1) at
     * the top of the stack, consumed by sub_00519E50.  See fmv_extents(). */
    case NFS_VA(0x005366C2):   /* sub_005364E0: EA Games and THX logos */
    case NFS_VA(0x005368E6):   /* sub_005366E0: NFSU2 title card and loading screens */
    case NFS_VA(0x00536BD1):   /* sub_00536910 */
        fmv_extents();
        break;

    /* The same quad's position Y, right after the game stored (320,240,0) and
     * just before sub_0051AD40 turns it into the matrix.  See fmv_center_y(). */
    case NFS_VA(0x0053656C):   /* sub_005364E0: EA Games / THX */
        fmv_center_y(0x78);
        break;

    case NFS_VA(0x00536743):   /* sub_005366E0: title card / loading screens */
        fmv_center_y(0x88);
        break;

    case NFS_VA(0x00536973):   /* sub_00536910 */
        fmv_center_y(0x98);
        break;

    case NFS_VA(0x00536CB5):   /* sub_00536BF0 */
        fmv_center_y(0x78);
        break;

    /* FixFOV: ST0 was viewport height/width; the mod loads a per-view scale
     * and picks the constants of the three later reads by view id (ecx). */
    case NFS_VA(0x005C7C34): {
        uint32_t view = g_ecx;
        double hor = 1.0 / (s_aspect / (4.0 / 3.0));
        if (view == 1 || view == 4) { s_fov_scale[0] = hor; s_fov_scale[1] = 0.43511; s_fov_scale[2] = 1.22; }
        else { s_fov_scale[0] = 1.0; s_fov_scale[1] = 0.5; s_fov_scale[2] = 1.0; }
        g_st[0] = view == 3 ? 0.45 : 1.0;   /* rear-view mirror */
        break;
    }

    /* `fdivr [1.0]`, `fmul [0.5]`, `fdivr [1.0]`: rescale by new/original. */
    case NFS_VA(0x005C7CBE):
        g_st[0] *= s_fov_scale[0] / rdf(0x00784250u);
        break;

    case NFS_VA(0x005C7CD0):
        g_st[0] *= s_fov_scale[1] / rdf(0x00784260u);
        break;

    case NFS_VA(0x005C7CF5):
        g_st[0] *= s_fov_scale[2] / rdf(0x00784250u);
        break;

    /* FPSLimit: eDisplayFrame pushes 1/60 s as the particle step. */
    case NFS_VA(0x005D2962):
        if (s_frame_time > 0.0f) set_esp_float(0x0, s_frame_time);
        break;

    default: break;
    }
}
