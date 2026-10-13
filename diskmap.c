/*
 * diskmap — a fast, tiny disk-space treemap in the spirit of SpaceMonger 1.4.
 *
 * Single-file C11 app: SDL2 for windowing, FreeType+Fontconfig for crisp text.
 * Dynamically linked, stripped binary is well under 200 KB.
 *
 * Build:  make
 * Usage:  ./diskmap [path]        (defaults to $HOME)
 *
 * Keys:   click / Right  zoom into folder      Esc / Backspace / Left / wheel-down  zoom out
 *         Right / wheel-up / Zoom In           zoom into biggest child folder
 *         g = zoom full (root)   r = reload    f = free-space box   o = open path
 *         right-click = copy path to clipboard
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Colors: by nesting depth (like the macOS DiskMap) so that the       */
/* contents of a folder are always visually distinct from the folder.  */
/* ------------------------------------------------------------------ */

static const SDL_Color depth_fill[5] = {
    {255, 168, 148, 255},   /* salmon */
    {255, 219,  61, 255},   /* yellow */
    {140, 235, 122, 255},   /* green  */
    { 97, 224, 219, 255},   /* cyan   */
    {255, 186, 122, 255},   /* orange */
};
static const SDL_Color free_fill = {0xF0, 0xA8, 0xB8, 255};   /* pink, SpaceMonger-style */
static const SDL_Color border_col = {0x2E, 0x2E, 0x2E, 255};

/* working palette: depth_fill/free_fill, muted toward the theme background
   when the active Omarchy theme is dark (set by load_theme) */
static SDL_Color pal[5];
static SDL_Color pal_free;

#define MAX_DRAW_DEPTH 5      /* children drawn while depth < MAX_DRAW_DEPTH */

/* ------------------------------------------------------------------ */
/* UI frame theme: follows the Omarchy theme colors.toml.              */
/* The treemap boxes keep their own fixed palette; only the frame      */
/* (toolbar, status bar, buttons, tooltip, text) is themed.            */
/* ------------------------------------------------------------------ */

typedef struct {
    SDL_Color bg, bar_bg, btn_bg, btn_hover, btn_border;
    SDL_Color text, text_dim, text_faint;
    SDL_Color accent, accent_text, field_bg, field_text;
} Theme;
static Theme theme;
static int theme_is_dark;   /* mode = "dark" in the active theme */

/* white or near-black, whichever contrasts with c */
static SDL_Color contrast_of(SDL_Color c)
{
    double lum = (0.299*c.r + 0.587*c.g + 0.114*c.b) / 255.0;
    return lum > 0.6 ? (SDL_Color){0x10,0x10,0x10,255} : (SDL_Color){0xFF,0xFF,0xFF,255};
}

static void theme_defaults(void)
{
    theme.bg = (SDL_Color){0x20,0x20,0x20,255};
    theme.bar_bg = (SDL_Color){0x2A,0x2A,0x2A,255};
    theme.btn_bg = (SDL_Color){0x3C,0x3C,0x3C,255};
    theme.btn_hover = (SDL_Color){0x50,0x6A,0x8C,255};
    theme.btn_border = (SDL_Color){0x60,0x60,0x60,255};
    theme.text = (SDL_Color){0xD8,0xD8,0xD8,255};
    theme.text_dim = (SDL_Color){0xC8,0xC8,0xC8,255};
    theme.text_faint = (SDL_Color){0xC0,0xC0,0xC0,255};
    theme.accent = (SDL_Color){0x50,0x6A,0x8C,255};
    theme.accent_text = (SDL_Color){0xFF,0xFF,0xFF,255};
    theme.field_bg = (SDL_Color){0xF0,0xF0,0xF0,255};
    theme.field_text = (SDL_Color){0x10,0x10,0x10,255};
}

static int hexcol(const char *s, SDL_Color *out)
{
    if (s[0] != '#' || strlen(s) < 7) return 0;
    unsigned r, g, b;
    if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) return 0;
    *out = (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, 255};
    return 1;
}

/* active theme lives at $XDG_STATE_HOME/omarchy/current/theme/colors.toml */
static void load_theme(void)
{
    theme_defaults();
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    char path[4200];
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/omarchy/current/theme/colors.toml", xdg);
    else snprintf(path, sizeof path, "%s/.local/state/omarchy/current/theme/colors.toml", home);
    FILE *f = fopen(path, "r");
    if (!f) {
        for (int i = 0; i < 5; i++) pal[i] = depth_fill[i];
        pal_free = free_fill;
        return;
    }
    int dark = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        while (*k == ' ' || *k == '\t') k++;
        char *e = eq - 1;
        while (e >= k && (*e == ' ' || *e == '\t')) *e-- = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (*v == '"') v++;
        char *q = strchr(v, '"');
        if (q) *q = '\0';
        if (!strcmp(k, "mode")) { if (strstr(v, "dark")) dark = 1; continue; }
        SDL_Color c;
        if (!hexcol(v, &c)) continue;   /* also skips rgba(...) values */
        if      (!strcmp(k, "background"))         theme.bg = theme.field_bg = c;
        else if (!strcmp(k, "dark_background"))    theme.btn_bg = c;
        else if (!strcmp(k, "darker_background"))  theme.bar_bg = c;
        else if (!strcmp(k, "lighter_background")) theme.btn_hover = c;
        else if (!strcmp(k, "foreground"))         theme.text = c;
        else if (!strcmp(k, "light_foreground"))   theme.text_dim = c;
        else if (!strcmp(k, "dark_foreground"))    theme.text_faint = c;
        else if (!strcmp(k, "accent"))             theme.accent = c;
        else if (!strcmp(k, "muted"))              theme.btn_border = c;
    }
    fclose(f);
    /* text on accent buttons / in the open field: whichever contrasts */
    theme.accent_text = contrast_of(theme.accent);
    theme.field_text = contrast_of(theme.field_bg);

    /* box palette: same hues, but in a dark theme mute them toward the
       theme background so the map sits calmly in the dark frame */
    for (int i = 0; i < 5; i++) pal[i] = depth_fill[i];
    pal_free = free_fill;
    theme_is_dark = dark;
    if (dark) {
        for (int i = 0; i < 5; i++) {
            pal[i].r = (Uint8)(pal[i].r * 0.75 + theme.bg.r * 0.25);
            pal[i].g = (Uint8)(pal[i].g * 0.75 + theme.bg.g * 0.25);
            pal[i].b = (Uint8)(pal[i].b * 0.75 + theme.bg.b * 0.25);
        }
        pal_free.r = (Uint8)(pal_free.r * 0.75 + theme.bg.r * 0.25);
        pal_free.g = (Uint8)(pal_free.g * 0.75 + theme.bg.g * 0.25);
        pal_free.b = (Uint8)(pal_free.b * 0.75 + theme.bg.b * 0.25);
    }
}

