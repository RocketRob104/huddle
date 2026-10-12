# HUDDLE (C edition)

An NFL viewer in C with the original Super Nintendo palette (grey shell, purple
and lavender). The first Python/Tk version is kept in `legacy/` for reference.

* **2025 season, built in.** Every team's standings and statistics and every
  player on a 2025 game-day roster (bio plus season stats) are downloaded from
  ESPN once, stored in `data/season_2025.json`, and compiled into the executable.
* **Current season, live.** The 2026 season is fetched when you press **Update**
  (standings, team stats and the selected team's roster) or **Update All Teams**
  (every roster; about a minute). The result is cached in `~/.huddle/` and
  reloaded on the next launch. Updates run on a background thread.
* **Postseason is separate.** The *Stats* toggle switches between Regular Season
  and Postseason. Postseason screens use amber heading bars (regular season is
  purple) and say "POSTSEASON" in the title, so the two are never mixed. The
  postseason standings view lists only playoff teams with their playoff record.
* **Snapshot history.** Every successful update also saves a timestamped copy in
  `~/.huddle/history/<year>/`. The *Snapshot* picker opens any of them
  (read-only, shown with an amber banner); choose "Latest" to resume updating.
  For 2025 the built-in download is listed as the baseline.

## Build and run

```bash
brew install sdl3 yyjson     # libcurl ships with macOS
make run
```

`make` downloads the 2025 season only if `data/season_2025.json` is missing.
`make refresh` re-downloads it. `make SEASON=2024` embeds a different year.

## Using it

Pick a team and season; tabs switch between Standings, Team (record plus about
280 team statistics) and Roster. Click a team in the standings or a player in a
roster to drill in. Mouse wheel, arrows, PgUp/PgDn scroll; `1` `2` `3` switch
tabs; `p` toggles postseason; `+`/`-` change text size; `Esc` goes back.

Testing without a window: `./huddle --dump standings|team|roster|player --team KC --season 2025 [--post]`

## Data notes

* ESPN's roster endpoints ignore the season and always return today's roster,
  so past-season rosters are rebuilt from that season's game-day rosters. Players
  who spent the whole year on the practice squad don't appear. Age and
  experience are shifted back to the season; GP/GS count regular-season games
  the player was dressed for.
* Regular-season and postseason statistics are stored separately. A player traded mid-season appears on
  both teams with ESPN's combined season totals.
* Some ESPN labels look odd (e.g. "Average Gain", "ESPN Quarterback Rating") and are shown as ESPN reports them.
* ESPN's API is unofficial and undocumented; it may change without notice.

## Layout

`src/espn.c` fetch + normalise, `src/store.c` JSON snapshot, `src/views.c` text
layout, `src/main.c` SDL3 GUI, `src/embedded.c` compiles the season into the
binary, `tools/fetch_season.c` is the build-time downloader.
