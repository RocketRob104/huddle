#include "espn.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "util.h"

#define CORE_URL "https://sports.core.api.espn.com/v2/sports/football/leagues/nfl"
#define STANDINGS_URL "https://site.web.api.espn.com/apis/v2/sports/football/nfl/standings"

#define ROSTER_THREADS 12
#define TEAM_THREADS 8

int espn_current_season_year(void)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int y = tmv.tm_year + 1900;
    return tmv.tm_mon + 1 >= 7 ? y : y - 1; /* seasons start in September */
}

/* ------------------------------------------------------------------ */
/* small JSON helpers (all NULL-safe)                                  */
/* ------------------------------------------------------------------ */
static const char *jstr(yyjson_val *o, const char *k) { return yyjson_get_str(yyjson_obj_get(o, k)); }
static const char *jref(yyjson_val *o, const char *k) { return yyjson_get_str(yyjson_obj_get(yyjson_obj_get(o, k), "$ref")); }
static double jnum(yyjson_val *o, const char *k) { return yyjson_get_num(yyjson_obj_get(o, k)); }

/* ".../teams/12?lang=en" -> "12" */
static bool ref_id(const char *ref, char *out, size_t n)
{
    if (!ref)
        return false;
    const char *q = strchr(ref, '?');
    const char *e = q ? q : ref + strlen(ref);
    const char *b = e;
    while (b > ref && b[-1] != '/')
        b--;
    size_t l = (size_t)(e - b);
    if (!l || l >= n)
        return false;
    memcpy(out, b, l);
    out[l] = 0;
    return true;
}

/* Collect every "$ref" from an ESPN paged list. Caller frees with refs_free. */
static int list_refs(const char *base, char ***out)
{
    char **refs = NULL;
    int n = 0, cap = 0;
    int pages = 1;
    for (int page = 1; page <= pages && page <= 20; page++) {
        char url[600];
        snprintf(url, sizeof url, "%s%climit=200&page=%d", base, strchr(base, '?') ? '&' : '?', page);
        yyjson_doc *doc = http_get_json(url, NULL);
        if (!doc)
            break;
        yyjson_val *root = yyjson_doc_get_root(doc);
        pages = (int)jnum(root, "pageCount");
        if (pages < 1)
            pages = 1;
        size_t i, max;
        yyjson_val *it;
        yyjson_arr_foreach(yyjson_obj_get(root, "items"), i, max, it) {
            const char *r = jstr(it, "$ref");
            if (!r)
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 128;
                refs = realloc(refs, (size_t)cap * sizeof *refs);
            }
            refs[n++] = strdup(r);
        }
        yyjson_doc_free(doc);
    }
    *out = refs;
    return n;
}

static void refs_free(char **refs, int n)
{
    for (int i = 0; i < n; i++)
        free(refs[i]);
    free(refs);
}

/* ------------------------------------------------------------------ */
/* statistics                                                          */
/* ------------------------------------------------------------------ */
/* Walk "splits.categories[].stats[]" into a StatList. */
static void parse_splits(yyjson_val *splits, StatList *out, bool keep_zero)
{
    size_t ci, cmax;
    yyjson_val *cat;
    yyjson_arr_foreach(yyjson_obj_get(splits, "categories"), ci, cmax, cat) {
        const char *cname = intern(jstr(cat, "name"));
        size_t si, smax;
        yyjson_val *st;
        yyjson_arr_foreach(yyjson_obj_get(cat, "stats"), si, smax, st) {
            const char *disp = jstr(st, "displayValue");
            if (!disp || !*disp)
                continue;
            if (!keep_zero && jnum(st, "value") == 0)
                continue;
            const char *nm = jstr(st, "displayName");
            if (!nm)
                nm = jstr(st, "name");
            if (!nm)
                continue;
            Stat *s = statlist_push(out);
            s->cat = cname;
            s->name = intern(nm);
            copy_str(s->val, sizeof s->val, disp);
        }
    }
}