/* ------------------------------------------------------------------ */
/* Tree storage                                                        */
/* ------------------------------------------------------------------ */

enum { NF_MOUNT = 1, NF_ERR = 2 };

typedef struct {
    uint32_t name_off;    /* offset into names buffer */
    uint64_t size;        /* allocated bytes (st_blocks*512) */
    int64_t  mtime;
    uint32_t kid_start, n_kids;
    uint32_t parent;
    uint32_t nfiles, nfolders;
    uint8_t  isdir, flags;
} Node;

typedef struct {
    Node    *nodes;   size_t n_nodes, cap_nodes;
    uint32_t *kids;   size_t n_kids,  cap_kids;
    char    *names;   size_t n_names, cap_names;
    char      root_path[4096];
} Tree;

static Tree *g_tree;                 /* what the UI renders (guarded by g_mtx) */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_scan_running, g_scan_abort, g_scan_failed;
static volatile uint64_t g_prog_files, g_prog_bytes;
static char g_scan_err[256];

/* filesystem stats for the scanned path; mountroot = scan root is a mount point */
static uint64_t g_fs_total, g_fs_free;
static volatile int g_fs_mountroot;

static uint32_t tree_add(Tree *t, uint32_t parent, const char *name,
                         uint8_t isdir, uint64_t size, int64_t mtime)
{
    if (t->n_nodes == t->cap_nodes) {
        t->cap_nodes = t->cap_nodes ? t->cap_nodes * 3 / 2 : 4096;
        t->nodes = realloc(t->nodes, t->cap_nodes * sizeof(Node));
    }
    size_t nlen = strlen(name) + 1;
    if (t->n_names + nlen > t->cap_names) {
        t->cap_names = (t->n_names + nlen) * 3 / 2 + 65536;
        t->names = realloc(t->names, t->cap_names);
    }
    memcpy(t->names + t->n_names, name, nlen);
    Node *nd = &t->nodes[t->n_nodes];
    nd->name_off = (uint32_t)t->n_names;
    nd->size = size; nd->mtime = mtime;
    nd->kid_start = 0; nd->n_kids = 0;
    nd->parent = parent; nd->nfiles = 0; nd->nfolders = 0;
    nd->isdir = isdir; nd->flags = 0;
    t->n_names += nlen;
    if (parent != UINT32_MAX) {
        if (t->n_kids == t->cap_kids) {
            t->cap_kids = t->cap_kids ? t->cap_kids * 3 / 2 : 4096;
            t->kids = realloc(t->kids, t->cap_kids * sizeof(uint32_t));
        }
        t->kids[t->n_kids] = (uint32_t)t->n_nodes;
        Node *p = &t->nodes[parent];
        if (p->n_kids == 0) p->kid_start = (uint32_t)t->n_kids;
        p->n_kids++;
        t->n_kids++;
    }
    return (uint32_t)(t->n_nodes++);
}

static const char *tree_name(Tree *t, uint32_t i) { return t->names + t->nodes[i].name_off; }

static void tree_free(Tree *t)
{
    free(t->nodes); free(t->kids); free(t->names);
    memset(t, 0, sizeof *t);
}

/* ------------------------------------------------------------------ */
/* Scanner (background thread)                                         */
/* ------------------------------------------------------------------ */

typedef struct { int dirfd; uint32_t node; } Frame;

#define MAX_DEPTH 500

