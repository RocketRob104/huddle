#include "views.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "palette.h"
#include "util.h"

/* While building a postseason view every heading bar is amber instead of purple,
 * so playoff numbers are never mistaken for regular-season ones. */
static bool g_post;

/* ------------------------------------------------------------------ */
/* Lines / Cells                                                       */
/* ------------------------------------------------------------------ */
void lines_clear(Lines *L) { L->n = 0; }

void lines_free(Lines *L)
{
    free(L->v);
    L->v = NULL;
    L->n = L->cap = 0;
}

static Line *line_add(Lines *L)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->v = realloc(L->v, (size_t)L->cap * sizeof *L->v);
    }
    Line *l = &L->v[L->n++];
    memset(l, 0, sizeof *l);
    for (int i = 0; i < 3; i++) {
        l->c[i].team = -1;
        l->c[i].player = -1;
        l->c[i].fg = SNES_BLACK;
    }
    return l;
}

static Cell *cell_set(Line *l, int x, int w, const char *text)
{
    Cell *c = &l->c[l->n++];
    c->x = x;
    c->w = w;
    fold_ascii(c->text, sizeof c->text, text);
    return c;
}

static void blank(Lines *L) { line_add(L); }

static void heading(Lines *L, int chars, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Cell *c = cell_set(line_add(L), 0, chars, buf);
    c->heading = true;
    c->accent = g_post;
    c->fg = g_post ? SNES_BLACK : SNES_WHITE;
}

static void text_line(Lines *L, const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cell_set(line_add(L), 0, 200, buf);
}

/* "Name ........ value" within w characters. */
static void kv(char *out, size_t n, int w, const char *k, const char *v)
{
    char kk[96], vv[40];
    fold_ascii(kk, sizeof kk, k);
    fold_ascii(vv, sizeof vv, v);
    int vl = (int)strlen(vv);
    int kl = w - vl - 1;
    if (kl < 4)
        kl = 4;
    snprintf(out, n, "%-*.*s %s", kl, kl, kk, vv);
}

/* Emit the stats of one StatList as headed groups flowed over `cols` columns. */
static void stats_block(Lines *L, const StatList *sl, int chars)
{
    int cols = chars >= 120 ? 3 : chars >= 80 ? 2 : 1;
    int cw = (chars - (cols - 1) * 2) / cols;
    int i = 0;
    while (i < sl->n) {
        const char *cat = sl->v[i].cat;
        char title[64];
        snprintf(title, sizeof title, "%c%s", cat[0] >= 'a' && cat[0] <= 'z' ? cat[0] - 32 : cat[0], cat + 1);
        heading(L, chars, "%s", title);
        int j = i;
        while (j < sl->n && sl->v[j].cat == cat)
            j++;
        for (int k = i; k < j; k += cols) {
            Line *l = line_add(L);
            for (int c = 0; c < cols && k + c < j; c++) {
                char buf[176];
                kv(buf, sizeof buf, cw, sl->v[k + c].name, sl->v[k + c].val);
                cell_set(l, c * (cw + 2), cw, buf);
            }
        }
        blank(L);
        i = j;
    }
}

static void record_str(const Team *t, char *out, size_t n)
{
    if (t->ties)
        snprintf(out, n, "%d-%d-%d", t->wins, t->losses, t->ties);
    else
        snprintf(out, n, "%d-%d", t->wins, t->losses);
}

/* ------------------------------------------------------------------ */
/* Standings                                                           */
/* ------------------------------------------------------------------ */
static int team_cmp(const Team *a, const Team *b, bool use_seed)
{
    if (use_seed) {
        int sa = a->seed > 0 ? a->seed : 999, sb = b->seed > 0 ? b->seed : 999;
        if (sa != sb)
            return sa - sb;
    }
    if (a->win_pct != b->win_pct)
        return a->win_pct > b->win_pct ? -1 : 1;
    if (a->wins != b->wins)
        return b->wins - a->wins;
    int da = a->pf - a->pa, db = b->pf - b->pa;
    if (da != db)
        return db - da;
    return strcmp(a->name, b->name);
}