/* ------------------------------------------------------------------ */
/* structure: conferences, divisions, team names                       */
/* ------------------------------------------------------------------ */
static bool fetch_structure(Season *s, ProgressFn pf, void *ud)
{
    char url[300];
    if (pf)
        pf(ud, "Fetching conferences and divisions...");
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/2/groups", s->year);
    char **confs;
    int nc = list_refs(url, &confs);
    for (int c = 0; c < nc; c++) {
        yyjson_doc *cd = http_get_json(confs[c], NULL);
        if (!cd)
            continue;
        char conf[8];
        copy_str(conf, sizeof conf, jstr(yyjson_doc_get_root(cd), "abbreviation"));
        const char *children = jref(yyjson_doc_get_root(cd), "children");
        char **divs = NULL;
        int nd = children ? list_refs(children, &divs) : 0;
        for (int d = 0; d < nd; d++) {
            yyjson_doc *dd = http_get_json(divs[d], NULL);
            if (!dd)
                continue;
            yyjson_val *dr = yyjson_doc_get_root(dd);
            char dname[16];
            copy_str(dname, sizeof dname, jstr(dr, "name"));
            const char *teams_ref = jref(dr, "teams");
            char **trefs = NULL;
            int nt = teams_ref ? list_refs(teams_ref, &trefs) : 0;
            for (int t = 0; t < nt; t++) {
                char id[8];
                if (!ref_id(trefs[t], id, sizeof id))
                    continue;
                Team *team = season_add_team(s, id);
                copy_str(team->conf, sizeof team->conf, conf);
                copy_str(team->division, sizeof team->division, dname);
            }
            refs_free(trefs, nt);
            yyjson_doc_free(dd);
        }
        refs_free(divs, nd);
        yyjson_doc_free(cd);
    }
    refs_free(confs, nc);
    return s->nteams > 0;
}

static void team_detail_job(int i, void *ctx)
{
    Season *s = ctx;
    Team *t = &s->teams[i];
    char url[300];
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/teams/%s", s->year, t->id);
    yyjson_doc *doc = http_get_json(url, NULL);
    if (!doc)
        return;
    yyjson_val *r = yyjson_doc_get_root(doc);
    copy_str(t->name, sizeof t->name, jstr(r, "displayName"));
    copy_str(t->abbr, sizeof t->abbr, jstr(r, "abbreviation"));
    yyjson_doc_free(doc);
}

/* ------------------------------------------------------------------ */
/* standings                                                           */
/* ------------------------------------------------------------------ */
static bool fetch_standings(Season *s)
{
    char url[300];
    snprintf(url, sizeof url, STANDINGS_URL "?season=%d&seasontype=2", s->year);
    yyjson_doc *doc = http_get_json(url, NULL);
    if (!doc)
        return false;
    int seen = 0;
    size_t ci, cmax;
    yyjson_val *child;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "children"), ci, cmax, child) {
        const char *conf = jstr(child, "abbreviation");
        size_t ei, emax;
        yyjson_val *e;
        yyjson_arr_foreach(yyjson_obj_get(yyjson_obj_get(child, "standings"), "entries"), ei, emax, e) {
            yyjson_val *tm = yyjson_obj_get(e, "team");
            const char *id = jstr(tm, "id");
            if (!id)
                continue;
            Team *t = season_add_team(s, id);
            if (!t->name[0])
                copy_str(t->name, sizeof t->name, jstr(tm, "displayName"));
            if (!t->abbr[0])
                copy_str(t->abbr, sizeof t->abbr, jstr(tm, "abbreviation"));
            if (!t->conf[0])
                copy_str(t->conf, sizeof t->conf, conf);
            size_t si, smax;
            yyjson_val *st;
            yyjson_arr_foreach(yyjson_obj_get(e, "stats"), si, smax, st) {
                const char *nm = jstr(st, "name");
                const char *dv = jstr(st, "displayValue");
                double v = jnum(st, "value");
                if (!nm)
                    continue;
                if (!strcmp(nm, "wins")) t->wins = (int)v;
                else if (!strcmp(nm, "losses")) t->losses = (int)v;
                else if (!strcmp(nm, "ties")) t->ties = (int)v;
                else if (!strcmp(nm, "pointsFor")) t->pf = (int)v;
                else if (!strcmp(nm, "pointsAgainst")) t->pa = (int)v;
                else if (!strcmp(nm, "winPercent")) t->win_pct = v;
                else if (!strcmp(nm, "playoffSeed")) t->seed = (int)v;
                else if (!strcmp(nm, "streak")) copy_str(t->streak, sizeof t->streak, dv);
                else if (!strcmp(nm, "overall")) copy_str(t->overall, sizeof t->overall, dv);
                else if (!strcmp(nm, "Home")) copy_str(t->home, sizeof t->home, dv);
                else if (!strcmp(nm, "Road")) copy_str(t->road, sizeof t->road, dv);
                else if (!strcmp(nm, "vs. Div.")) copy_str(t->vs_div, sizeof t->vs_div, dv);
                else if (!strcmp(nm, "vs. Conf.")) copy_str(t->vs_conf, sizeof t->vs_conf, dv);
            }
            seen++;
        }
    }
    yyjson_doc_free(doc);
    if (seen)
        now_string(s->standings_at);
    return seen > 0;
}

