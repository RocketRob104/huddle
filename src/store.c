#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#include "util.h"

/* ---------------------------- writing ---------------------------- */

/* {"passing":[["Passing Yards","3,394"],...],"rushing":[...]} - stats are stored
 * contiguously by category, so consecutive entries share one key. */
static yyjson_mut_val *stats_json(yyjson_mut_doc *d, const StatList *sl)
{
    yyjson_mut_val *obj = yyjson_mut_obj(d);
    const char *cur = NULL;
    yyjson_mut_val *arr = NULL;
    for (int i = 0; i < sl->n; i++) {
        const Stat *st = &sl->v[i];
        if (!cur || strcmp(cur, st->cat)) {
            arr = yyjson_mut_arr(d);
            yyjson_mut_obj_add_val(d, obj, st->cat, arr); /* cat is interned: lives forever */
            cur = st->cat;
        }
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_strcpy(d, pair, st->name);
        yyjson_mut_arr_add_strcpy(d, pair, st->val);
        yyjson_mut_arr_append(arr, pair);
    }
    return obj;
}

#define ADD_STR(o, key, val) yyjson_mut_obj_add_strcpy(d, (o), (key), (val))

bool store_save(const Season *s, const char *path)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_int(d, root, "version", 1);
    yyjson_mut_obj_add_int(d, root, "year", s->year);
    ADD_STR(root, "fetched_at", s->fetched_at);
    ADD_STR(root, "standings_at", s->standings_at);
    yyjson_mut_obj_add_bool(d, root, "structure", s->has_structure);

    yyjson_mut_val *teams = yyjson_mut_arr(d);
    yyjson_mut_obj_add_val(d, root, "teams", teams);
    for (int i = 0; i < s->nteams; i++) {
        const Team *t = &s->teams[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_arr_append(teams, o);
        ADD_STR(o, "id", t->id);
        ADD_STR(o, "name", t->name);
        ADD_STR(o, "abbr", t->abbr);
        ADD_STR(o, "conf", t->conf);
        ADD_STR(o, "div", t->division);
        yyjson_mut_obj_add_int(d, o, "w", t->wins);
        yyjson_mut_obj_add_int(d, o, "l", t->losses);
        yyjson_mut_obj_add_int(d, o, "t", t->ties);
        yyjson_mut_obj_add_int(d, o, "pf", t->pf);
        yyjson_mut_obj_add_int(d, o, "pa", t->pa);
        yyjson_mut_obj_add_real(d, o, "pct", t->win_pct);
        ADD_STR(o, "streak", t->streak);
        yyjson_mut_obj_add_int(d, o, "seed", t->seed);
        ADD_STR(o, "overall", t->overall);
        ADD_STR(o, "home", t->home);
        ADD_STR(o, "road", t->road);
        ADD_STR(o, "vsdiv", t->vs_div);
        ADD_STR(o, "vsconf", t->vs_conf);
        yyjson_mut_obj_add_bool(d, o, "roster", t->roster_loaded);
        ADD_STR(o, "roster_at", t->roster_at);
        yyjson_mut_obj_add_val(d, o, "stats", stats_json(d, &t->stats));
        ADD_STR(o, "prec", t->post_record);
        yyjson_mut_obj_add_val(d, o, "pstats", stats_json(d, &t->post_stats));

        yyjson_mut_val *players = yyjson_mut_arr(d);
        yyjson_mut_obj_add_val(d, o, "players", players);
        for (int p = 0; p < t->nplayers; p++) {
            const Player *pl = &t->players[p];
            yyjson_mut_val *po = yyjson_mut_obj(d);
            yyjson_mut_arr_append(players, po);
            ADD_STR(po, "id", pl->id);
            ADD_STR(po, "name", pl->name);
            ADD_STR(po, "pos", pl->pos);
            ADD_STR(po, "no", pl->jersey);
            yyjson_mut_obj_add_int(d, po, "age", pl->age);
            ADD_STR(po, "ht", pl->height);
            ADD_STR(po, "wt", pl->weight);
            ADD_STR(po, "exp", pl->exp);
            ADD_STR(po, "college", pl->college);
            ADD_STR(po, "status", pl->status);
            yyjson_mut_obj_add_int(d, po, "gp", pl->gp);
            yyjson_mut_obj_add_int(d, po, "gs", pl->gs);
            yyjson_mut_obj_add_val(d, po, "stats", stats_json(d, &pl->stats));
            yyjson_mut_obj_add_val(d, po, "pstats", stats_json(d, &pl->post_stats));
        }
    }

    /* Write to a temp file then rename, so a crash never leaves a half-written cache. */
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    yyjson_write_err err;
    bool ok = yyjson_mut_write_file(tmp, d, YYJSON_WRITE_NOFLAG, NULL, &err);
    yyjson_mut_doc_free(d);
    if (!ok)
        return false;
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

/* ---------------------------- reading ---------------------------- */

static void load_stats(yyjson_val *obj, StatList *out)
{
    size_t ci, cmax;
    yyjson_val *key, *arr;
    yyjson_obj_foreach(obj, ci, cmax, key, arr) {
        const char *cat = intern(yyjson_get_str(key));
        size_t i, max;
        yyjson_val *pair;
        yyjson_arr_foreach(arr, i, max, pair) {
            Stat *s = statlist_push(out);
            s->cat = cat;
            s->name = intern(yyjson_get_str(yyjson_arr_get(pair, 0)));
            copy_str(s->val, sizeof s->val, yyjson_get_str(yyjson_arr_get(pair, 1)));
        }
    }
}

static const char *gs(yyjson_val *o, const char *k) { return yyjson_get_str(yyjson_obj_get(o, k)); }
static int gi(yyjson_val *o, const char *k) { return (int)yyjson_get_int(yyjson_obj_get(o, k)); }

#define LOAD_STR(dst, src, key) copy_str((dst), sizeof(dst), gs((src), (key)))

Season *store_load_mem(const char *buf, size_t len)
{
    yyjson_doc *doc = yyjson_read(buf, len, 0);
    if (!doc)
        return NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (gi(root, "version") != 1) {
        yyjson_doc_free(doc);
        return NULL;
    }
    Season *s = season_new(gi(root, "year"));
    LOAD_STR(s->fetched_at, root, "fetched_at");
    LOAD_STR(s->standings_at, root, "standings_at");
    s->has_structure = yyjson_get_bool(yyjson_obj_get(root, "structure"));

    size_t i, max;
    yyjson_val *o;
    yyjson_arr_foreach(yyjson_obj_get(root, "teams"), i, max, o) {
        Team *t = season_add_team(s, gs(o, "id") ? gs(o, "id") : "0");
        LOAD_STR(t->name, o, "name");
        LOAD_STR(t->abbr, o, "abbr");
        LOAD_STR(t->conf, o, "conf");
        LOAD_STR(t->division, o, "div");
        t->wins = gi(o, "w");
        t->losses = gi(o, "l");
        t->ties = gi(o, "t");
        t->pf = gi(o, "pf");
        t->pa = gi(o, "pa");
        t->win_pct = yyjson_get_real(yyjson_obj_get(o, "pct"));
        LOAD_STR(t->streak, o, "streak");
        t->seed = gi(o, "seed");
        LOAD_STR(t->overall, o, "overall");
        LOAD_STR(t->home, o, "home");
        LOAD_STR(t->road, o, "road");
        LOAD_STR(t->vs_div, o, "vsdiv");
        LOAD_STR(t->vs_conf, o, "vsconf");
        t->roster_loaded = yyjson_get_bool(yyjson_obj_get(o, "roster"));
        LOAD_STR(t->roster_at, o, "roster_at");
        load_stats(yyjson_obj_get(o, "stats"), &t->stats);
        LOAD_STR(t->post_record, o, "prec");
        load_stats(yyjson_obj_get(o, "pstats"), &t->post_stats);

        yyjson_val *parr = yyjson_obj_get(o, "players");
        size_t np = yyjson_arr_size(parr);
        if (np) {
            t->players = calloc(np, sizeof *t->players);
            size_t pi, pmax;
            yyjson_val *po;
            yyjson_arr_foreach(parr, pi, pmax, po) {
                Player *p = &t->players[pi];
                LOAD_STR(p->id, po, "id");
                LOAD_STR(p->name, po, "name");
                LOAD_STR(p->pos, po, "pos");
                LOAD_STR(p->jersey, po, "no");
                p->age = gi(po, "age");
                LOAD_STR(p->height, po, "ht");
                LOAD_STR(p->weight, po, "wt");
                LOAD_STR(p->exp, po, "exp");
                LOAD_STR(p->college, po, "college");
                LOAD_STR(p->status, po, "status");
                p->gp = gi(po, "gp");
                p->gs = gi(po, "gs");
                load_stats(yyjson_obj_get(po, "stats"), &p->stats);
                load_stats(yyjson_obj_get(po, "pstats"), &p->post_stats);
            }
            t->nplayers = (int)np;
        }
    }
    yyjson_doc_free(doc);
    return s;
}

Season *store_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    Season *s = got == (size_t)n ? store_load_mem(buf, got) : NULL;
    free(buf);
    return s;
}
