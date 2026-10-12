/* HUDDLE - NFL Performance Viewer (C edition)
 *
 * SDL3 front end. Immediate-mode widgets drawn with SDL's built-in 8x8 font and
 * the original SNES palette. Data comes from:
 *   1. the 2025 season snapshot compiled into the executable (embedded.c), and
 *   2. the current season, downloaded from ESPN by the Update buttons on a
 *      worker thread and cached in ~/.huddle/.
 */
#include <SDL3/SDL.h>
#include <pthread.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "embedded.h"
#include "espn.h"
#include "http.h"
#include "model.h"
#include "palette.h"
#include "store.h"
#include "util.h"
#include "views.h"

enum { V_STANDINGS, V_TEAM, V_ROSTER, V_PLAYER };
enum { DD_NONE, DD_TEAM, DD_SEASON, DD_SNAP };

#define PAD 12.0f
#define MAX_SEASONS 2

typedef struct {
    pthread_t th;
    bool running;
    Season *work;
    int slot;
    UpdateOpts opts;
    char team_id[8];
    pthread_mutex_t mu;
    char msg[160];
    bool ok;
    char summary[256];
} Job;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    float d;            /* pixel density (2 on Retina)         */
    float ui;           /* text zoom: glyph = 8*ui points      */
    float cw, lh;       /* char width / line height, in points */
    int ww, wh;
    float mx, my;
    bool click;

    Season *seasons[MAX_SEASONS];
    bool from_embedded[MAX_SEASONS];
    int nseasons, sel;

    char team_id[8];
    int view;
    bool post;          /* show postseason numbers instead of regular season */
    int player_idx;
    char emb_fetched[24]; /* when the built-in snapshot was downloaded */
    int emb_year;

    Lines lines;
    bool dirty;
    int built_chars;
    float scroll;
    int vis_lines;

    int open_dd;
    float dd_x, dd_y, dd_w;
    float dd_scroll;

    Job job;
    Uint32 ev_job;
    char status[300];
    bool status_warn;
    bool offline;
} A;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
/* A saved snapshot being browsed (read-only). NULL = the live/latest data. */
static Season *g_snap_view;
static int g_snap_sel; /* 0 = latest, 1.. = index into g_snaps + 1 */

static Season *cur_season(void) { return g_snap_view ? g_snap_view : A.seasons[A.sel]; }

static int find_team_idx(const Season *s, const char *id)
{
    for (int i = 0; i < s->nteams; i++)
        if (!strcmp(s->teams[i].id, id))
            return i;
    return -1;
}

/* Team indices sorted by name. */
static int team_order(const Season *s, int *out)
{
    int n = 0;
    for (int i = 0; i < s->nteams; i++) {
        int j = n++;
        while (j > 0 && strcmp(s->teams[i].name, s->teams[out[j - 1]].name) < 0) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}

static void set_status(bool warn, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.status, sizeof A.status, fmt, ap);
    va_end(ap);
    A.status_warn = warn;
}

static const char *cache_dir(void)
{
    static char dir[512];
    const char *home = getenv("HOME");
    snprintf(dir, sizeof dir, "%s/.huddle", home ? home : ".");
    return dir;
}

static const char *cache_path(int year)
{
    static char p[600];
    snprintf(p, sizeof p, "%s/season_%d.json", cache_dir(), year);
    return p;
}

static void select_view(int v)
{
    A.view = v;
    A.dirty = true;
    A.scroll = 0;
}

static void select_team(int idx)
{
    Season *s = cur_season();
    if (idx < 0 || idx >= s->nteams)
        return;
    copy_str(A.team_id, sizeof A.team_id, s->teams[idx].id);
    if (A.view == V_STANDINGS)
        A.view = V_TEAM;
    else if (A.view == V_PLAYER)
        A.view = V_ROSTER;
    A.dirty = true;
    A.scroll = 0;
}

static void snapshots_reset(void);
static void snapshots_rescan(void);

static void select_season(int slot)
{
    if (slot < 0 || slot >= A.nseasons || slot == A.sel)
        return;
    A.sel = slot;
    snapshots_reset();
    Season *s = cur_season();
    if (find_team_idx(s, A.team_id) < 0 && s->nteams)
        copy_str(A.team_id, sizeof A.team_id, s->teams[0].id);
    if (A.view == V_PLAYER)
        A.view = V_ROSTER;
    A.dirty = true;
    A.scroll = 0;
}

/* ------------------------------------------------------------------ */
/* data loading                                                        */
/* ------------------------------------------------------------------ */
static Season *load_slot(Season *embedded, int year, bool *from_embedded)
{
    Season *s = embedded && embedded->year == year ? embedded : NULL;
    *from_embedded = s != NULL;
    Season *c = store_load_file(cache_path(year));
    if (c && c->year == year && (!s || strcmp(c->fetched_at, s->fetched_at) > 0)) {
        if (s != embedded)
            season_free(s);
        s = c;
        *from_embedded = false;
    } else {
        season_free(c);
    }
    return s;
}