/* ------------------------------------------------------------------ */
/* team statistics                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    Season *s;
    int *failed; /* per-index flag */
} TeamCtx;

static void team_stats_job(int i, void *ctx)
{
    TeamCtx *c = ctx;
    Team *t = &c->s->teams[i];
    char url[300];
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/2/teams/%s/statistics", c->s->year, t->id);
    yyjson_doc *doc = http_get_json(url, NULL);
    if (!doc) {
        c->failed[i] = 1;
        return;
    }
    StatList fresh = {0};
    parse_splits(yyjson_obj_get(yyjson_doc_get_root(doc), "splits"), &fresh, true);
    yyjson_doc_free(doc);
    statlist_free(&t->stats);
    t->stats = fresh;

    /* Postseason (season type 3). Teams that missed the playoffs return 404:
     * that just means "no postseason", not an error. */
    StatList post = {0};
    char rec[12] = "";
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/3/teams/%s/statistics", c->s->year, t->id);
    long st = 0;
    doc = http_get_json(url, &st);
    if (doc) {
        parse_splits(yyjson_obj_get(yyjson_doc_get_root(doc), "splits"), &post, true);
        yyjson_doc_free(doc);
        snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/3/teams/%s/record", c->s->year, t->id);
        doc = http_get_json(url, NULL);
        if (doc) {
            size_t ri, rmax;
            yyjson_val *item;
            yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "items"), ri, rmax, item) {
                if (jstr(item, "name") && !strcmp(jstr(item, "name"), "overall"))
                    copy_str(rec, sizeof rec, jstr(item, "summary"));
            }
            yyjson_doc_free(doc);
        }
    } else if (st != 404) {
        return; /* transient error: keep whatever postseason data we already had */
    }
    statlist_free(&t->post_stats);
    t->post_stats = post;
    copy_str(t->post_record, sizeof t->post_record, rec);
}

