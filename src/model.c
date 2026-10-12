#include "model.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

Stat *statlist_push(StatList *l)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        Stat *nv = realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv)
            abort();
        l->v = nv;
        l->cap = nc;
    }
    Stat *s = &l->v[l->n++];
    memset(s, 0, sizeof *s);
    return s;
}

void statlist_free(StatList *l)
{
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

static void statlist_copy(StatList *dst, const StatList *src)
{
    dst->v = NULL;
    dst->n = dst->cap = 0;
    if (src->n == 0)
        return;
    dst->v = malloc((size_t)src->n * sizeof *dst->v);
    memcpy(dst->v, src->v, (size_t)src->n * sizeof *dst->v);
    dst->n = dst->cap = src->n;
}

Season *season_new(int year)
{
    Season *s = calloc(1, sizeof *s);
    s->year = year;
    return s;
}

void team_clear_roster(Team *t)
{
    for (int i = 0; i < t->nplayers; i++) {
        statlist_free(&t->players[i].stats);
        statlist_free(&t->players[i].post_stats);
    }
    free(t->players);
    t->players = NULL;
    t->nplayers = 0;
    t->roster_loaded = false;
    t->roster_at[0] = 0;
}

void team_clear_stats(Team *t)
{
    statlist_free(&t->stats);
    statlist_free(&t->post_stats);
    t->post_record[0] = 0;
}

void season_free(Season *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->nteams; i++) {
        team_clear_roster(&s->teams[i]);
        team_clear_stats(&s->teams[i]);
    }
    free(s->teams);
    free(s);
}

Season *season_clone(const Season *src)
{
    Season *s = calloc(1, sizeof *s);
    *s = *src;
    s->teams = NULL;
    s->cap = s->nteams;
    if (s->nteams)
        s->teams = malloc((size_t)s->nteams * sizeof *s->teams);
    for (int i = 0; i < src->nteams; i++) {
        Team *d = &s->teams[i];
        const Team *t = &src->teams[i];
        *d = *t;
        statlist_copy(&d->stats, &t->stats);
        statlist_copy(&d->post_stats, &t->post_stats);
        d->players = NULL;
        if (t->nplayers) {
            d->players = malloc((size_t)t->nplayers * sizeof *d->players);
            for (int p = 0; p < t->nplayers; p++) {
                d->players[p] = t->players[p];
                statlist_copy(&d->players[p].stats, &t->players[p].stats);
                statlist_copy(&d->players[p].post_stats, &t->players[p].post_stats);
            }
        }
    }
    return s;
}

Season *season_skeleton(const Season *from, int year)
{
    Season *s = season_new(year);
    for (int i = 0; i < from->nteams; i++) {
        Team *t = season_add_team(s, from->teams[i].id);
        copy_str(t->name, sizeof t->name, from->teams[i].name);
        copy_str(t->abbr, sizeof t->abbr, from->teams[i].abbr);
        copy_str(t->conf, sizeof t->conf, from->teams[i].conf);
        copy_str(t->division, sizeof t->division, from->teams[i].division);
    }
    return s;
}

Team *season_find_team(Season *s, const char *id)
{
    for (int i = 0; i < s->nteams; i++)
        if (!strcmp(s->teams[i].id, id))
            return &s->teams[i];
    return NULL;
}

Team *season_add_team(Season *s, const char *id)
{
    Team *t = season_find_team(s, id);
    if (t)
        return t;
    if (s->nteams == s->cap) {
        int nc = s->cap ? s->cap * 2 : 40;
        Team *nt = realloc(s->teams, (size_t)nc * sizeof *nt);
        if (!nt)
            abort();
        s->teams = nt;
        s->cap = nc;
    }
    t = &s->teams[s->nteams++];
    memset(t, 0, sizeof *t);
    copy_str(t->id, sizeof t->id, id);
    return t;
}