static void load_data(void)
{
    Season *emb = store_load_mem(embedded_season_start, (size_t)(embedded_season_end - embedded_season_start));
    if (emb) {
        A.emb_year = emb->year;
        copy_str(A.emb_fetched, sizeof A.emb_fetched, emb->fetched_at);
    }
    int cur = espn_current_season_year();
    int years[MAX_SEASONS];
    int n = 0;
    if (emb)
        years[n++] = emb->year;
    if (!emb || cur > emb->year)
        years[n++] = cur;

    Season *skeleton_src = NULL;
    for (int i = 0; i < n; i++) {
        Season *s = load_slot(emb, years[i], &A.from_embedded[A.nseasons]);
        if (!s) {
            /* Brand-new season: start from last season's teams so the UI is usable offline. */
            s = skeleton_src ? season_skeleton(skeleton_src, years[i]) : season_new(years[i]);
        }
        A.seasons[A.nseasons++] = s;
        if (s->nteams)
            skeleton_src = s;
    }
    /* An embedded season that was superseded by a newer cache must not be freed twice. */
    if (emb) {
        bool used = false;
        for (int i = 0; i < A.nseasons; i++)
            used |= A.seasons[i] == emb;
        if (!used)
            season_free(emb);
    }
}

/* ------------------------------------------------------------------ */
/* background update                                                   */
/* ------------------------------------------------------------------ */
static void job_progress(void *ud, const char *msg)
{
    Job *j = ud;
    pthread_mutex_lock(&j->mu);
    copy_str(j->msg, sizeof j->msg, msg);
    pthread_mutex_unlock(&j->mu);
}

static void *job_main(void *p)
{
    Job *j = p;
    j->opts.team_id = j->team_id;
    j->ok = espn_update(j->work, &j->opts, job_progress, j, j->summary, sizeof j->summary);
    if (j->ok) {
        mkdir(cache_dir(), 0755);
        bool saved = store_save(j->work, cache_path(j->work->year));
        /* Every successful update also leaves a timestamped, browsable snapshot. */
        char dir[700], path[800], ts[32], tag[32] = "quick";
        snprintf(dir, sizeof dir, "%s/history", cache_dir());
        mkdir(dir, 0755);
        snprintf(dir, sizeof dir, "%s/history/%d", cache_dir(), j->work->year);
        mkdir(dir, 0755);
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d_%H%M%S", &tmv);
        if (j->opts.all_rosters)
            copy_str(tag, sizeof tag, "all");
        else if (j->team_id[0]) {
            Team *t = season_find_team(j->work, j->team_id);
            copy_str(tag, sizeof tag, t && t->abbr[0] ? t->abbr : "team");
        }
        snprintf(path, sizeof path, "%s/%s_%s.json", dir, ts, tag);
        saved &= store_save(j->work, path);
        if (!saved)
            snprintf(j->summary + strlen(j->summary), sizeof j->summary - strlen(j->summary), " (could not write cache)");
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = A.ev_job;
    SDL_PushEvent(&ev);
    return NULL;
}

static void start_job(int slot, bool all)
{
    if (A.job.running || A.offline || g_snap_view)
        return;
    Job *j = &A.job;
    j->slot = slot;
    j->work = season_clone(A.seasons[slot]); /* worker owns this copy; UI never shares mutable state */
    memset(&j->opts, 0, sizeof j->opts);
    j->opts.all_rosters = all;
    j->team_id[0] = 0;
    if (!all && slot == A.sel)
        copy_str(j->team_id, sizeof j->team_id, A.team_id);
    j->msg[0] = 0;
    j->ok = false;
    j->running = true;
    if (pthread_create(&j->th, NULL, job_main, j) != 0) {
        season_free(j->work);
        j->running = false;
        set_status(true, "Could not start the update thread.");
    }
}

static void finish_job(void)
{
    Job *j = &A.job;
    if (!j->running)
        return;
    pthread_join(j->th, NULL);
    j->running = false;
    if (j->ok) {
        season_free(A.seasons[j->slot]);
        A.seasons[j->slot] = j->work;
        A.from_embedded[j->slot] = false;
        set_status(false, "%s", j->summary);
        A.dirty = true;
        if (j->slot == A.sel)
            snapshots_rescan();
    } else {
        season_free(j->work);
        set_status(true, "%s", j->summary);
    }
    j->work = NULL;
}

/* ------------------------------------------------------------------ */
/* saved snapshots (history)                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    char path[800];
    char label[72];
    bool builtin; /* the snapshot compiled into the executable */
} SnapEnt;

static SnapEnt *g_snaps;
static int g_nsnaps;

static int snap_cmp_desc(const void *a, const void *b) { return strcmp(((const SnapEnt *)b)->path, ((const SnapEnt *)a)->path); }