/* Indices of teams matching `pred`, sorted. */
static int pick_sorted(const Season *s, int *idx, const char *conf, const char *div, bool use_seed)
{
    int n = 0;
    for (int i = 0; i < s->nteams; i++) {
        const Team *t = &s->teams[i];
        if (conf && strcmp(t->conf, conf))
            continue;
        if (div && strcmp(t->division, div))
            continue;
        int j = n++;
        while (j > 0 && team_cmp(t, &s->teams[idx[j - 1]], use_seed) < 0) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = i;
    }
    return n;
}

typedef struct {
    Cell *v;
    int n, cap;
} Cells;

static Cell *cells_add(Cells *C)
{
    if (C->n == C->cap) {
        C->cap = C->cap ? C->cap * 2 : 64;
        C->v = realloc(C->v, (size_t)C->cap * sizeof *C->v);
    }
    Cell *c = &C->v[C->n++];
    memset(c, 0, sizeof *c);
    c->team = c->player = -1;
    c->fg = SNES_BLACK;
    return c;
}

#define STAND_W 45

static void cells_heading(Cells *C, const char *text)
{
    Cell *c = cells_add(C);
    fold_ascii(c->text, sizeof c->text, text);
    c->heading = true;
    c->accent = g_post;
    c->fg = g_post ? SNES_BLACK : SNES_WHITE;
    c->w = STAND_W;
}

static void cells_blank(Cells *C) { cells_add(C)->w = STAND_W; }

static void cells_team_rows(Cells *C, const Season *s, const int *idx, int n)
{
    for (int r = 0; r < n; r++) {
        const Team *t = &s->teams[idx[r]];
        char rec[16], nm[48], line[176];
        record_str(t, rec, sizeof rec);
        fold_ascii(nm, sizeof nm, t->name);
        snprintf(line, sizeof line, "%2d. %-22.22s %-7s %+5d %-4s", r + 1, nm, rec, t->pf - t->pa, t->streak);
        Cell *c = cells_add(C);
        copy_str(c->text, sizeof c->text, line);
        c->w = STAND_W;
        c->team = idx[r];
    }
}

static void build_post_standings(Lines *L, const Season *s, int chars)
{
    int idx[40], n = 0;
    for (int i = 0; i < s->nteams; i++) {
        if (!s->teams[i].post_record[0])
            continue;
        int w = 0, l = 0, w2 = 0, l2 = 0;
        sscanf(s->teams[i].post_record, "%d-%d", &w, &l);
        int j = n++;
        while (j > 0) {
            sscanf(s->teams[idx[j - 1]].post_record, "%d-%d", &w2, &l2);
            if (w > w2 || (w == w2 && l < l2) || (w == w2 && l == l2 && strcmp(s->teams[i].name, s->teams[idx[j - 1]].name) < 0)) {
                idx[j] = idx[j - 1];
                j--;
            } else
                break;
        }
        idx[j] = i;
    }
    heading(L, chars, "%d NFL POSTSEASON  -  playoff teams only (separate from the regular season)", s->year);
    if (n == 0) {
        blank(L);
        text_line(L, "No %d postseason results yet. The playoffs have not been played or have not been downloaded.", s->year);
        return;
    }
    text_line(L, "Teams ranked by playoff wins. Click a team to open it.");
    blank(L);
    char hdr[160];
    snprintf(hdr, sizeof hdr, "%3s  %-22s %-5s %-12s %s", "", "Team", "Conf", "Playoffs W-L", "Regular season");
    Cell *hc = cell_set(line_add(L), 0, chars, hdr);
    hc->fg = SNES_PURPLE;
    for (int r = 0; r < n; r++) {
        const Team *t = &s->teams[idx[r]];
        char rec[16], nm[48], row[160];
        record_str(t, rec, sizeof rec);
        fold_ascii(nm, sizeof nm, t->name);
        snprintf(row, sizeof row, "%2d.  %-22.22s %-5s %-12s %s", r + 1, nm, t->conf, t->post_record, rec);
        Cell *c = cell_set(line_add(L), 0, chars, row);
        c->team = idx[r];
    }
}