static int cmp_sz_r(const void *a, const void *b, void *arg)
{
    Tree *t = arg;
    uint64_t sa = t->nodes[*(const uint32_t *)a].size;
    uint64_t sb = t->nodes[*(const uint32_t *)b].size;
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

static void *scan_thread(void *arg)
{
    const char *path = (const char *)arg;
    Tree *t = calloc(1, sizeof(Tree));
    snprintf(t->root_path, sizeof t->root_path, "%s", path);

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(g_scan_err, sizeof g_scan_err, "%s: %s", path, strerror(errno));
        fprintf(stderr, "diskmap: %s\n", g_scan_err);
        free(t); g_scan_running = 0; g_scan_failed = 1; return NULL;
    }
    dev_t root_dev = st.st_dev;

    int rootfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) {
        snprintf(g_scan_err, sizeof g_scan_err, "%s: %s", path, strerror(errno));
        fprintf(stderr, "diskmap: %s\n", g_scan_err);
        free(t); g_scan_running = 0; g_scan_failed = 1; return NULL;
    }

    uint32_t root = tree_add(t, UINT32_MAX, path, 1,
                             (uint64_t)st.st_blocks * 512, st.st_mtime);

    Frame *stack = malloc((MAX_DEPTH + 1) * sizeof(Frame));
    size_t sp = 0;
    stack[sp++] = (Frame){rootfd, root};

    while (sp) {
        Frame f = stack[--sp];
        DIR *d = fdopendir(f.dirfd);          /* takes ownership of dirfd */
        if (!d) { close(f.dirfd); t->nodes[f.node].flags |= NF_ERR; continue; }
        struct dirent *de;
        int aborted = 0;
        while ((de = readdir(d))) {
            if (g_scan_abort) { aborted = 1; break; }
            if (de->d_name[0] == '.' &&
                (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
                continue;
            struct stat ls;
            int is_dir = de->d_type == DT_DIR;
            int is_reg = de->d_type == DT_REG;
            if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
                if (fstatat(dirfd(d), de->d_name, &ls, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (S_ISLNK(ls.st_mode)) {   /* count the link's own blocks, like du */
                    uint64_t bytes = (uint64_t)ls.st_blocks * 512;
                    tree_add(t, f.node, de->d_name, 0, bytes, ls.st_mtime);
                    g_prog_bytes += bytes;
                    g_prog_files++;
                    continue;
                }
                is_dir = S_ISDIR(ls.st_mode);
                is_reg = S_ISREG(ls.st_mode);
            }
            if (!is_dir && !is_reg) continue;
            if (fstatat(dirfd(d), de->d_name, &ls, AT_SYMLINK_NOFOLLOW) != 0) continue;

            if (is_dir) {
                uint32_t n = tree_add(t, f.node, de->d_name, 1,
                                      (uint64_t)ls.st_blocks * 512, ls.st_mtime);
                if (ls.st_dev == root_dev && sp < MAX_DEPTH) {
                    int cfd = openat(dirfd(d), de->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if (cfd >= 0) stack[sp++] = (Frame){cfd, n};
                    else { t->nodes[n].flags |= NF_ERR; }
                } else {
                    t->nodes[n].flags |= NF_MOUNT;      /* other filesystem / too deep */
                }
            } else {
                uint64_t bytes = (uint64_t)ls.st_blocks * 512;
                tree_add(t, f.node, de->d_name, 0, bytes, ls.st_mtime);
                g_prog_bytes += bytes;
            }
            g_prog_files++;
        }
        closedir(d);
        if (aborted) {
            while (sp) close(stack[--sp].dirfd);
            goto fail;
        }
    }
    free(stack);

    /* bottom-up aggregation (nodes are created in pre-order) */
    for (size_t i = t->n_nodes - 1; i > 0; i--) {
        Node *nd = &t->nodes[i];
        Node *p  = &t->nodes[nd->parent];
        p->size += nd->size;
        p->nfiles   += nd->nfiles   + !nd->isdir;
        p->nfolders += nd->nfolders + nd->isdir;
    }
    t->nodes[0].nfolders++;   /* the root itself */

    /* sort each sibling group by size, descending */
    for (size_t i = 0; i < t->n_nodes; i++)
        if (t->nodes[i].n_kids > 1)
            qsort_r(t->kids + t->nodes[i].kid_start, t->nodes[i].n_kids,
                    sizeof(uint32_t), cmp_sz_r, t);

    struct statvfs sv;
    if (statvfs(path, &sv) == 0) {
        g_fs_total = (uint64_t)sv.f_blocks * sv.f_frsize;
        g_fs_free  = (uint64_t)sv.f_bavail * sv.f_frsize;
    }
    /* free space box only makes sense when scanning a filesystem root */
    g_fs_mountroot = 1;
    if (strcmp(path, "/") != 0) {
        char parent[4096];
        snprintf(parent, sizeof parent, "%s/..", path);
        struct stat pst;
        if (stat(parent, &pst) == 0 && pst.st_dev == st.st_dev) g_fs_mountroot = 0;
    }

    pthread_mutex_lock(&g_mtx);
    if (g_tree) tree_free(g_tree);
    g_tree = t;
    pthread_mutex_unlock(&g_mtx);
    g_scan_running = 0;
    return NULL;

fail:
    free(stack);
    tree_free(t);
    free(t);
    g_scan_running = 0;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Font (FreeType atlas)                                               */
/* ------------------------------------------------------------------ */

static SDL_Renderer *g_rnd;

typedef struct {
    SDL_Texture *tex;
    int cell_w, cell_h, baseline;
    int gx[128], gy[128], gw[128], gh[128], gadv[128], gxoff[128], gyoff[128];
    int h;                      /* line height */
} Font;

static Font font;

static void font_init(int px)
{
    FT_Library lib; FT_Init_FreeType(&lib);
    char *file = NULL;
    FcInit();
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"Liberation Sans");
    FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);
    FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
    FcResult fr;
    FcPattern *m = FcFontMatch(NULL, pat, &fr);
    if (m) {
        FcChar8 *f = NULL;
        if (FcPatternGetString(m, FC_FILE, 0, &f) == FcResultMatch) file = strdup((char *)f);
        FcPatternDestroy(m);
    }
    FcPatternDestroy(pat);

    FT_Face face;
    if (!file || FT_New_Face(lib, file, 0, &face) != 0) {
        fprintf(stderr, "diskmap: no font found via fontconfig\n");
        exit(1);
    }
    FT_Set_Pixel_Sizes(face, 0, px);

    int maxadv = 0, maxh = 0, maxasc = 0;
    for (int c = 32; c < 127; c++) {
        FT_Load_Char(face, c, FT_LOAD_RENDER);
        FT_GlyphSlot g = face->glyph;
        if (g->advance.x / 64 > maxadv) maxadv = g->advance.x / 64;
        if ((int)g->bitmap.rows > maxh) maxh = (int)g->bitmap.rows;
        if ((int)g->bitmap_top > maxasc) maxasc = (int)g->bitmap_top;
    }
    font.cell_w = maxadv + 2;
    font.cell_h = maxh + 2;
    font.baseline = maxasc;
    font.h = px + 4;

    int cols = 16, rows = 7;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, cols * font.cell_w,
                                                    rows * font.cell_h, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_FillRect(s, NULL, 0);
    for (int c = 32; c < 127; c++) {
        FT_Load_Char(face, c, FT_LOAD_RENDER);
        FT_GlyphSlot g = face->glyph;
        int idx = c - 32, cx = (idx % cols) * font.cell_w, cy = (idx / cols) * font.cell_h;
        int dx = cx + 1, dy = cy + (font.baseline - (int)g->bitmap_top);
        SDL_LockSurface(s);
        uint32_t *dst = (uint32_t *)s->pixels;
        for (int y = 0; y < (int)g->bitmap.rows; y++)
            for (int x = 0; x < (int)g->bitmap.width; x++) {
                uint32_t a = g->bitmap.buffer[y * g->bitmap.pitch + x];
                dst[(dy + y) * s->w + dx + x] = 0x00FFFFFFu | (a << 24);
            }
        SDL_UnlockSurface(s);
        font.gx[c] = dx; font.gy[c] = dy;
        font.gw[c] = (int)g->bitmap.width; font.gh[c] = (int)g->bitmap.rows;
        font.gadv[c] = (int)(g->advance.x / 64);
        font.gxoff[c] = (int)g->bitmap_left; font.gyoff[c] = (int)g->bitmap_top;
    }
    font.tex = SDL_CreateTextureFromSurface(g_rnd, s);
    SDL_FreeSurface(s);
    FT_Done_Face(face); FT_Done_FreeType(lib);
    free(file);
}

/* ------------------------------------------------------------------ */
/* App state                                                           */
/* ------------------------------------------------------------------ */

#define FREE_NODE UINT32_MAX

typedef struct { SDL_Rect r; uint32_t node; uint8_t kind, depth; } Item;
/* kind: 0 folder, 1 file, 2 free */

static Item *items; static size_t n_items, cap_items;

static struct {
    SDL_Window *win;
    int W, H;
    uint32_t zoom;              /* node index filling the viewport (in g_tree) */
    char zoom_path[4096];       /* survives tree swaps */
    int show_free;
    int open_mode; char open_buf[4096]; int open_len;
    int mouse_x, mouse_y; uint32_t hover; /* node under cursor, FREE_NODE none */
    int dirty;
    int scan_done_once;
} app;

enum { TB_OPEN, TB_RELOAD, TB_FULL, TB_IN, TB_OUT, TB_FREE, TB_COUNT };
static const char *tb_label[TB_COUNT] = {"Open","Reload","Zoom Full","Zoom In","Zoom Out","Free Space"};
static SDL_Rect tb_rect[TB_COUNT];
enum { TOOLBAR_H = 34, STATUS_H = 24 };

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */
/* ------------------------------------------------------------------ */

static int text_w(const char *s)
{
    int w = 0;
    for (; *s; s++) { unsigned c = (unsigned char)*s; w += font.gadv[c >= 32 && c < 127 ? c : '?']; }
    return w;
}

static void draw_text(int x, int y, const char *s, SDL_Color col, int maxw)
{
    SDL_SetTextureColorMod(font.tex, col.r, col.g, col.b);
    int pen = x;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c >= 127) c = '?';
        if (maxw && pen - x + font.gadv[c] > maxw) {
            for (const char *p = "..."; *p; p++) {
                unsigned d = (unsigned char)*p;
                if (pen - x + font.gadv[d] > maxw) break;
                SDL_Rect src = { font.gx[d], font.gy[d], font.gw[d], font.gh[d] };
                SDL_Rect dst = { pen, y, font.gw[d], font.gh[d] };
                SDL_RenderCopy(g_rnd, font.tex, &src, &dst);
                pen += font.gadv[d];
            }
            break;
        }
        SDL_Rect src = { font.gx[c], font.gy[c], font.gw[c], font.gh[c] };
        SDL_Rect dst = { pen + font.gxoff[c], y + font.baseline - font.gyoff[c], font.gw[c], font.gh[c] };
        if (font.gw[c]) SDL_RenderCopy(g_rnd, font.tex, &src, &dst);
        pen += font.gadv[c];
    }
}