/* "2026-10-10_162805_KC.json" -> "2026-10-10 16:28:05  (standings, team stats + KC roster)" */
static void snap_label(const char *file, char *out, size_t n)
{
    int Y, M, D, h, m, sec;
    char tag[32] = "";
    if (sscanf(file, "%4d-%2d-%2d_%2d%2d%2d_%31[^.]", &Y, &M, &D, &h, &m, &sec, tag) >= 6) {
        char what[64];
        if (!strcmp(tag, "quick") || !tag[0])
            snprintf(what, sizeof what, "standings + team stats");
        else if (!strcmp(tag, "all"))
            snprintf(what, sizeof what, "standings, stats + all rosters");
        else
            snprintf(what, sizeof what, "standings, stats + %s roster", tag);
        snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d  (%s)", Y, M, D, h, m, sec, what);
    } else {
        snprintf(out, n, "%s", file);
    }
}

static void snapshots_rescan(void)
{
    free(g_snaps);
    g_snaps = NULL;
    g_nsnaps = 0;
    int year = A.seasons[A.sel]->year;
    int cap = 0;
    if (year == A.emb_year && !A.from_embedded[A.sel]) {
        cap = 16;
        g_snaps = calloc((size_t)cap, sizeof *g_snaps);
        g_snaps[g_nsnaps].builtin = true;
        snprintf(g_snaps[g_nsnaps].label, sizeof g_snaps[0].label, "%s  (built-in baseline)", A.emb_fetched);
        g_nsnaps++;
    }
    char dir[700];
    snprintf(dir, sizeof dir, "%s/history/%d", cache_dir(), year);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0 || e->d_name[0] == '.')
            continue;
        if (g_nsnaps == cap) {
            cap = cap ? cap * 2 : 16;
            g_snaps = realloc(g_snaps, (size_t)cap * sizeof *g_snaps);
        }
        SnapEnt *se = &g_snaps[g_nsnaps++];
        memset(se, 0, sizeof *se);
        snprintf(se->path, sizeof se->path, "%s/%s", dir, e->d_name);
        snap_label(e->d_name, se->label, sizeof se->label);
    }
    closedir(d);
    /* newest first; the built-in baseline (if any) is the oldest, so keep it last */
    int first = (g_nsnaps && g_snaps[0].builtin) ? 1 : 0;
    qsort(g_snaps + first, (size_t)(g_nsnaps - first), sizeof *g_snaps, snap_cmp_desc);
    if (first) {
        SnapEnt b = g_snaps[0];
        memmove(g_snaps, g_snaps + 1, (size_t)(g_nsnaps - 1) * sizeof *g_snaps);
        g_snaps[g_nsnaps - 1] = b;
    }
}

static void snapshots_reset(void)
{
    season_free(g_snap_view);
    g_snap_view = NULL;
    g_snap_sel = 0;
    snapshots_rescan();
}