void build_standings(Lines *L, const Season *s, int chars, bool post)
{
    lines_clear(L);
    g_post = post;
    if (post && s->nteams) {
        build_post_standings(L, s, chars);
        return;
    }
    if (s->nteams == 0) {
        text_line(L, "No %d data yet. Press Update to download it from ESPN.", s->year);
        return;
    }
    char upd[64];
    snprintf(upd, sizeof upd, "%s", s->standings_at[0] ? s->standings_at : "never");
    heading(L, chars, "%d NFL Standings   (as of %s)", s->year, upd);
    text_line(L, "Columns: rank, team, record, point differential, streak.  Click a team to open it.");
    blank(L);

    int idx[40];
    Cells left = {0}, right = {0};

    static const char *confs[] = {"AFC", "NFC"};
    for (int c = 0; c < 2; c++) {
        bool seeds = false;
        for (int i = 0; i < s->nteams; i++)
            if (!strcmp(s->teams[i].conf, confs[c]) && s->teams[i].seed > 0)
                seeds = true;
        int n = pick_sorted(s, idx, confs[c], NULL, seeds);
        char h[40];
        snprintf(h, sizeof h, "%s Standings", confs[c]);
        cells_heading(&left, h);
        cells_team_rows(&left, s, idx, n);
        cells_blank(&left);
    }
    static const char *divs[] = {"AFC East", "AFC North", "AFC South", "AFC West", "NFC East", "NFC North", "NFC South", "NFC West"};
    for (int d = 0; d < 8; d++) {
        int n = pick_sorted(s, idx, NULL, divs[d], false);
        if (!n)
            continue;
        cells_heading(&right, divs[d]);
        cells_team_rows(&right, s, idx, n);
        cells_blank(&right);
    }

    if (chars >= 2 * STAND_W + 2) {
        int rows = left.n > right.n ? left.n : right.n;
        for (int i = 0; i < rows; i++) {
            Line *l = line_add(L);
            if (i < left.n) {
                l->c[l->n] = left.v[i];
                l->c[l->n++].x = 0;
            }
            if (i < right.n) {
                l->c[l->n] = right.v[i];
                l->c[l->n++].x = STAND_W + 2;
            }
        }
    } else {
        for (int i = 0; i < left.n; i++) {
            Line *l = line_add(L);
            l->c[l->n++] = left.v[i];
        }
        for (int i = 0; i < right.n; i++) {
            Line *l = line_add(L);
            l->c[l->n++] = right.v[i];
        }
    }
    free(left.v);
    free(right.v);
}

/* ------------------------------------------------------------------ */
/* Team                                                                */
/* ------------------------------------------------------------------ */
void build_team(Lines *L, const Season *s, const Team *t, int chars, bool post)
{
    lines_clear(L);
    g_post = post;
    char rec[16], buf[200];
    record_str(t, rec, sizeof rec);
    if (post) {
        heading(L, chars, "%s (%s)  -  %d POSTSEASON", t->name, t->abbr, s->year);
        text_line(L, "Playoff numbers only - kept separate from the regular season.  Regular season record was %s.", rec);
        blank(L);
        if (!t->post_record[0] && t->post_stats.n == 0) {
            text_line(L, "%s did not play in the %d postseason (or it has not been played / downloaded yet).", t->name, s->year);
            return;
        }
        text_line(L, "Postseason record: %s", t->post_record[0] ? t->post_record : "-");
        blank(L);
        stats_block(L, &t->post_stats, chars);
        return;
    }
    heading(L, chars, "%s (%s)  -  %d season", t->name, t->abbr, s->year);
    text_line(L, "Division: %s    Conference: %s    Conference rank: %s", t->division[0] ? t->division : "-", t->conf[0] ? t->conf : "-",
              t->seed > 0 ? (snprintf(buf, sizeof buf, "%d", t->seed), buf) : "-");
    blank(L);
    text_line(L, "Record: %-8s  Win %%: %-6.3f  Streak: %s", rec, t->win_pct, t->streak[0] ? t->streak : "-");
    text_line(L, "Points For: %d   Points Against: %d   Differential: %+d", t->pf, t->pa, t->pf - t->pa);
    text_line(L, "Home: %-6s  Road: %-6s  vs Div: %-6s  vs Conf: %s", t->home[0] ? t->home : "-", t->road[0] ? t->road : "-",
              t->vs_div[0] ? t->vs_div : "-", t->vs_conf[0] ? t->vs_conf : "-");
    blank(L);
    if (t->stats.n == 0) {
        text_line(L, "Team statistics for %d have not been downloaded yet. Press Update.", s->year);
        return;
    }
    stats_block(L, &t->stats, chars);
}