static void fmt_size(uint64_t b, char *out, size_t n)
{
    static const char *u[] = {"B","KiB","MiB","GiB","TiB","PiB"};
    double v = (double)b; int i = 0;
    while (v >= 1024 && i < 5) { v /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%llu %s", (unsigned long long)b, u[i]);
    else if (v >= 100) snprintf(out, n, "%.0f %s", v, u[i]);
    else snprintf(out, n, "%.1f %s", v, u[i]);
}

static void fmt_date(int64_t t, char *out, size_t n)
{
    struct tm tm; time_t tt = (time_t)t;
    localtime_r(&tt, &tm);
    strftime(out, n, "%d %b %Y %H:%M", &tm);
}

/* ------------------------------------------------------------------ */
/* Squarified treemap layout                                           */
/* ------------------------------------------------------------------ */

static void push_item(SDL_Rect r, uint32_t node, uint8_t kind, int depth)
{
    if (n_items == cap_items) { cap_items = cap_items ? cap_items*3/2 : 4096; items = realloc(items, cap_items*sizeof(Item)); }
    items[n_items++] = (Item){r, node, kind, (uint8_t)depth};
}

static double worst_ratio(const double *a, int n, double len)
{
    double sum = 0, mx = 0, mn = 1e300;
    for (int i = 0; i < n; i++) { sum += a[i]; if (a[i] > mx) mx = a[i]; if (a[i] < mn) mn = a[i]; }
    if (sum <= 0 || mn <= 0) return 1e300;
    double l2 = len*len, s2 = sum*sum;
    double r1 = l2*mx/s2, r2 = s2/(l2*mn);
    return r1 > r2 ? r1 : r2;
}

static void layout_emit(SDL_Rect cr, uint32_t id, int depth);
static void layout_node(uint32_t nidx, SDL_Rect r, int depth);

static void layout_emit(SDL_Rect cr, uint32_t id, int depth)
{
    if (id == FREE_NODE) { push_item(cr, FREE_NODE, 2, depth); return; }
    layout_node(id, cr, depth);
}

/* lay out `count` areas (already scaled to rect area) into r */
static void squarify(const double *areas, const uint32_t *ids, int count, SDL_Rect r, int depth)
{
    double row[64]; uint32_t rid[64];
    int i = 0;
    int cx = r.x, cy = r.y, cw = r.w, ch = r.h;
    while (i < count && cw > 0 && ch > 0) {
        double len = cw < ch ? cw : ch;
        int j = i, rn = 0;
        double cur = 1e300;
        while (j < count && rn < 64) {
            row[rn] = areas[j]; rid[rn] = ids[j];
            double w = worst_ratio(row, rn + 1, len);
            if (rn && w > cur) break;
            cur = w; rn++; j++;
        }
        if (!rn) { row[0] = areas[i]; rid[0] = ids[i]; rn = 1; }
        double sum = 0;
        for (int k = 0; k < rn; k++) sum += row[k];
        double thick = sum / len;
        if (cw >= ch) {                          /* strip is a column */
            if (thick > cw) thick = cw;
            int tw = (i + rn >= count) ? cw : (int)thick;
            int py = 0;
            for (int k = 0; k < rn; k++) {
                int ph = (k == rn - 1) ? ch : (int)(row[k] / thick);
                if (ph > 0) layout_emit((SDL_Rect){cx, cy + py, tw, ph}, rid[k], depth);
                py += ph;
            }
            cx += tw; cw -= tw;
        } else {                                 /* strip is a row */
            if (thick > ch) thick = ch;
            int th2 = (i + rn >= count) ? ch : (int)thick;
            int px = 0;
            for (int k = 0; k < rn; k++) {
                int pw = (k == rn - 1) ? cw : (int)(row[k] / thick);
                if (pw > 0) layout_emit((SDL_Rect){cx + px, cy, pw, th2}, rid[k], depth);
                px += pw;
            }
            cy += th2; ch -= th2;
        }
        i += rn;
    }
}

static void layout_children(uint32_t nidx, SDL_Rect r, int depth)
{
    Tree *t = g_tree;
    Node *nd = &t->nodes[nidx];
    uint32_t nk = nd->n_kids;
    int add_free = (nidx == 0 && app.show_free && g_fs_free && g_fs_mountroot);
    uint32_t cap = nk + (add_free ? 1 : 0);
    if (!cap || r.w < 3 || r.h < 3) return;

    uint32_t *ids = malloc(cap * sizeof(uint32_t));
    double *areas = malloc(cap * sizeof(double));
    double total_sz = 0;
    uint32_t m = 0;
    uint32_t max_kids = depth == 0 ? 320 : 180;   /* like the macOS DiskMap */
    for (uint32_t i = 0; i < nk && m < max_kids; i++) {
        uint32_t ci = t->kids[nd->kid_start + i];
        if (t->nodes[ci].size == 0) continue;
        ids[m] = ci; areas[m] = (double)t->nodes[ci].size; total_sz += areas[m]; m++;
    }
    if (add_free) { ids[m] = FREE_NODE; areas[m] = (double)g_fs_free; total_sz += areas[m]; m++; }
    if (!m || total_sz <= 0) { free(ids); free(areas); return; }

    double scale = ((double)r.w * r.h) / total_sz;
    for (uint32_t i = 0; i < m; i++) areas[i] *= scale;
    squarify(areas, ids, (int)m, r, depth);
    free(ids); free(areas);
}

static void layout_node(uint32_t nidx, SDL_Rect r, int depth)
{
    Tree *t = g_tree;
    Node *nd = &t->nodes[nidx];
    push_item(r, nidx, nd->n_kids ? 0 : 1, depth);
    if (!nd->n_kids) return;
    /* high level only: stop nesting at MAX_DRAW_DEPTH or when too small;
       zooming in is how you dig deeper */
    if (depth >= MAX_DRAW_DEPTH || r.w < 42 || r.h < 38) return;
    /* children live inside the 1px-inset box, below the label strip
       (same insets as the macOS DiskMap) */
    SDL_Rect inner = r;
    int label_h = font.h + 2;
    inner.x += 3; inner.w -= 6;
    inner.y += label_h + 1; inner.h -= label_h + 4;
    layout_children(nidx, inner, depth + 1);
}

static void layout_all(void)
{
    n_items = 0;
    if (!g_tree) return;
    if (app.zoom >= g_tree->n_nodes) app.zoom = 0;
    SDL_Rect area = {0, TOOLBAR_H, app.W, app.H - TOOLBAR_H - STATUS_H};
    layout_node(app.zoom, area, 0);
}

/* ------------------------------------------------------------------ */
/* Hit testing & paths                                                 */
/* ------------------------------------------------------------------ */

static uint32_t hit_test(int x, int y)
{
    for (size_t i = n_items; i-- > 0;) {
        Item *it = &items[i];
        if (x >= it->r.x && x < it->r.x + it->r.w && y >= it->r.y && y < it->r.y + it->r.h)
            return it->node;
    }
    return FREE_NODE;
}

static void node_path(uint32_t n, char *out, size_t cap)
{
    uint32_t stack[MAX_DEPTH + 2]; int sp = 0;
    for (uint32_t i = n; i != UINT32_MAX && sp < MAX_DEPTH + 1 && i < g_tree->n_nodes; i = g_tree->nodes[i].parent)
        stack[sp++] = i;
    out[0] = 0; size_t len = 0;
    while (sp) {
        const char *nm = tree_name(g_tree, stack[--sp]);
        const char *sep = (len && out[len-1] != '/') ? "/" : "";
        int written = snprintf(out + len, cap - len, "%s%s", sep, nm);
        if (written > 0) len += (size_t)written;
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void draw_button(int i, int pressed)
{
    SDL_Rect r = tb_rect[i];
    SDL_Color bg = pressed ? theme.accent : theme.btn_bg;
    SDL_SetRenderDrawColor(g_rnd, bg.r, bg.g, bg.b, 255);
    SDL_RenderFillRect(g_rnd, &r);
    SDL_Color br = pressed ? theme.accent : theme.btn_border;
    SDL_SetRenderDrawColor(g_rnd, br.r, br.g, br.b, 255);
    SDL_RenderDrawRect(g_rnd, &r);
    draw_text(r.x + 8, r.y + (r.h - font.h)/2 - 1, tb_label[i],
              pressed ? theme.accent_text : theme.text, 0);
}

static void render(void)
{
    Tree *t = g_tree;
    SDL_SetRenderDrawColor(g_rnd, theme.bg.r, theme.bg.g, theme.bg.b, 255);
    SDL_RenderClear(g_rnd);

    /* ---- toolbar ---- */
    SDL_Rect tb = {0, 0, app.W, TOOLBAR_H};
    SDL_SetRenderDrawColor(g_rnd, theme.bar_bg.r, theme.bar_bg.g, theme.bar_bg.b, 255);
    SDL_RenderFillRect(g_rnd, &tb);
    for (int i = 0; i < TB_COUNT; i++)
        draw_button(i, i == TB_FREE && app.show_free);

    if (app.open_mode) {
        SDL_Rect f = {tb_rect[TB_OPEN].x + tb_rect[TB_OPEN].w + 8, 5,
                      app.W - tb_rect[TB_OPEN].x - tb_rect[TB_OPEN].w - 16, TOOLBAR_H - 10};
        SDL_SetRenderDrawColor(g_rnd, theme.field_bg.r, theme.field_bg.g, theme.field_bg.b, 255);
        SDL_RenderFillRect(g_rnd, &f);
        draw_text(f.x + 4, f.y + (f.h - font.h)/2 - 1, app.open_buf, theme.field_text, f.w - 8);
        int cx = f.x + 4 + text_w(app.open_buf);
        SDL_SetRenderDrawColor(g_rnd, theme.field_text.r, theme.field_text.g, theme.field_text.b, 255);
        SDL_RenderDrawLine(g_rnd, cx, f.y + 3, cx, f.y + f.h - 3);
    } else if (t) {
        int x0 = tb_rect[TB_FREE].x + tb_rect[TB_FREE].w + 12;
        draw_text(x0, (TOOLBAR_H - font.h)/2 - 1, app.zoom_path,
                  theme.text_dim, app.W - x0 - 10);
    }

    /* ---- treemap ---- */
    if (t) {
        if (app.hover != FREE_NODE && app.hover >= t->n_nodes) app.hover = FREE_NODE;
        for (size_t i = 0; i < n_items; i++) {
            Item *it = &items[i];
            /* like the macOS DiskMap: inset every box 1 px so adjacent
               boxes show one clean border with a hairline of parent color */
            SDL_Rect b = {it->r.x + 1, it->r.y + 1, it->r.w - 2, it->r.h - 2};
            if (b.w < 1 || b.h < 1) continue;
            SDL_Color fill = it->kind == 2 ? pal_free : pal[it->depth % 5];
            if (it->node == app.hover && it->node != FREE_NODE) {
                fill.r = (Uint8)(fill.r + (255 - fill.r) * 0.35);
                fill.g = (Uint8)(fill.g + (255 - fill.g) * 0.35);
                fill.b = (Uint8)(fill.b + (255 - fill.b) * 0.35);
            }
            SDL_SetRenderDrawColor(g_rnd, fill.r, fill.g, fill.b, 255);
            SDL_RenderFillRect(g_rnd, &b);
            /* stroke like the macOS version: dark gray at 72% alpha over
               the fill, so borders are soft and tinted by the box color */
            SDL_SetRenderDrawColor(g_rnd,
                (Uint8)(fill.r * 0.28 + border_col.r * 0.72),
                (Uint8)(fill.g * 0.28 + border_col.g * 0.72),
                (Uint8)(fill.b * 0.28 + border_col.b * 0.72), 255);
            SDL_RenderDrawRect(g_rnd, &b);

            /* labels: near-black on the (always fairly light) fills;
               only switch to white if a fill ever gets truly dark */
            double lum = (0.299*fill.r + 0.587*fill.g + 0.114*fill.b) / 255.0;
            SDL_Color tc = lum > 0.45 ? (SDL_Color){0x14,0x14,0x14,255}
                                      : (SDL_Color){0xFF,0xFF,0xFF,255};
            if (it->kind == 2) {   /* free space */
                if (b.w > 90 && b.h > font.h) {
                    char l1[64], fs[32];
                    fmt_size(g_fs_free, fs, sizeof fs);
                    snprintf(l1, sizeof l1, "Free Space  %s", fs);
                    draw_text(b.x + 3, b.y + 2, l1, tc, b.w - 6);
                }
                continue;
            }
            /* one-line label: "name  size" (size only when there is room) */
            if (b.w > 28 && b.h > font.h - 2) {
                if (b.w > 95) {
                    char lab[544], sz[32];
                    fmt_size(t->nodes[it->node].size, sz, sizeof sz);
                    snprintf(lab, sizeof lab, "%s  %s", tree_name(t, it->node), sz);
                    draw_text(b.x + 3, b.y + 2, lab, tc, b.w - 6);
                } else {
                    draw_text(b.x + 3, b.y + 2, tree_name(t, it->node), tc, b.w - 6);
                }
            }
        }
    }

    /* ---- scanning overlay ---- */
    if (g_scan_running) {
        char msg[128], fs[32];
        fmt_size(g_prog_bytes, fs, sizeof fs);
        snprintf(msg, sizeof msg, "Scanning... %llu files, %s",
                 (unsigned long long)g_prog_files, fs);
        int w = text_w(msg) + 20;
        SDL_Rect box = {8, TOOLBAR_H + 8, w, font.h + 12};
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_rnd, theme.bar_bg.r, theme.bar_bg.g, theme.bar_bg.b, 230);
        SDL_RenderFillRect(g_rnd, &box);
        SDL_SetRenderDrawColor(g_rnd, theme.btn_border.r, theme.btn_border.g, theme.btn_border.b, 255);
        SDL_RenderDrawRect(g_rnd, &box);
        draw_text(box.x + 10, box.y + 6, msg, theme.accent, 0);
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_NONE);
    } else if (g_scan_failed) {
        char msg[300];
        snprintf(msg, sizeof msg, "Cannot scan: %s", g_scan_err);
        int w = text_w(msg) + 20;
        SDL_Rect box = {8, TOOLBAR_H + 8, w, font.h + 12};
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_rnd, 0x30, 0x10, 0x10, 230);
        SDL_RenderFillRect(g_rnd, &box);
        SDL_SetRenderDrawColor(g_rnd, 0xA0, 0x50, 0x50, 255);
        SDL_RenderDrawRect(g_rnd, &box);
        draw_text(box.x + 10, box.y + 6, msg, (SDL_Color){0xFF,0xC0,0xC0,255}, 0);
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_NONE);
    } else if (!t) {
        draw_text(12, TOOLBAR_H + 12, "No tree. Press Open or Reload.", theme.text_faint, 0);
    }

    /* ---- status bar: totals ---- */
    SDL_Rect sb = {0, app.H - STATUS_H, app.W, STATUS_H};
    SDL_SetRenderDrawColor(g_rnd, theme.bar_bg.r, theme.bar_bg.g, theme.bar_bg.b, 255);
    SDL_RenderFillRect(g_rnd, &sb);
    if (t) {
        Node *z = &t->nodes[app.zoom];
        char tot[32]; fmt_size(z->size, tot, sizeof tot);
        char left[96];
        snprintf(left, sizeof left, "%s  |  %u files, %u folders", tot, z->nfiles, z->nfolders);
        draw_text(8, app.H - STATUS_H + (STATUS_H - font.h)/2 - 1, left, theme.text_dim, 0);
    }

    /* ---- tooltip ---- */
    if (app.hover != FREE_NODE && t && !app.open_mode && app.hover < t->n_nodes &&
        app.mouse_y > TOOLBAR_H && app.mouse_y < app.H - STATUS_H) {
        Node *nd = &t->nodes[app.hover];
        char path[4096]; node_path(app.hover, path, sizeof path);
        char sz[32], dt[32], l2[128], extra[128];
        fmt_size(nd->size, sz, sizeof sz);
        fmt_date(nd->mtime, dt, sizeof dt);
        snprintf(l2, sizeof l2, "%s   %s", sz, dt);
        double p = t->nodes[app.zoom].size ? 100.0 * nd->size / t->nodes[app.zoom].size : 0;
        if (nd->n_kids) snprintf(extra, sizeof extra, "%.1f%% of view  |  %u files, %u folders", p, nd->nfiles, nd->nfolders);
        else snprintf(extra, sizeof extra, "%.1f%% of view", p);
        const char *lines[3] = {path, l2, extra};
        int w = 0;
        for (int i = 0; i < 3; i++) { int tw = text_w(lines[i]); if (tw > w) w = tw; }
        w += 20; int h = font.h * 3 + 18;
        int tx = app.mouse_x + 14, ty = app.mouse_y + 14;
        if (tx + w > app.W) tx = app.mouse_x - w - 10;
        if (ty + h > app.H - STATUS_H) ty = app.mouse_y - h - 10;
        SDL_Rect box = {tx, ty, w, h};
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_rnd, theme.bar_bg.r, theme.bar_bg.g, theme.bar_bg.b, 240);
        SDL_RenderFillRect(g_rnd, &box);
        SDL_SetRenderDrawColor(g_rnd, theme.btn_border.r, theme.btn_border.g, theme.btn_border.b, 255);
        SDL_RenderDrawRect(g_rnd, &box);
        SDL_SetRenderDrawBlendMode(g_rnd, SDL_BLENDMODE_NONE);
        draw_text(tx + 10, ty + 7, lines[0], theme.text, w - 20);
        draw_text(tx + 10, ty + 7 + font.h, lines[1], theme.accent, w - 20);
        draw_text(tx + 10, ty + 7 + font.h*2, lines[2], theme.text_faint, w - 20);
    }

    SDL_RenderPresent(g_rnd);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void set_title(void)
{
    char title[512];
    if (g_fs_total) {
        char tot[32], fr[32];
        fmt_size(g_fs_total, tot, sizeof tot);
        fmt_size(g_fs_free, fr, sizeof fr);
        snprintf(title, sizeof title, "%s - %s total - %s free - diskmap", app.zoom_path, tot, fr);
    } else snprintf(title, sizeof title, "diskmap");
    SDL_SetWindowTitle(app.win, title);
}

