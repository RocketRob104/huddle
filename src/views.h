#ifndef HUDDLE_VIEWS_H
#define HUDDLE_VIEWS_H
#include <stdbool.h>
#include <stdint.h>

#include "model.h"

/* The output panel is a list of Lines; each Line holds up to three Cells laid
 * out in fixed-width character columns. Cells may be clickable. */
typedef struct {
    char text[176];
    uint32_t fg;     /* 0xRRGGBB */
    int x, w;        /* column start and width, in characters */
    bool heading;    /* drawn as a bar: purple/white, or amber/black when accent (postseason) */
    bool accent;
    int team;        /* index into Season.teams to open when clicked, or -1 */
    int player;      /* index into Team.players to open when clicked, or -1 */
} Cell;

typedef struct {
    Cell c[3];
    int n;
} Line;

typedef struct {
    Line *v;
    int n, cap;
} Lines;

void lines_clear(Lines *L);
void lines_free(Lines *L);

/* `chars` is the panel width in characters. */
void build_standings(Lines *L, const Season *s, int chars, bool post);
void build_team(Lines *L, const Season *s, const Team *t, int chars, bool post);
void build_roster(Lines *L, const Season *s, const Team *t, int team_idx, int chars);
void build_player(Lines *L, const Season *s, const Team *t, const Player *p, int chars, bool post);

#endif