/* ------------------------------------------------------------------ */
/* Roster                                                              */
/* ------------------------------------------------------------------ */
void build_roster(Lines *L, const Season *s, const Team *t, int team_idx, int chars)
{
    lines_clear(L);
    g_post = false;
    heading(L, chars, "%s roster  -  %d season", t->name, s->year);
    if (!t->roster_loaded || t->nplayers == 0) {
        blank(L);
        text_line(L, "The %d roster for this team has not been downloaded yet.", s->year);
        text_line(L, "Press Update to fetch it (Update All fetches every team).");
        return;
    }
    text_line(L, "%d players. Click a player for their %d season statistics.   Roster fetched %s", t->nplayers, s->year, t->roster_at);
    blank(L);
    bool any_status = false; /* past seasons carry no status: ESPN only knows "today's" */
    for (int i = 0; i < t->nplayers; i++)
        any_status |= t->players[i].status[0] != 0;
    char hdr[176];
    snprintf(hdr, sizeof hdr, "%-5s %3s  %-24s %-4s %-7s %-9s %-7s %-23s %3s %3s  %s", "Pos", "#", "Player", "Age", "Height", "Weight",
             "Exp", "College", "GP", "GS", any_status ? "Status" : "");
    Line *h = line_add(L);
    Cell *hc = cell_set(h, 0, chars, hdr);
    hc->heading = true;
    hc->accent = g_post;
    hc->fg = g_post ? SNES_BLACK : SNES_WHITE;
    for (int i = 0; i < t->nplayers; i++) {
        const Player *p = &t->players[i];
        char row[176], nm[64], col[64];
        fold_ascii(nm, sizeof nm, p->name);
        fold_ascii(col, sizeof col, p->college);
        char age[8];
        if (p->age > 0)
            snprintf(age, sizeof age, "%d", p->age);
        else
            snprintf(age, sizeof age, "-");
        char gp[8] = "-", gs[8] = "-";
        if (p->gp || p->gs) {
            snprintf(gp, sizeof gp, "%d", p->gp);
            snprintf(gs, sizeof gs, "%d", p->gs);
        }
        snprintf(row, sizeof row, "%-5.5s %3.3s  %-24.24s %-4s %-7.7s %-9.9s %-7.7s %-23.23s %3s %3s  %-.14s", p->pos[0] ? p->pos : "-",
                 p->jersey[0] ? p->jersey : "-", nm, age, p->height[0] ? p->height : "-", p->weight[0] ? p->weight : "-",
                 p->exp[0] ? p->exp : "-", col[0] ? col : "-", gp, gs, p->status);
        Line *l = line_add(L);
        Cell *c = cell_set(l, 0, chars, row);
        c->team = team_idx;
        c->player = i;
    }
}

/* ------------------------------------------------------------------ */
/* Player                                                              */
/* ------------------------------------------------------------------ */
void build_player(Lines *L, const Season *s, const Team *t, const Player *p, int chars, bool post)
{
    lines_clear(L);
    g_post = post;
    heading(L, chars, "%s  -  %s #%s  -  %s %d%s", p->name, p->pos[0] ? p->pos : "?", p->jersey[0] ? p->jersey : "-", t->name, s->year,
            post ? " POSTSEASON" : "");
    text_line(L, "Age: %d    Height: %s    Weight: %s    Experience: %s", p->age, p->height[0] ? p->height : "-",
              p->weight[0] ? p->weight : "-", p->exp[0] ? p->exp : "-");
    text_line(L, "College: %s    Games played: %d    Games started: %d%s%s", p->college[0] ? p->college : "-", p->gp, p->gs,
              p->status[0] ? "    Status: " : "", p->status);
    blank(L);
    if (post) {
        text_line(L, "Playoff numbers only - kept separate from the regular season.");
        blank(L);
        if (p->post_stats.n == 0) {
            text_line(L, "No %d postseason statistics for this player (team missed the playoffs, or he did not record any).", s->year);
            return;
        }
        stats_block(L, &p->post_stats, chars);
        return;
    }
    if (p->stats.n == 0) {
        text_line(L, "No %d regular-season statistics recorded for this player.", s->year);
        return;
    }
    stats_block(L, &p->stats, chars);
}