/* ------------------------------------------------------------------ */
/* rosters                                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    char id[16];
    char name[48];
} CollegeEnt;
static CollegeEnt *g_col;
static int g_ncol, g_capcol;
static pthread_mutex_t g_col_mu = PTHREAD_MUTEX_INITIALIZER;

static void college_name(const char *ref, char *out, size_t n)
{
    char id[16];
    out[0] = 0;
    if (!ref_id(ref, id, sizeof id))
        return;
    pthread_mutex_lock(&g_col_mu);
    for (int i = 0; i < g_ncol; i++)
        if (!strcmp(g_col[i].id, id)) {
            copy_str(out, n, g_col[i].name);
            pthread_mutex_unlock(&g_col_mu);
            return;
        }
    pthread_mutex_unlock(&g_col_mu);

    yyjson_doc *doc = http_get_json(ref, NULL);
    if (!doc)
        return;
    yyjson_val *r = yyjson_doc_get_root(doc);
    const char *nm = jstr(r, "name");
    if (!nm)
        nm = jstr(r, "shortDisplayName");
    if (!nm)
        nm = jstr(r, "displayName");
    if (nm) {
        copy_str(out, n, nm);
        pthread_mutex_lock(&g_col_mu);
        if (g_ncol == g_capcol) {
            g_capcol = g_capcol ? g_capcol * 2 : 256;
            g_col = realloc(g_col, (size_t)g_capcol * sizeof *g_col);
        }
        copy_str(g_col[g_ncol].id, sizeof g_col[g_ncol].id, id);
        copy_str(g_col[g_ncol].name, sizeof g_col[g_ncol].name, nm);
        g_ncol++;
        pthread_mutex_unlock(&g_col_mu);
    }
    yyjson_doc_free(doc);
}

/* Fill a Player from a season athlete record (seasons/Y/athletes/ID).
 * ESPN reports age and experience as of *today*, so both are shifted back to the
 * requested season. */
static void parse_athlete(yyjson_val *a, Player *p, int season)
{
    const char *name = jstr(a, "fullName");
    if (!name) name = jstr(a, "displayName");
    if (!name) name = jstr(a, "shortName");
    copy_str(p->id, sizeof p->id, jstr(a, "id"));
    copy_str(p->name, sizeof p->name, name ? name : "Unknown Player");
    copy_str(p->pos, sizeof p->pos, jstr(yyjson_obj_get(a, "position"), "abbreviation"));
    copy_str(p->jersey, sizeof p->jersey, jstr(a, "jersey"));
    copy_str(p->height, sizeof p->height, jstr(a, "displayHeight"));
    copy_str(p->weight, sizeof p->weight, jstr(a, "displayWeight"));
    copy_str(p->status, sizeof p->status, jstr(yyjson_obj_get(a, "status"), "name"));

    /* age on September 1 of the season */
    int by, bm, bd;
    const char *dob = jstr(a, "dateOfBirth");
    if (dob && sscanf(dob, "%d-%d-%d", &by, &bm, &bd) == 3) {
        int age = season - by;
        if (bm > 9 || (bm == 9 && bd > 1))
            age--;
        p->age = age;
    } else {
        p->age = (int)jnum(a, "age");
    }

    yyjson_val *exp = yyjson_obj_get(a, "experience");
    if (exp) {
        int yrs = (int)jnum(exp, "years") - (espn_current_season_year() - season);
        if (yrs <= 0)
            copy_str(p->exp, sizeof p->exp, "Rookie");
        else
            snprintf(p->exp, sizeof p->exp, "%d %s", yrs, yrs == 1 ? "yr" : "yrs");
    }
    const char *cref = jref(a, "college");
    if (cref)
        college_name(cref, p->college, sizeof p->college);
}

typedef struct {
    int year;
    char **refs;    /* season athlete record URLs */
    Player *out;
    char *ok;
    bool want_post; /* the team played in the postseason, so ask for playoff stats */
} RosterCtx;

static void player_job(int i, void *vctx)
{
    RosterCtx *c = vctx;
    yyjson_doc *doc = http_get_json(c->refs[i], NULL);
    if (!doc)
        return;
    Player *p = &c->out[i];
    memset(p, 0, sizeof *p);
    parse_athlete(yyjson_doc_get_root(doc), p, c->year);
    yyjson_doc_free(doc);
    if (!p->id[0])
        return;
    c->ok[i] = 1;

    /* Season stats. A 404 just means this player has none (e.g. never played). */
    char url[300];
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/2/athletes/%s/statistics/0", c->year, p->id);
    yyjson_doc *sd = http_get_json(url, NULL);
    if (sd) {
        parse_splits(yyjson_obj_get(yyjson_doc_get_root(sd), "splits"), &p->stats, false);
        yyjson_doc_free(sd);
    }
    if (c->want_post) {
        snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/3/athletes/%s/statistics/0", c->year, p->id);
        sd = http_get_json(url, NULL);
        if (sd) {
            parse_splits(yyjson_obj_get(yyjson_doc_get_root(sd), "splits"), &p->post_stats, false);
            yyjson_doc_free(sd);
        }
    }
}

