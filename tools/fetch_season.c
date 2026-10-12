/* huddle-fetch: download a complete NFL season from ESPN into a JSON snapshot.
 *
 *   huddle-fetch 2025 data/season_2025.json
 *
 * Run by `make` when data/season_2025.json is missing; the result is compiled
 * into the huddle binary. */
#include <stdio.h>
#include <stdlib.h>

#include "../src/espn.h"
#include "../src/http.h"
#include "../src/store.h"

static void progress(void *ud, const char *msg)
{
    (void)ud;
    fprintf(stderr, "  %s\n", msg);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s YEAR OUT.json\n", argv[0]);
        return 2;
    }
    int year = atoi(argv[1]);
    http_init();
    Season *s = season_new(year);
    UpdateOpts o = {.all_rosters = true};
    char summary[256];
    fprintf(stderr, "Downloading the %d NFL season from ESPN (this takes a minute or two)...\n", year);
    if (!espn_update(s, &o, progress, NULL, summary, sizeof summary)) {
        fprintf(stderr, "FAILED: %s\n", summary);
        return 1;
    }
    int players = 0, empty = 0;
    for (int i = 0; i < s->nteams; i++) {
        players += s->teams[i].nplayers;
        if (!s->teams[i].roster_loaded)
            empty++;
    }
    if (empty) {
        fprintf(stderr, "FAILED: %d team rosters did not download; refusing to write a partial snapshot.\n", empty);
        return 1;
    }
    if (!store_save(s, argv[2])) {
        fprintf(stderr, "FAILED: could not write %s\n", argv[2]);
        return 1;
    }
    fprintf(stderr, "%s\nWrote %s: %d teams, %d players.\n", summary, argv[2], s->nteams, players);
    season_free(s);
    return 0;
}
