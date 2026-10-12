#ifndef HUDDLE_MODEL_H
#define HUDDLE_MODEL_H
#include <stdbool.h>

/* One statistic line. cat/name are interned (never freed); val is ESPN's
 * display string, e.g. "3,394". */
typedef struct {
    const char *cat;
    const char *name;
    char val[20];
} Stat;

typedef struct {
    Stat *v;
    int n, cap;
} StatList;

typedef struct {
    char id[16];
    char name[48];
    char pos[8];
    char jersey[6];
    int age;
    char height[12];
    char weight[12];
    char exp[12];
    char college[48];
    char status[24];   /* current-season rosters only; blank for past seasons */
    int gp, gs;        /* games played / started (regular season); 0 = unknown */
    StatList stats;      /* regular-season stats, zero-valued stats omitted */
    StatList post_stats; /* postseason stats (separate), empty if none       */
} Player;

typedef struct {
    char id[8];
    char name[40];
    char abbr[8];
    char conf[8];      /* AFC / NFC            */
    char division[16]; /* "AFC East"           */
    int wins, losses, ties;
    int pf, pa;
    double win_pct;
    char streak[8];
    int seed;          /* playoff seed, 0 = none */
    char overall[12], home[12], road[12], vs_div[12], vs_conf[12];
    StatList stats;    /* team regular-season statistics */
    StatList post_stats; /* team postseason statistics; empty = missed the playoffs */
    char post_record[12]; /* postseason W-L, e.g. "3-1"; empty = no playoffs */
    Player *players;
    int nplayers;
    bool roster_loaded;
    char roster_at[24];
} Team;

typedef struct {
    int year;
    char fetched_at[24];   /* last successful update of anything     */
    char standings_at[24]; /* last standings refresh                 */
    bool has_structure;    /* conference/division/team names fetched */
    Team *teams;
    int nteams, cap;
} Season;

Season *season_new(int year);
void season_free(Season *s);
Season *season_clone(const Season *s);
/* Same teams/divisions as `from`, but no records, stats or rosters. */
Season *season_skeleton(const Season *from, int year);
Team *season_find_team(Season *s, const char *id);
/* Finds or appends. May move s->teams, so don't hold Team pointers across it. */
Team *season_add_team(Season *s, const char *id);

void team_clear_roster(Team *t);
void team_clear_stats(Team *t);

Stat *statlist_push(StatList *l);
void statlist_free(StatList *l);

#endif