static int pos_rank(const char *pos)
{
    static const char *order[] = {"QB", "RB", "FB", "WR", "TE", "T", "OT", "LT", "G", "OG", "LG", "C", "RG", "RT", "OL",
                                  "DE", "DT", "NT", "DL", "OLB", "ILB", "MLB", "LB", "CB", "S", "FS", "SS", "DB",
                                  "PK", "K", "P", "LS", "KR", "PR", "SPEC"};
    for (size_t i = 0; i < sizeof order / sizeof *order; i++)
        if (!strcmp(pos, order[i]))
            return (int)i;
    return 99;
}

static int player_cmp(const void *a, const void *b)
{
    const Player *x = a, *y = b;
    int d = pos_rank(x->pos) - pos_rank(y->pos);
    if (d)
        return d;
    int jx = atoi(x->jersey), jy = atoi(y->jersey);
    if (jx != jy)
        return jx - jy;
    return strcmp(x->name, y->name);
}

/* Install fetched players (taking ownership of out[], compacting failures). */
static bool install_roster(Team *t, Player *out, const char *ok, int n, const int *gp, const int *gs, const char (*jersey)[6])
{
    Player *kept = malloc((size_t)n * sizeof *kept);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!ok[i]) {
            statlist_free(&out[i].stats);
            statlist_free(&out[i].post_stats);
            continue;
        }
        if (gp) {
            out[i].gp = gp[i];
            out[i].gs = gs[i];
            if (jersey[i][0])
                copy_str(out[i].jersey, sizeof out[i].jersey, jersey[i]);
            out[i].status[0] = 0; /* ESPN's status is "today's", meaningless for a past season */
        }
        kept[k++] = out[i];
    }
    free(out);
    if (k == 0) {
        free(kept);
        return false;
    }
    qsort(kept, (size_t)k, sizeof *kept, player_cmp);
    team_clear_roster(t);
    t->players = kept;
    t->nplayers = k;
    t->roster_loaded = true;
    now_string(t->roster_at);
    return true;
}

/* Current season: ESPN's live roster endpoint (includes practice squad / IR). */
static bool fetch_roster_current(Season *s, Team *t)
{
    char url[300];
    snprintf(url, sizeof url, CORE_URL "/seasons/%d/teams/%s/athletes", s->year, t->id);
    char **refs;
    int n = list_refs(url, &refs);
    if (n == 0)
        return false;
    RosterCtx c = {.year = s->year, .refs = refs};
    char purl[300], **pe = NULL;
    snprintf(purl, sizeof purl, CORE_URL "/seasons/%d/types/3/teams/%s/events", s->year, t->id);
    int npost = list_refs(purl, &pe);
    refs_free(pe, npost);
    c.want_post = npost > 0;
    c.out = calloc((size_t)n, sizeof *c.out);
    c.ok = calloc((size_t)n, 1);
    parallel_for(n, ROSTER_THREADS, player_job, &c);
    refs_free(refs, n);
    bool r = install_roster(t, c.out, c.ok, n, NULL, NULL, NULL);
    free(c.ok);
    return r;
}

/* ---- past seasons -------------------------------------------------
 * ESPN's roster endpoints ignore the season parameter and always return TODAY's
 * roster, so a past season's roster is rebuilt from its game-day rosters: every
 * player who was on this team's game-day roster in any regular-season or
 * playoff game. (Players who spent the whole year on the
 * practice squad never appear. Injured-reserve players usually do.) */
typedef struct {
    long pid;
    char jersey[6];
    bool starter, dnp;
    bool valid; /* false = on the team's list but not dressed (IR, inactive) */
} GameEntry;

typedef struct {
    GameEntry *e;
    int n;
} GameRoster;

typedef struct {
    const Season *s;
    const Team *t;
    char **ev_ids;
    GameRoster *out;
} GameCtx;

