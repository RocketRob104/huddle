#ifndef HUDDLE_ESPN_H
#define HUDDLE_ESPN_H
#include <stdbool.h>
#include <stddef.h>

#include "model.h"

typedef void (*ProgressFn)(void *ud, const char *msg);

typedef struct {
    bool all_rosters;      /* download every team's roster + player stats  */
    const char *team_id;   /* otherwise: just this team's roster (or NULL) */
} UpdateOpts;

/* Refresh `s` in place from ESPN: structure (first time), standings, team
 * statistics, then rosters + per-player statistics as requested.
 * Returns false if nothing useful could be fetched; `summary` explains either way.
 * Safe to call on a private copy from a worker thread. */
bool espn_update(Season *s, const UpdateOpts *o, ProgressFn pf, void *ud, char *summary, size_t sn);

/* The season ESPN would currently call "this season" (rolls over in July). */
int espn_current_season_year(void);

#endif