static void start_scan(const char *path)
{
    if (g_scan_running) return;
    load_theme();   /* pick up theme changes on reload */
    static char argpath[4096];
    if (!realpath(path, argpath)) snprintf(argpath, sizeof argpath, "%s", path);
    g_scan_running = 1; g_scan_abort = 0; g_scan_failed = 0;
    g_scan_err[0] = 0;
    g_prog_files = 0; g_prog_bytes = 0;
    pthread_t th;
    pthread_create(&th, NULL, scan_thread, argpath);
    pthread_detach(th);
}

static void zoom_to(uint32_t n)
{
    app.zoom = n;
    node_path(n, app.zoom_path, sizeof app.zoom_path);
    set_title();
    app.dirty = 1;
}

static void after_tree_swap(void)
{
    /* keep zoom position: strip the root prefix, then walk the rest */
    uint32_t cur = 0;
    char *p = app.zoom_path;
    size_t rpl = strlen(g_tree->root_path);
    if (rpl > 1 && !strncmp(p, g_tree->root_path, rpl)) p += rpl;
    while (*p == '/') p++;
    for (;;) {
        char seg[256]; size_t i = 0;
        while (*p && *p != '/' && i < 255) seg[i++] = *p++;
        seg[i] = 0;
        if (!i) break;
        int found = -1;
        Node *nd = &g_tree->nodes[cur];
        for (uint32_t k = 0; k < nd->n_kids; k++) {
            uint32_t ci = g_tree->kids[nd->kid_start + k];
            if (!strcmp(tree_name(g_tree, ci), seg)) { found = (int)ci; break; }
        }
        if (found < 0) break;
        cur = (uint32_t)found;
        while (*p == '/') p++;
    }
    zoom_to(cur);
}