static void game_job(int i, void *vctx)
{
    GameCtx *c = vctx;
    char url[400];
    snprintf(url, sizeof url, CORE_URL "/events/%s/competitions/%s/competitors/%s/roster", c->ev_ids[i], c->ev_ids[i], c->t->id);
    yyjson_doc *doc = http_get_json(url, NULL);
    if (!doc)
        return;
    yyjson_val *entries = yyjson_obj_get(yyjson_doc_get_root(doc), "entries");
    size_t n = yyjson_arr_size(entries);
    GameEntry *ge = calloc(n ? n : 1, sizeof *ge);
    int k = 0;
    size_t ii, imax;
    yyjson_val *en;
    yyjson_arr_foreach(entries, ii, imax, en) {
        long pid = (long)yyjson_get_sint(yyjson_obj_get(en, "playerId"));
        if (pid <= 0)
            continue;
        ge[k].pid = pid;
        copy_str(ge[k].jersey, sizeof ge[k].jersey, jstr(en, "jersey"));
        ge[k].starter = yyjson_get_bool(yyjson_obj_get(en, "starter"));
        ge[k].dnp = yyjson_get_bool(yyjson_obj_get(en, "didNotPlay"));
        ge[k].valid = yyjson_get_bool(yyjson_obj_get(en, "valid"));
        k++;
    }
    c->out[i].e = ge;
    c->out[i].n = k;
    yyjson_doc_free(doc);
}

typedef struct {
    long pid;
    char jersey[6];
    int gp, gs;
} Agg;

static bool fetch_roster_from_games(Season *s, Team *t)
{
    /* The team's games: regular season (type 2) then playoffs (type 3). */
    char **ev_refs[2] = {NULL, NULL};
    int nev[2] = {0, 0};
    for (int ty = 0; ty < 2; ty++) {
        char url[300];
        snprintf(url, sizeof url, CORE_URL "/seasons/%d/types/%d/teams/%s/events", s->year, ty + 2, t->id);
        nev[ty] = list_refs(url, &ev_refs[ty]);
    }
    int total = nev[0] + nev[1];
    if (total == 0) {
        refs_free(ev_refs[0], nev[0]);
        refs_free(ev_refs[1], nev[1]);
        return false;
    }
    char **ids = calloc((size_t)total, sizeof *ids);
    for (int i = 0; i < total; i++) {
        const char *ref = i < nev[0] ? ev_refs[0][i] : ev_refs[1][i - nev[0]];
        char id[24];
        if (ref_id(ref, id, sizeof id))
            ids[i] = strdup(id);
    }
    refs_free(ev_refs[0], nev[0]);
    refs_free(ev_refs[1], nev[1]);

    GameRoster *gr = calloc((size_t)total, sizeof *gr);
    GameCtx gc = {.s = s, .t = t, .ev_ids = ids, .out = gr};
    parallel_for(total, TEAM_THREADS, game_job, &gc);

    Agg *agg = NULL;
    int nagg = 0, cap = 0, games_ok = 0;
    for (int g = 0; g < total; g++) {
        if (!gr[g].e)
            continue;
        games_ok++;
        bool regular = g < nev[0];
        for (int k = 0; k < gr[g].n; k++) {
            const GameEntry *e = &gr[g].e[k];
            int j = 0;
            while (j < nagg && agg[j].pid != e->pid)
                j++;
            if (j == nagg) {
                if (nagg == cap) {
                    cap = cap ? cap * 2 : 128;
                    agg = realloc(agg, (size_t)cap * sizeof *agg);
                }
                memset(&agg[nagg], 0, sizeof agg[nagg]);
                agg[nagg++].pid = e->pid;
            }
            if (e->jersey[0])
                copy_str(agg[j].jersey, sizeof agg[j].jersey, e->jersey);
            if (regular && e->valid && !e->dnp)
                agg[j].gp++;
            if (regular && e->valid && !e->dnp && e->starter)
                agg[j].gs++;
        }
        free(gr[g].e);
    }
    free(gr);
    for (int i = 0; i < total; i++)
        free(ids[i]);
    free(ids);
    if (nagg == 0 || games_ok < total / 2) { /* mostly failed: don't install a partial roster */
        free(agg);
        return false;
    }

    char **refs = calloc((size_t)nagg, sizeof *refs);
    int *gp = malloc((size_t)nagg * sizeof *gp), *gs = malloc((size_t)nagg * sizeof *gs);
    char (*jersey)[6] = malloc((size_t)nagg * sizeof *jersey);
    for (int i = 0; i < nagg; i++) {
        char url[300];
        snprintf(url, sizeof url, CORE_URL "/seasons/%d/athletes/%ld", s->year, agg[i].pid);
        refs[i] = strdup(url);
        gp[i] = agg[i].gp;
        gs[i] = agg[i].gs;
        copy_str(jersey[i], 6, agg[i].jersey);
    }
    free(agg);

    RosterCtx c = {.year = s->year, .refs = refs, .want_post = nev[1] > 0};
    c.out = calloc((size_t)nagg, sizeof *c.out);
    c.ok = calloc((size_t)nagg, 1);
    parallel_for(nagg, ROSTER_THREADS, player_job, &c);
    refs_free(refs, nagg);
    bool r = install_roster(t, c.out, c.ok, nagg, gp, gs, (const char (*)[6])jersey);
    free(c.ok);
    free(gp);
    free(gs);
    free(jersey);
    return r;
}