static void select_snapshot(int i) /* 0 = latest */
{
    if (i == g_snap_sel)
        return;
    Season *loaded = NULL;
    if (i > 0 && i <= g_nsnaps) {
        const SnapEnt *se = &g_snaps[i - 1];
        loaded = se->builtin ? store_load_mem(embedded_season_start, (size_t)(embedded_season_end - embedded_season_start))
                             : store_load_file(se->path);
        if (!loaded) {
            set_status(true, "Could not read that snapshot.");
            return;
        }
    }
    season_free(g_snap_view);
    g_snap_view = loaded;
    g_snap_sel = loaded ? i : 0;
    if (find_team_idx(cur_season(), A.team_id) < 0 && cur_season()->nteams)
        copy_str(A.team_id, sizeof A.team_id, cur_season()->teams[0].id);
    if (A.view == V_PLAYER)
        A.view = V_ROSTER;
    A.status[0] = 0;
    A.dirty = true;
    A.scroll = 0;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */
static void set_col(uint32_t rgb, int a) { SDL_SetRenderDrawColor(A.ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (Uint8)a); }

static void fill(float x, float y, float w, float h, uint32_t rgb, int a)
{
    SDL_FRect r = {x, y, w, h};
    set_col(rgb, a);
    SDL_RenderFillRect(A.ren, &r);
}

static void outline(float x, float y, float w, float h, uint32_t rgb)
{
    SDL_FRect r = {x, y, w, h};
    set_col(rgb, 255);
    SDL_RenderRect(A.ren, &r);
}

static void text_at(float x, float y, const char *s, uint32_t rgb)
{
    set_col(rgb, 255);
    SDL_SetRenderScale(A.ren, A.d * A.ui, A.d * A.ui);
    SDL_RenderDebugText(A.ren, x / A.ui, (y + (A.lh - 8 * A.ui) / 2) / A.ui, s);
    SDL_SetRenderScale(A.ren, A.d, A.d);
}

static float text_w(const char *s) { return (float)strlen(s) * A.cw; }

static bool inside(float x, float y, float w, float h) { return A.mx >= x && A.mx < x + w && A.my >= y && A.my < y + h; }

static bool hot(float x, float y, float w, float h) { return A.open_dd == DD_NONE && inside(x, y, w, h); }

static bool take_click(float x, float y, float w, float h)
{
    if (A.click && hot(x, y, w, h)) {
        A.click = false;
        return true;
    }
    return false;
}

static float btn_h(void) { return A.lh + 10; }

/* Flat SNES-style button. Returns true when clicked. */
static bool button(float x, float y, const char *label, bool enabled, bool active, float *w_out)
{
    float w = (float)(strlen(label) + 2) * A.cw, h = btn_h();
    bool over = enabled && hot(x, y, w, h);
    uint32_t bg = active ? SNES_PURPLE : (over ? SNES_PURPLE : SNES_LAVENDER);
    uint32_t fg = (active || over) ? SNES_WHITE : SNES_BLACK;
    fill(x, y, w, h, bg, enabled ? 255 : 110);
    if (!enabled)
        fg = SNES_DARK_GRAY;
    text_at(x + A.cw, y + 5, label, fg);
    if (w_out)
        *w_out = w;
    return enabled && take_click(x, y, w, h);
}

static void draw_arrow(float cx, float cy, uint32_t rgb)
{
    float u = A.cw / 4;
    for (int i = 0; i < 4; i++)
        fill(cx - (4 - i) * u * 0.6f, cy + i * u * 0.6f, (4 - i) * u * 1.2f, u * 0.6f, rgb, 255);
}

/* Combo-box header; returns true when clicked (caller opens the popup). */
static bool dropdown(float x, float y, float w, const char *label, int which)
{
    float h = btn_h();
    bool over = hot(x, y, w, h);
    fill(x, y, w, h, SNES_LAVENDER, 255);
    outline(x, y, w, h, over || A.open_dd == which ? SNES_PURPLE : SNES_DARK_GRAY);
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.*s", (int)((w - 3 * A.cw) / A.cw), label);
    text_at(x + A.cw * 0.7f, y + 5, tmp, SNES_BLACK);
    draw_arrow(x + w - A.cw * 1.2f, y + h / 2 - A.cw * 0.5f, SNES_PURPLE);
    if (which == DD_TEAM || which == DD_SEASON || which == DD_SNAP) {
        if (A.open_dd == which) {
            A.dd_x = x;
            A.dd_y = y + h;
            A.dd_w = w;
        }
    }
    return take_click(x, y, w, h);
}

/* ------------------------------------------------------------------ */
/* panel                                                               */
/* ------------------------------------------------------------------ */
static int panel_chars(void) { return (int)((A.ww - 2 * PAD - 2 * 8 - 14) / A.cw); }

static void rebuild(void)
{
    Season *s = cur_season();
    int chars = panel_chars();
    int ti = find_team_idx(s, A.team_id);
    if (ti < 0 && A.view != V_STANDINGS)
        A.view = V_STANDINGS;
    if (A.view == V_PLAYER && (ti < 0 || A.player_idx < 0 || A.player_idx >= s->teams[ti].nplayers))
        A.view = V_ROSTER;
    switch (A.view) {
    case V_STANDINGS:
        build_standings(&A.lines, s, chars, A.post);
        break;
    case V_TEAM:
        build_team(&A.lines, s, &s->teams[ti], chars, A.post);
        break;
    case V_ROSTER:
        build_roster(&A.lines, s, &s->teams[ti], ti, chars);
        break;
    case V_PLAYER:
        build_player(&A.lines, s, &s->teams[ti], &s->teams[ti].players[A.player_idx], chars, A.post);
        break;
    }
    A.built_chars = chars;
    A.dirty = false;
}

static void draw_panel(float px, float py, float pw, float ph)
{
    fill(px, py, pw, ph, SNES_LAVENDER, 255);
    float in = 8;
    float x0 = px + in, y0 = py + in;
    A.vis_lines = (int)((ph - 2 * in) / A.lh);
    if (A.vis_lines < 1)
        A.vis_lines = 1;
    float max_scroll = (float)(A.lines.n - A.vis_lines);
    if (max_scroll < 0)
        max_scroll = 0;
    if (A.scroll > max_scroll)
        A.scroll = max_scroll;
    if (A.scroll < 0)
        A.scroll = 0;
    int first = (int)A.scroll;

    for (int r = 0; r < A.vis_lines && first + r < A.lines.n; r++) {
        const Line *ln = &A.lines.v[first + r];
        float y = y0 + r * A.lh;
        for (int c = 0; c < ln->n; c++) {
            const Cell *cell = &ln->c[c];
            float x = x0 + cell->x * A.cw;
            float w = (float)cell->w * A.cw;
            bool clickable = cell->player >= 0 || cell->team >= 0;
            if (cell->heading) {
                fill(x - 3, y, w + 6, A.lh, cell->accent ? SNES_WARNING : SNES_PURPLE, 255);
            } else if (clickable && cell->text[0] && hot(x - 3, y, w + 6, A.lh)) {
                fill(x - 3, y, w + 6, A.lh, SNES_PURPLE, 70);
            }
            if (cell->text[0]) {
                char tmp[200];
                snprintf(tmp, sizeof tmp, "%.*s", cell->w, cell->text);
                text_at(x, y, tmp, cell->fg);
            }
            if (clickable && cell->text[0] && take_click(x - 3, y, w + 6, A.lh)) {
                if (cell->player >= 0) {
                    A.player_idx = cell->player;
                    select_view(V_PLAYER);
                } else {
                    select_team(cell->team);
                    select_view(V_TEAM);
                }
            }
        }
    }

    if (max_scroll > 0) { /* scrollbar */
        float sx = px + pw - 12, sh = ph - 2 * in;
        fill(sx, y0, 6, sh, SNES_LIGHT_GRAY, 160);
        float th = sh * A.vis_lines / (float)A.lines.n;
        if (th < 16)
            th = 16;
        fill(sx, y0 + (sh - th) * (A.scroll / max_scroll), 6, th, SNES_PURPLE, 255);
    }
}

/* ------------------------------------------------------------------ */
/* popups                                                              */
/* ------------------------------------------------------------------ */
static void draw_popup(void)
{
    if (A.open_dd == DD_NONE)
        return;
    Season *s = cur_season();
    int cap = (A.open_dd == DD_SNAP ? g_nsnaps + 1 : (A.open_dd == DD_TEAM ? s->nteams : A.nseasons)) + 1;
    char(*labels)[80] = calloc((size_t)cap, sizeof *labels);
    int *order = calloc((size_t)cap, sizeof *order);
    int n = 0, cur_i = -1;
    if (A.open_dd == DD_TEAM) {
        n = team_order(s, order);
        int ti = find_team_idx(s, A.team_id);
        for (int i = 0; i < n; i++) {
            snprintf(labels[i], sizeof labels[i], "%s", s->teams[order[i]].name);
            if (order[i] == ti)
                cur_i = i;
        }
    } else if (A.open_dd == DD_SEASON) {
        for (int i = 0; i < A.nseasons; i++) {
            snprintf(labels[i], sizeof labels[i], "%d%s", A.seasons[i]->year, A.from_embedded[i] ? "  (built in)" : "  (updated)");
            order[i] = i;
        }
        n = A.nseasons;
        cur_i = A.sel;
    } else {
        snprintf(labels[0], sizeof labels[0], "Latest (live data)");
        for (int i = 0; i < g_nsnaps; i++)
            snprintf(labels[i + 1], sizeof labels[i + 1], "%s", g_snaps[i].label);
        for (int i = 0; i <= g_nsnaps; i++)
            order[i] = i;
        n = g_nsnaps + 1;
        cur_i = g_snap_sel;
    }
    float rowh = A.lh + 6;
    float w = A.dd_w;
    for (int i = 0; i < n; i++) {
        float lw = (float)(strlen(labels[i]) + 2) * A.cw;
        if (lw > w)
            w = lw;
    }
    if (A.dd_x + w > (float)A.ww - 4)
        A.dd_x = (float)A.ww - 4 - w;
    int vis = (int)((A.wh - A.dd_y - 8) / rowh);
    if (vis > n)
        vis = n;
    if (vis < 1)
        vis = 1;
    float max_scroll = (float)(n - vis);
    if (A.dd_scroll > max_scroll)
        A.dd_scroll = max_scroll;
    if (A.dd_scroll < 0)
        A.dd_scroll = 0;
    float h = vis * rowh;
    fill(A.dd_x, A.dd_y, w, h, SNES_LIGHT_GRAY, 255);
    outline(A.dd_x, A.dd_y, w, h, SNES_PURPLE);

    int first = (int)A.dd_scroll;
    int picked = -1;
    for (int r = 0; r < vis; r++) {
        int i = first + r;
        float y = A.dd_y + r * rowh;
        bool over = inside(A.dd_x, y, w, rowh);
        if (over)
            fill(A.dd_x + 1, y, w - 2, rowh, SNES_PURPLE, 255);
        else if (i == cur_i)
            fill(A.dd_x + 1, y, w - 2, rowh, SNES_LAVENDER, 255);
        text_at(A.dd_x + A.cw, y + 3, labels[i], over ? SNES_WHITE : SNES_BLACK);
        if (over && A.click)
            picked = i;
    }
    if (A.click) {
        if (picked >= 0) {
            if (A.open_dd == DD_TEAM)
                select_team(order[picked]);
            else if (A.open_dd == DD_SEASON)
                select_season(order[picked]);
            else
                select_snapshot(order[picked]);
        }
        A.open_dd = DD_NONE; /* click inside picks, click outside just closes */
        A.click = false;
    }
    free(labels);
    free(order);
}

/* ------------------------------------------------------------------ */
/* one frame                                                           */
/* ------------------------------------------------------------------ */
static void frame(void)
{
    SDL_GetWindowSize(A.win, &A.ww, &A.wh);
    A.d = SDL_GetWindowPixelDensity(A.win);
    if (A.d < 1)
        A.d = 1;
    A.cw = 8 * A.ui;
    A.lh = 12 * A.ui;
    SDL_SetRenderScale(A.ren, A.d, A.d);

    Season *s = cur_season();
    if (A.dirty || panel_chars() != A.built_chars)
        rebuild();

    set_col(SNES_LIGHT_GRAY, 255);
    SDL_RenderClear(A.ren);

    /* header strip */
    float hh = 2 * A.lh + 2 * PAD;
    fill(0, 0, (float)A.ww, hh, SNES_DARK_GRAY, 255);
    text_at(PAD, PAD, "HUDDLE - NFL Performance Viewer", SNES_WHITE);
    text_at(PAD, PAD + A.lh, "Built-in 2025 season + live current season from ESPN", SNES_WHITE);

    /* toolbar: team, season, update buttons */
    bool busy = A.job.running;
    float y = hh + PAD;
    float x = PAD;
    text_at(x, y + 5, "Team:", SNES_BLACK);
    x += text_w("Team:") + A.cw;
    int ti = find_team_idx(s, A.team_id);
    float team_w = 27 * A.cw;
    if (dropdown(x, y, team_w, ti >= 0 ? s->teams[ti].name : "(no teams)", DD_TEAM) && s->nteams) {
        A.open_dd = DD_TEAM;
        A.dd_scroll = 0;
    }
    if (A.open_dd == DD_TEAM) {
        A.dd_x = x;
        A.dd_y = y + btn_h();
        A.dd_w = team_w;
    }
    x += team_w + 2 * A.cw;
    text_at(x, y + 5, "Season:", SNES_BLACK);
    x += text_w("Season:") + A.cw;
    char yr[16];
    snprintf(yr, sizeof yr, "%d", s->year);
    float season_w = 9 * A.cw;
    if (dropdown(x, y, season_w, yr, DD_SEASON)) {
        A.open_dd = DD_SEASON;
        A.dd_scroll = 0;
    }
    if (A.open_dd == DD_SEASON) {
        A.dd_x = x;
        A.dd_y = y + btn_h();
        A.dd_w = season_w;
    }
    x += season_w + 2 * A.cw;
    float bw;
    if (button(x, y, "Update", !busy && !A.offline && !g_snap_view, false, &bw))
        start_job(A.sel, false);
    x += bw + A.cw;
    if (button(x, y, "Update All Teams", !busy && !A.offline && !g_snap_view, false, &bw))
        start_job(A.sel, true);

    /* view tabs */
    y += btn_h() + 8;
    x = PAD;
    if (button(x, y, "Standings", true, A.view == V_STANDINGS, &bw))
        select_view(V_STANDINGS);
    x += bw + A.cw;
    if (button(x, y, "Team", ti >= 0, A.view == V_TEAM, &bw))
        select_view(V_TEAM);
    x += bw + A.cw;
    if (button(x, y, "Roster", ti >= 0, A.view == V_ROSTER || A.view == V_PLAYER, &bw))
        select_view(V_ROSTER);
    x += bw + A.cw;
    if (A.view == V_PLAYER && button(x, y, "< Back to roster", true, false, &bw))
        select_view(V_ROSTER);

    /* row 3: regular season / postseason (kept strictly separate) + snapshot picker */
    y += btn_h() + 8;
    x = PAD;
    text_at(x, y + 5, "Stats:", SNES_BLACK);
    x += text_w("Stats:") + A.cw;
    if (button(x, y, "Regular Season", true, !A.post, &bw) && A.post) {
        A.post = false;
        A.dirty = true;
        A.scroll = 0;
    }
    x += bw + A.cw;
    if (button(x, y, "Postseason", true, A.post, &bw) && !A.post) {
        A.post = true;
        A.dirty = true;
        A.scroll = 0;
    }
    x += bw + 3 * A.cw;
    text_at(x, y + 5, "Snapshot:", SNES_BLACK);
    x += text_w("Snapshot:") + A.cw;
    float snap_w = 46 * A.cw;
    const char *snap_label = g_snap_sel == 0 ? "Latest (live data)" : g_snaps[g_snap_sel - 1].label;
    if (dropdown(x, y, snap_w, snap_label, DD_SNAP)) {
        snapshots_rescan();
        A.open_dd = DD_SNAP;
        A.dd_scroll = 0;
    }
    if (A.open_dd == DD_SNAP) {
        A.dd_x = x;
        A.dd_y = y + btn_h();
        A.dd_w = snap_w;
    }

    /* status line */
    y += btn_h() + 8;
    float sh = A.lh + 8;
    char st[320];
    bool warn = A.status_warn;
    if (busy) {
        pthread_mutex_lock(&A.job.mu);
        snprintf(st, sizeof st, "Updating %d: %s", A.seasons[A.job.slot]->year, A.job.msg[0] ? A.job.msg : "starting...");
        pthread_mutex_unlock(&A.job.mu);
        warn = true;
    } else if (g_snap_view) {
        snprintf(st, sizeof st, "Viewing a SAVED SNAPSHOT from %s (read-only). Pick \"Latest\" in Snapshot to update again.",
                 g_snap_view->fetched_at[0] ? g_snap_view->fetched_at : "an earlier time");
        warn = true;
    } else if (A.status[0]) {
        snprintf(st, sizeof st, "%s", A.status);
    } else {
        snprintf(st, sizeof st, "%d %s  |  standings as of %s  |  %s", s->year, A.post ? "POSTSEASON" : "regular season", s->standings_at[0] ? s->standings_at : "never",
                 A.from_embedded[A.sel] ? "built-in snapshot" : "cached from ESPN");
    }
    if (warn)
        fill(PAD, y, A.ww - 2 * PAD, sh, SNES_WARNING, 255);
    text_at(PAD + 6, y + 4, st, SNES_BLACK);

    /* output panel */
    y += sh + 8;
    draw_panel(PAD, y, A.ww - 2 * PAD, A.wh - y - PAD);

    draw_popup();
    A.click = false;
}

/* ------------------------------------------------------------------ */
/* CLI: text dump + screenshot (used for testing without a display)    */
/* ------------------------------------------------------------------ */
static int find_team_arg(const Season *s, const char *arg)
{
    for (int i = 0; i < s->nteams; i++)
        if (!strcasecmp(s->teams[i].abbr, arg) || strcasestr(s->teams[i].name, arg))
            return i;
    return -1;
}

static int dump_mode(const char *what, int year, const char *team_arg, const char *player_arg, bool dump_post)
{
    load_data();
    int slot = 0;
    for (int i = 0; i < A.nseasons; i++)
        if (A.seasons[i]->year == year)
            slot = i;
    Season *s = A.seasons[slot];
    int ti = team_arg ? find_team_arg(s, team_arg) : (s->nteams ? 0 : -1);
    Lines L = {0};
    int chars = 130;
    if (!strcmp(what, "standings")) {
        build_standings(&L, s, chars, dump_post);
    } else if (ti < 0) {
        fprintf(stderr, "no such team\n");
        return 1;
    } else if (!strcmp(what, "team")) {
        build_team(&L, s, &s->teams[ti], chars, dump_post);
    } else if (!strcmp(what, "roster")) {
        build_roster(&L, s, &s->teams[ti], ti, chars);
    } else if (!strcmp(what, "player")) {
        int pi = -1;
        for (int i = 0; i < s->teams[ti].nplayers; i++)
            if (player_arg && strcasestr(s->teams[ti].players[i].name, player_arg))
                pi = i;
        if (pi < 0) {
            fprintf(stderr, "no such player on that team\n");
            return 1;
        }
        build_player(&L, s, &s->teams[ti], &s->teams[ti].players[pi], chars, dump_post);
    } else {
        fprintf(stderr, "dump: standings|team|roster|player\n");
        return 1;
    }
    for (int i = 0; i < L.n; i++) {
        int pos = 0;
        for (int c = 0; c < L.v[i].n; c++) {
            const Cell *cell = &L.v[i].c[c];
            if (cell->x > pos)
                printf("%*s", cell->x - pos, "");
            printf("%s", cell->text);
            pos = (cell->x > pos ? cell->x : pos) + (int)strlen(cell->text);
        }
        printf("\n");
    }
    lines_free(&L);
    return 0;
}

int main(int argc, char **argv)
{
    const char *shot = NULL, *view = NULL, *team_arg = NULL, *player_arg = NULL, *dump = NULL;
    int want_year = 0, want_snap = 0;
    float ui = 0;
    float click_x[8], click_y[8]; /* testing: synthetic left clicks, one every 3 frames */
    int nclicks = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--shot") && more) shot = argv[++i];
        else if (!strcmp(a, "--view") && more) view = argv[++i];
        else if (!strcmp(a, "--team") && more) team_arg = argv[++i];
        else if (!strcmp(a, "--player") && more) player_arg = argv[++i];
        else if (!strcmp(a, "--season") && more) want_year = atoi(argv[++i]);
        else if (!strcmp(a, "--dump") && more) dump = argv[++i];
        else if (!strcmp(a, "--ui") && more) ui = (float)atof(argv[++i]);
        else if (!strcmp(a, "--offline")) A.offline = true;
        else if (!strcmp(a, "--post")) A.post = true;
        else if (!strcmp(a, "--snap") && more) want_snap = atoi(argv[++i]);
        else if (!strcmp(a, "--click") && more && nclicks < 8) sscanf(argv[++i], "%f,%f", &click_x[nclicks], &click_y[nclicks]), nclicks++;
        else if (!strcmp(a, "--help")) {
            printf("huddle [--season YEAR] [--team ABBR] [--offline]\n"
                   "       huddle --dump standings|team|roster|player [--season Y] [--team ABBR] [--player NAME] [--post]\n"
                   "       huddle --shot out.bmp [--view standings|team|roster|player] ...   (testing)\n");
            return 0;
        }
    }

    if (dump)
        return dump_mode(dump, want_year ? want_year : 2025, team_arg, player_arg, A.post);

    http_init();
    pthread_mutex_init(&A.job.mu, NULL);
    load_data();
    A.sel = A.nseasons - 1;
    if (want_year)
        for (int i = 0; i < A.nseasons; i++)
            if (A.seasons[i]->year == want_year)
                A.sel = i;
    {
        Season *s = cur_season();
        int order[64];
        int n = team_order(s, order);
        int ti = team_arg ? find_team_arg(s, team_arg) : -1;
        if (ti >= 0)
            copy_str(A.team_id, sizeof A.team_id, s->teams[ti].id);
        else if (n)
            copy_str(A.team_id, sizeof A.team_id, s->teams[order[0]].id);
    }
    A.view = V_STANDINGS;
    A.dirty = true;
    snapshots_rescan();
    if (want_snap > 0)
        select_snapshot(want_snap);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    A.win = SDL_CreateWindow("HUDDLE - NFL Dashboard", 1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!A.win) {
        fprintf(stderr, "window failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(A.win, 760, 480);
    A.ren = SDL_CreateRenderer(A.win, NULL);
    if (!A.ren) {
        fprintf(stderr, "renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(A.ren, 1);
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
    A.ev_job = SDL_RegisterEvents(1);
    A.d = SDL_GetWindowPixelDensity(A.win);
    A.ui = ui > 0 ? ui : (A.d >= 2 ? 1.5f : 2.0f);

    /* First launch of a new season: pull standings + team stats so there is something to look at. */
    if (!A.offline && !shot && A.seasons[A.sel]->standings_at[0] == 0)
        start_job(A.sel, false);

    if (view) {
        if (!strcmp(view, "team")) A.view = V_TEAM;
        else if (!strcmp(view, "roster")) A.view = V_ROSTER;
        else if (!strcmp(view, "player")) {
            Season *s = cur_season();
            int ti = find_team_idx(s, A.team_id);
            A.view = V_ROSTER;
            if (ti >= 0)
                for (int i = 0; i < s->teams[ti].nplayers; i++)
                    if (player_arg && strcasestr(s->teams[ti].players[i].name, player_arg)) {
                        A.player_idx = i;
                        A.view = V_PLAYER;
                    }
        }
    }

    int shot_frames = 0;
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, shot ? 20 : 60);
        while (got) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
            } else if (e.type == A.ev_job) {
                finish_job();
            } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                A.mx = e.motion.x;
                A.my = e.motion.y;
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                A.mx = e.button.x;
                A.my = e.button.y;
                A.click = true;
            } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (A.open_dd != DD_NONE)
                    A.dd_scroll -= e.wheel.y * 2;
                else
                    A.scroll -= e.wheel.y * 3;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) {
                    if (A.open_dd != DD_NONE) A.open_dd = DD_NONE;
                    else if (A.view == V_PLAYER) select_view(V_ROSTER);
                } else if (k == SDLK_DOWN) A.scroll += 1;
                else if (k == SDLK_UP) A.scroll -= 1;
                else if (k == SDLK_PAGEDOWN) A.scroll += A.vis_lines - 1;
                else if (k == SDLK_PAGEUP) A.scroll -= A.vis_lines - 1;
                else if (k == SDLK_HOME) A.scroll = 0;
                else if (k == SDLK_END) A.scroll = 1e9f;
                else if (k == SDLK_P) { A.post = !A.post; A.dirty = true; A.scroll = 0; }
                else if (k == SDLK_1) select_view(V_STANDINGS);
                else if (k == SDLK_2) select_view(V_TEAM);
                else if (k == SDLK_3) select_view(V_ROSTER);
                else if (k == SDLK_EQUALS || k == SDLK_PLUS) { if (A.ui < 3) A.ui += 0.5f; A.dirty = true; }
                else if (k == SDLK_MINUS) { if (A.ui > 1) A.ui -= 0.5f; A.dirty = true; }
            }
            got = SDL_PollEvent(&e);
        }
        if (shot && shot_frames >= 3 && (shot_frames - 3) % 3 == 0 && (shot_frames - 3) / 3 < nclicks) {
            int k = (shot_frames - 3) / 3;
            A.mx = click_x[k];
            A.my = click_y[k];
            A.click = true;
        }
        frame();
        if (shot && ++shot_frames == 6 + 3 * nclicks) {
            SDL_Surface *surf = SDL_RenderReadPixels(A.ren, NULL);
            if (surf) {
                SDL_SaveBMP(surf, shot);
                SDL_DestroySurface(surf);
            }
            break;
        }
        SDL_RenderPresent(A.ren);
    }
    if (A.job.running)
        _exit(0); /* don't wait for a long download on quit */
    SDL_Quit();
    return 0;
}