/* ------------------------------------------------------------------ */
/* Tab completion for the Open field                                   */
/* ------------------------------------------------------------------ */

static void tab_complete(void)
{
    char dir[4096], frag[256];
    char *slash = strrchr(app.open_buf, '/');
    if (slash) {
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - app.open_buf) + 1, app.open_buf);
        snprintf(frag, sizeof frag, "%s", slash + 1);
    } else { snprintf(dir, sizeof dir, "."); snprintf(frag, sizeof frag, "%s", app.open_buf); }

    DIR *d = opendir(dir);
    if (!d) return;
    static char names[512][256];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 512) {
        if (de->d_name[0] == '.' && frag[0] != '.') continue;
        if (frag[0] && strncmp(de->d_name, frag, strlen(frag))) continue;
        snprintf(names[n], 256, "%s", de->d_name);
        n++;
    }
    closedir(d);
    if (!n) return;
    if (n == 1) {
        snprintf(app.open_buf, sizeof app.open_buf, "%s%s", dir, names[0]);
        app.open_len = (int)strlen(app.open_buf);
        char full[4096]; struct stat st;
        snprintf(full, sizeof full, "%s%s", dir, names[0]);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            strcat(app.open_buf, "/");
            app.open_len = (int)strlen(app.open_buf);
        }
        return;
    }
    size_t pl = strlen(frag), cp = 256;
    for (int i = 0; i < n; i++) {
        size_t j = 0;
        while (j < cp && names[i][j] && names[0][j] == names[i][j]) j++;
        if (j < cp) cp = j;
    }
    if (cp > pl) {
        snprintf(app.open_buf, sizeof app.open_buf, "%s%.*s", dir, (int)cp, names[0]);
        app.open_len = (int)strlen(app.open_buf);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void layout_toolbar(void)
{
    int x = 6;
    for (int i = 0; i < TB_COUNT; i++) {
        int w = text_w(tb_label[i]) + 18;
        tb_rect[i] = (SDL_Rect){x, 4, w, TOOLBAR_H - 8};
        x += w + 6;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("HOME");
    if (!path || !*path) path = ".";

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "diskmap: SDL: %s\n", SDL_GetError()); return 1; }
    app.win = SDL_CreateWindow("diskmap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1150, 720, SDL_WINDOW_RESIZABLE);
    g_rnd = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_rnd) g_rnd = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);
    SDL_GetWindowSize(app.win, &app.W, &app.H);
    font_init(15);
    load_theme();
    layout_toolbar();
    SDL_StartTextInput();

    snprintf(app.zoom_path, sizeof app.zoom_path, "%s", path);
    app.hover = FREE_NODE;
    app.show_free = 1;
    start_scan(path);

    int oneshot = getenv("DISKMAP_ONESHOT") != NULL;
    int quit = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 100)) {
            if (e.type == SDL_QUIT) quit = 1;
            else if (e.type == SDL_WINDOWEVENT &&
                     (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                      e.window.event == SDL_WINDOWEVENT_SHOWN)) {
                SDL_GetWindowSize(app.win, &app.W, &app.H); app.dirty = 1;
            }
            else if (e.type == SDL_KEYDOWN) {
                if (app.open_mode) {
                    if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                        if (app.open_buf[0]) {
                            snprintf(app.zoom_path, sizeof app.zoom_path, "%s", app.open_buf);
                            start_scan(app.open_buf);
                        }
                        app.open_mode = 0;
                    } else if (e.key.keysym.sym == SDLK_ESCAPE) app.open_mode = 0;
                    else if (e.key.keysym.sym == SDLK_BACKSPACE) { if (app.open_len) app.open_buf[--app.open_len] = 0; }
                    else if (e.key.keysym.sym == SDLK_TAB) tab_complete();
                    app.dirty = 1;
                } else {
                    switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: case SDLK_BACKSPACE:
                        if (g_tree && app.zoom < g_tree->n_nodes && app.zoom)
                            zoom_to(g_tree->nodes[app.zoom].parent);
                        break;
                    case SDLK_PLUS: case SDLK_EQUALS: case SDLK_RIGHT:
                        if (g_tree && app.zoom < g_tree->n_nodes && g_tree->nodes[app.zoom].n_kids) {
                            uint32_t c = g_tree->kids[g_tree->nodes[app.zoom].kid_start];
                            if (g_tree->nodes[c].n_kids) zoom_to(c);
                        }
                        break;
                    case SDLK_MINUS: case SDLK_KP_MINUS: case SDLK_LEFT:
                        if (g_tree && app.zoom < g_tree->n_nodes && app.zoom)
                            zoom_to(g_tree->nodes[app.zoom].parent);
                        break;
                    case SDLK_g: if (g_tree) zoom_to(0); break;
                    case SDLK_r: if (g_tree) start_scan(g_tree->root_path); break;
                    case SDLK_f: app.show_free = !app.show_free; app.dirty = 1; break;
                    case SDLK_o:
                        app.open_mode = 1;
                        snprintf(app.open_buf, sizeof app.open_buf, "%s", g_tree ? g_tree->root_path : "/");
                        app.open_len = (int)strlen(app.open_buf);
                        app.dirty = 1;
                        break;
                    }
                }
            }
            else if (e.type == SDL_TEXTINPUT && app.open_mode) {
                if (app.open_len < (int)sizeof(app.open_buf) - 2) {
                    app.open_buf[app.open_len++] = e.text.text[0];
                    app.open_buf[app.open_len] = 0;
                    app.dirty = 1;
                }
            }
            else if (e.type == SDL_MOUSEMOTION) {
                app.mouse_x = e.motion.x; app.mouse_y = e.motion.y;
                uint32_t h = hit_test(e.motion.x, e.motion.y);
                if (h != app.hover) { app.hover = h; app.dirty = 1; }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int x = e.button.x, y = e.button.y;
                if (y < TOOLBAR_H) {
                    for (int i = 0; i < TB_COUNT; i++)
                        if (x >= tb_rect[i].x && x < tb_rect[i].x + tb_rect[i].w) {
                            switch (i) {
                            case TB_OPEN:
                                app.open_mode = 1;
                                snprintf(app.open_buf, sizeof app.open_buf, "%s", g_tree ? g_tree->root_path : "/");
                                app.open_len = (int)strlen(app.open_buf);
                                break;
                            case TB_RELOAD: if (g_tree) start_scan(g_tree->root_path); break;
                            case TB_FULL: if (g_tree) zoom_to(0); break;
                            case TB_IN:
                                if (g_tree && app.zoom < g_tree->n_nodes && g_tree->nodes[app.zoom].n_kids) {
                                    uint32_t c = g_tree->kids[g_tree->nodes[app.zoom].kid_start];
                                    if (g_tree->nodes[c].n_kids) zoom_to(c);
                                }
                                break;
                            case TB_OUT:
                                if (g_tree && app.zoom < g_tree->n_nodes && app.zoom)
                                    zoom_to(g_tree->nodes[app.zoom].parent);
                                break;
                            case TB_FREE: app.show_free = !app.show_free; break;
                            }
                            app.dirty = 1;
                        }
                } else if (y < app.H - STATUS_H && g_tree) {
                    uint32_t h = hit_test(x, y);
                    if (h != FREE_NODE && h < g_tree->n_nodes && g_tree->nodes[h].n_kids)
                        zoom_to(h);
                }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && g_tree) {
                uint32_t h = hit_test(e.button.x, e.button.y);
                if (h != FREE_NODE && h < g_tree->n_nodes) {
                    char p[4096]; node_path(h, p, sizeof p);
                    SDL_SetClipboardText(p);
                }
            }
            else if (e.type == SDL_MOUSEWHEEL && g_tree && app.zoom < g_tree->n_nodes) {
                if (e.wheel.y > 0 && g_tree->nodes[app.zoom].n_kids) {
                    uint32_t c = g_tree->kids[g_tree->nodes[app.zoom].kid_start];
                    if (g_tree->nodes[c].n_kids) zoom_to(c);
                } else if (e.wheel.y < 0 && app.zoom) {
                    zoom_to(g_tree->nodes[app.zoom].parent);
                }
            }
        }

        /* tree swap check */
        if (pthread_mutex_trylock(&g_mtx) == 0) {
            static Tree *seen;
            if (g_tree && g_tree != seen) {
                seen = g_tree;
                const char *zenv = getenv("DISKMAP_ZOOM");
                if (zenv && *zenv) {
                    const char *rp = g_tree->root_path;
                    snprintf(app.zoom_path, sizeof app.zoom_path,
                             "%s%s%s", rp, rp[strlen(rp)-1] == '/' ? "" : "/", zenv);
                }
                after_tree_swap(); app.scan_done_once = 1;
            }
            pthread_mutex_unlock(&g_mtx);
        }

        if (app.dirty || g_scan_running) {
            pthread_mutex_lock(&g_mtx);
            if (app.dirty) { layout_all(); app.dirty = 0; }
            render();
            pthread_mutex_unlock(&g_mtx);
        }
        if (oneshot && !g_scan_running && (app.scan_done_once || g_scan_failed)) {
            pthread_mutex_lock(&g_mtx);
            if (g_tree) printf("scanned %zu nodes, root size %llu bytes, %u files, %u folders\n",
                g_tree->n_nodes, (unsigned long long)g_tree->nodes[0].size,
                g_tree->nodes[0].nfiles, g_tree->nodes[0].nfolders);
            const char *shot = getenv("DISKMAP_SHOT");
            if (shot) {
                SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, app.W, app.H, 32, SDL_PIXELFORMAT_RGBA32);
                SDL_RenderReadPixels(g_rnd, NULL, SDL_PIXELFORMAT_RGBA32, s->pixels, s->pitch);
                SDL_SaveBMP(s, shot);
                SDL_FreeSurface(s);
            }
            pthread_mutex_unlock(&g_mtx);
            break;
        }
    }
    SDL_Quit();
    return 0;
}