static bool fetch_roster(Season *s, Team *t)
{
    return s->year < espn_current_season_year() ? fetch_roster_from_games(s, t) : fetch_roster_current(s, t);
}

/* ------------------------------------------------------------------ */
/* orchestration                                                       */
/* ------------------------------------------------------------------ */
bool espn_update(Season *s, const UpdateOpts *o, ProgressFn pf, void *ud, char *summary, size_t sn)
{
    char msg[160];
    int warnings = 0;

    if (!s->has_structure || s->nteams == 0) {
        if (!fetch_structure(s, pf, ud)) {
            snprintf(summary, sn, "Could not reach ESPN for %d (are you online?)", s->year);
            return false;
        }
        if (pf)
            pf(ud, "Fetching team names...");
        parallel_for(s->nteams, TEAM_THREADS, team_detail_job, s);
        s->has_structure = true;
    }

    if (pf)
        pf(ud, "Fetching standings...");
    if (!fetch_standings(s)) {
        snprintf(summary, sn, "Standings request failed for %d (are you online?)", s->year);
        return false;
    }

    if (pf)
        pf(ud, "Fetching team statistics...");
    TeamCtx tc = {.s = s, .failed = calloc((size_t)s->nteams, sizeof(int))};
    parallel_for(s->nteams, TEAM_THREADS, team_stats_job, &tc);
    for (int i = 0; i < s->nteams; i++)
        warnings += tc.failed[i];
    free(tc.failed);

    int rosters = 0;
    if (o && o->all_rosters) {
        for (int i = 0; i < s->nteams; i++) {
            snprintf(msg, sizeof msg, "Roster %d/%d: %s (players + stats)...", i + 1, s->nteams, s->teams[i].name);
            if (pf)
                pf(ud, msg);
            if (fetch_roster(s, &s->teams[i]))
                rosters++;
            else
                warnings++;
        }
    } else if (o && o->team_id && *o->team_id) {
        Team *t = season_find_team(s, o->team_id);
        if (t) {
            snprintf(msg, sizeof msg, "Fetching roster: %s (players + stats)...", t->name);
            if (pf)
                pf(ud, msg);
            if (fetch_roster(s, t))
                rosters++;
            else
                warnings++;
        }
    }

    now_string(s->fetched_at);
    snprintf(summary, sn, "Updated %d: standings, team stats, %d roster%s%s%s", s->year, rosters, rosters == 1 ? "" : "s",
             warnings ? " - some requests failed: " : "", warnings ? "press Update to retry" : "");
    return true;
}
