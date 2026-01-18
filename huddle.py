"""
HUDDLE (HUYDDLE Unifies Data for Deep Logical Evaluation)
---------------------------------------------------------

This module builds a simple desktop application that lets you pick any NFL
team from a dropdown and view that team's current performance numbers. The
interface leans on the Super Nintendo hardware palette (light gray body with
purple and lavender accents) and is filled with comments so that newcomers to
Python can follow along with every design decision.
"""

from __future__ import annotations

import json
import threading
from datetime import datetime
import tkinter as tk
from tkinter import ttk, messagebox
from urllib.error import URLError, HTTPError
from urllib.request import urlopen

# ------------------------------------------------------------
# Configuration: endpoints and palette
# ------------------------------------------------------------
# ESPN offers a free standings endpoint that does not require an API key.
# We only read public standings, so the app stays lightweight and dependency-free.
TEAM_STANDINGS_URL = "https://site.web.api.espn.com/apis/v2/sports/football/nfl/standings"

# SNES hardware-inspired palette. These colors echo the North American console:
# a neutral gray shell with purple buttons and lavender highlights.
SNES_COLORS = {
    "light_gray": "#cfcfcf",   # Console body
    "dark_gray": "#7f7f7f",    # Accent strips / header
    "purple": "#6b4ca5",       # Primary action color
    "lavender": "#b19cd9",     # Secondary panels / text areas
    "warning": "#e5b567",      # Soft highlight for status messages
}

TEAM_METADATA = {
    # AFC East
    "Buffalo Bills": {"conference": "AFC", "division": "AFC East"},
    "Miami Dolphins": {"conference": "AFC", "division": "AFC East"},
    "New England Patriots": {"conference": "AFC", "division": "AFC East"},
    "New York Jets": {"conference": "AFC", "division": "AFC East"},
    # AFC North
    "Baltimore Ravens": {"conference": "AFC", "division": "AFC North"},
    "Cincinnati Bengals": {"conference": "AFC", "division": "AFC North"},
    "Cleveland Browns": {"conference": "AFC", "division": "AFC North"},
    "Pittsburgh Steelers": {"conference": "AFC", "division": "AFC North"},
    # AFC South
    "Houston Texans": {"conference": "AFC", "division": "AFC South"},
    "Indianapolis Colts": {"conference": "AFC", "division": "AFC South"},
    "Jacksonville Jaguars": {"conference": "AFC", "division": "AFC South"},
    "Tennessee Titans": {"conference": "AFC", "division": "AFC South"},
    # AFC West
    "Denver Broncos": {"conference": "AFC", "division": "AFC West"},
    "Kansas City Chiefs": {"conference": "AFC", "division": "AFC West"},
    "Las Vegas Raiders": {"conference": "AFC", "division": "AFC West"},
    "Los Angeles Chargers": {"conference": "AFC", "division": "AFC West"},
    # NFC East
    "Dallas Cowboys": {"conference": "NFC", "division": "NFC East"},
    "New York Giants": {"conference": "NFC", "division": "NFC East"},
    "Philadelphia Eagles": {"conference": "NFC", "division": "NFC East"},
    "Washington Commanders": {"conference": "NFC", "division": "NFC East"},
    # NFC North
    "Chicago Bears": {"conference": "NFC", "division": "NFC North"},
    "Detroit Lions": {"conference": "NFC", "division": "NFC North"},
    "Green Bay Packers": {"conference": "NFC", "division": "NFC North"},
    "Minnesota Vikings": {"conference": "NFC", "division": "NFC North"},
    # NFC South
    "Atlanta Falcons": {"conference": "NFC", "division": "NFC South"},
    "Carolina Panthers": {"conference": "NFC", "division": "NFC South"},
    "New Orleans Saints": {"conference": "NFC", "division": "NFC South"},
    "Tampa Bay Buccaneers": {"conference": "NFC", "division": "NFC South"},
    # NFC West
    "Arizona Cardinals": {"conference": "NFC", "division": "NFC West"},
    "Los Angeles Rams": {"conference": "NFC", "division": "NFC West"},
    "San Francisco 49ers": {"conference": "NFC", "division": "NFC West"},
    "Seattle Seahawks": {"conference": "NFC", "division": "NFC West"},
}

# First season for each modern franchise so the year dropdown can run back
# to the true start of the organization.
FRANCHISE_START_YEARS = {
    "Arizona Cardinals": 1920,
    "Atlanta Falcons": 1966,
    "Baltimore Ravens": 1996,
    "Buffalo Bills": 1960,
    "Carolina Panthers": 1995,
    "Chicago Bears": 1920,
    "Cincinnati Bengals": 1968,
    "Cleveland Browns": 1946,
    "Dallas Cowboys": 1960,
    "Denver Broncos": 1960,
    "Detroit Lions": 1930,
    "Green Bay Packers": 1921,
    "Houston Texans": 2002,
    "Indianapolis Colts": 1953,
    "Jacksonville Jaguars": 1995,
    "Kansas City Chiefs": 1960,
    "Las Vegas Raiders": 1960,
    "Los Angeles Chargers": 1960,
    "Los Angeles Rams": 1937,
    "Miami Dolphins": 1966,
    "Minnesota Vikings": 1961,
    "New England Patriots": 1960,
    "New Orleans Saints": 1967,
    "New York Giants": 1925,
    "New York Jets": 1960,
    "Philadelphia Eagles": 1933,
    "Pittsburgh Steelers": 1933,
    "San Francisco 49ers": 1946,
    "Seattle Seahawks": 1976,
    "Tampa Bay Buccaneers": 1976,
    "Tennessee Titans": 1960,
    "Washington Commanders": 1932,
}
EARLIEST_FRANCHISE_YEAR = min(FRANCHISE_START_YEARS.values())

# A fallback list of teams so the dropdown always has content even when offline.
# Performance numbers are intentionally empty so we do not mislead with stale data.
FALLBACK_TEAM_DATA = {
    name: {
        "record": "No live data yet.",
        "wins": None,
        "losses": None,
        "ties": None,
        "points_for": None,
        "points_against": None,
        "win_pct": None,
        "streak": None,
        "note": "Press 'Refresh Data' once you are online to pull standings.",
        "conference": meta["conference"],
        "division": meta["division"],
        "conference_rank": None,
    }
    for name, meta in TEAM_METADATA.items()
}


def fetch_json(url: str, timeout: int = 10) -> dict:
    """
    Fetch JSON from the provided URL using only the standard library.

    This helper keeps network logic in one place. If the fetch fails, callers
    can catch the exception and decide how to fall back.
    """
    with urlopen(url, timeout=timeout) as response:
        raw_bytes = response.read()
    return json.loads(raw_bytes.decode("utf-8"))


def current_season_year(today: datetime | None = None) -> int:
    """Return the season year that most likely represents the current NFL season."""
    now = today or datetime.now()
    # NFL seasons start in early fall, so before July we assume the prior season.
    return now.year if now.month >= 7 else now.year - 1


def standings_url_for_year(season_year: int) -> str:
    """Build the ESPN standings endpoint for a specific season year."""
    return f"{TEAM_STANDINGS_URL}?season={season_year}&seasontype=2"


def _collect_entries(node: dict | list, conference: str | None = None) -> list[tuple[dict, str | None]]:
    """
    Walk the standings payload and return every team entry alongside its conference.

    ESPN wraps standings differently over time (sometimes under "children",
    other times under "standings"). This recursive search keeps the parser
    resilient to small schema shifts and collects entries across conferences
    instead of stopping after the first 16 teams.
    """
    # Allow callers to pass either dicts or lists; treat lists as containers.
    if isinstance(node, list):
        paired_entries = []
        for item in node:
            paired_entries.extend(_collect_entries(item, conference))
        return paired_entries

    if not isinstance(node, dict):
        return []

    # Determine whether this node represents a conference boundary.
    current_conference = conference
    if node.get("isConference"):
        current_conference = (
            node.get("abbreviation") or node.get("shortName") or node.get("name") or conference
        )
    elif node.get("abbreviation") in {"AFC", "NFC"}:
        current_conference = node.get("abbreviation")
    elif node.get("name") in {"American Football Conference", "National Football Conference"}:
        current_conference = node.get("abbreviation") or node.get("shortName") or node.get("name")

    paired_entries = []

    # If this node already has 'entries', gather them but continue searching.
    if "entries" in node and isinstance(node["entries"], list):
        for entry in node["entries"]:
            paired_entries.append((entry, current_conference))

    # Common wrappers: standings, children, groups, leagues, conferences, divisions.
    for key in ("standings", "children", "groups", "leagues", "conferences", "divisions"):
        if key in node:
            paired_entries.extend(_collect_entries(node[key], current_conference))

    return paired_entries


def parse_standings(raw_payload: dict) -> dict:
    """
    Convert the ESPN standings JSON into a simple mapping:
        { "Team Name": { "record": "10-7", "wins": 10, ... }, ... }
    """
    entries = _collect_entries(raw_payload)
    parsed = {}

    seen_ids = set()

    for entry, conference in entries:
        team = entry.get("team", {})
        team_id = team.get("id")

        # Avoid duplicates when the payload lists teams under multiple parents.
        if team_id:
            if team_id in seen_ids:
                continue
            seen_ids.add(team_id)

        display_name = (
            team.get("displayName")
            or " ".join(filter(None, [team.get("location"), team.get("name")])).strip()
            or team.get("name", "Unknown Team")
        )
        meta = TEAM_METADATA.get(display_name, {})

        # Stats arrive as a list of {"name": "...", "value": ...}.
        stats_block = entry.get("stats", [])
        stats = {item.get("name"): item.get("value") for item in stats_block if "name" in item}
        display_values = {item.get("name"): item.get("displayValue") for item in stats_block if "name" in item}

        wins = int(stats.get("wins", 0) or 0)
        losses = int(stats.get("losses", 0) or 0)
        ties = int(stats.get("ties", 0) or 0)
        points_for = stats.get("pointsFor")
        points_against = stats.get("pointsAgainst")
        win_pct = stats.get("winPercent")
        streak = display_values.get("streak") or stats.get("streak")
        seed_raw = stats.get("playoffSeed")
        try:
            conference_rank = int(seed_raw) if seed_raw is not None else None
        except (TypeError, ValueError):
            conference_rank = None

        # Build a friendly record string. Example: "12-5" or "9-7-1" when tied.
        record_text = f"{wins}-{losses}" if ties == 0 else f"{wins}-{losses}-{ties}"

        parsed[display_name] = {
            "record": record_text,
            "wins": wins,
            "losses": losses,
            "ties": ties,
            "points_for": points_for,
            "points_against": points_against,
            "win_pct": win_pct,
            "streak": streak,
            "note": entry.get("note", {}).get("text"),
            "conference": conference or meta.get("conference"),
            "division": meta.get("division"),
            "conference_rank": conference_rank,
        }

    # Returning an empty dict signals to the caller that parsing failed.
    return parsed


class HuddleApp:
    """
    All HUDDLE behavior is packaged into this Tkinter-based class.

    UI and data concerns live together here so the script stays single-file and
    easy to tweak. Each helper method has a docstring and inline comments to
    explain the reasoning behind layout or data choices.
    """

    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("HUDDLE - NFL Dashboard")
        self.root.configure(bg=SNES_COLORS["light_gray"])

        # Tkinter variables give us two-way binding between code and widgets.
        self.selected_team = tk.StringVar()
        self.selected_year = tk.StringVar()
        self.status_text = tk.StringVar(value="Loading fallback teams...")

        # Data cache: team name -> performance dict.
        self.team_data = dict(FALLBACK_TEAM_DATA)
        self.team_data_by_year: dict[int, dict] = {}
        self.active_fetches: set[int] = set()
        self.current_year = current_season_year()
        self.showing_standings = False

        # Build the interface before we hit the network so the app feels snappy.
        self._build_layout()
        self._populate_dropdown(sorted(TEAM_METADATA.keys()))
        self._update_year_dropdown(self.selected_team.get())
        self._set_current_year_data(self._get_selected_year() or self.current_year)
        self.display_standings()

        # Kick off a background fetch so the UI thread never blocks.
        self._start_background_fetch(self.current_year)

    # ------------------------------------------------------------
    # UI construction helpers
    # ------------------------------------------------------------
    def _build_layout(self) -> None:
        """Create and arrange all widgets for the main window."""
        header = tk.Frame(self.root, bg=SNES_COLORS["dark_gray"], padx=12, pady=12)
        header.pack(fill="x")

        tk.Label(
            header,
            text="HUDDLE • NFL Performance Viewer",
            font=("Helvetica", 16, "bold"),
            fg="white",
            bg=SNES_COLORS["dark_gray"],
        ).pack(anchor="w")

        tk.Label(
            header,
            text="Select a team, fetch its current record, and explore quick stats.",
            font=("Helvetica", 10),
            fg="white",
            bg=SNES_COLORS["dark_gray"],
        ).pack(anchor="w", pady=(4, 0))

        content = tk.Frame(self.root, bg=SNES_COLORS["light_gray"], padx=12, pady=12)
        content.pack(fill="both", expand=True)

        # Row for dropdown and buttons.
        row = tk.Frame(content, bg=SNES_COLORS["light_gray"])
        row.pack(fill="x", pady=(0, 10))

        tk.Label(
            row,
            text="Team:",
            font=("Helvetica", 11, "bold"),
            bg=SNES_COLORS["light_gray"],
        ).pack(side="left", padx=(0, 6))

        # ttk widgets are more modern; we style them with the SNES palette.
        style = ttk.Style()
        style.theme_use("clam")
        style.configure(
            "Snes.TCombobox",
            fieldbackground=SNES_COLORS["lavender"],
            background=SNES_COLORS["lavender"],
            foreground="black",
            arrowcolor=SNES_COLORS["purple"],
        )

        self.team_dropdown = ttk.Combobox(
            row,
            textvariable=self.selected_team,
            state="readonly",
            width=32,
            style="Snes.TCombobox",
        )
        self.team_dropdown.pack(side="left", padx=(0, 10))
        self.team_dropdown.bind("<<ComboboxSelected>>", lambda event: self._on_team_change())

        tk.Label(
            row,
            text="Year:",
            font=("Helvetica", 11, "bold"),
            bg=SNES_COLORS["light_gray"],
        ).pack(side="left", padx=(0, 6))

        self.year_dropdown = ttk.Combobox(
            row,
            textvariable=self.selected_year,
            state="readonly",
            width=8,
            style="Snes.TCombobox",
        )
        self.year_dropdown.pack(side="left", padx=(0, 10))
        self.year_dropdown.bind("<<ComboboxSelected>>", lambda event: self._on_year_change())

        self.refresh_button = tk.Button(
            row,
            text="Refresh Data",
            command=lambda: self._start_background_fetch(force=True),
            bg=SNES_COLORS["lavender"],
            fg="black",
            activebackground=SNES_COLORS["purple"],
            activeforeground="white",
            relief="flat",
            padx=10,
            pady=6,
        )
        self.refresh_button.pack(side="left", padx=(0, 6))

        self.standings_button = tk.Button(
            row,
            text="Standings",
            command=self.display_standings,
            bg=SNES_COLORS["lavender"],
            fg="black",
            activebackground=SNES_COLORS["purple"],
            activeforeground="white",
            relief="flat",
            padx=10,
            pady=6,
        )
        self.standings_button.pack(side="left", padx=(6, 0))

        # Status line to keep the user informed.
        self.status_label = tk.Label(
            content,
            textvariable=self.status_text,
            bg=SNES_COLORS["light_gray"],
            fg=SNES_COLORS["dark_gray"],
            font=("Helvetica", 10),
            anchor="w",
        )
        self.status_label.pack(fill="x", pady=(0, 10))

        # Text area where results are shown.
        self.output = tk.Text(
            content,
            height=12,
            bg=SNES_COLORS["lavender"],
            fg="black",
            font=("Courier New", 11),
            relief="flat",
            padx=12,
            pady=12,
            state="disabled",  # Prevent user edits.
            wrap="word",
        )
        self.output.pack(fill="both", expand=True)

    def _populate_dropdown(self, team_names: list[str]) -> None:
        """Load team names into the dropdown and select the first by default."""
        self.team_dropdown["values"] = team_names
        if team_names:
            self.selected_team.set(team_names[0])

    def _populate_year_dropdown(self, years: list[int], selected_year: int | None = None) -> None:
        """Load season years into the dropdown, preferring the provided year."""
        year_strings = [str(year) for year in years]
        self.year_dropdown["values"] = year_strings
        if not years:
            return
        if selected_year is None or selected_year not in years:
            selected_year = years[0]
        self.selected_year.set(str(selected_year))

    def _get_selected_year(self) -> int | None:
        """Return the year from the dropdown, if one is selected."""
        try:
            return int(self.selected_year.get())
        except (TypeError, ValueError):
            return None

    def _years_for_team(self, team_name: str) -> list[int]:
        """Generate a descending list of valid years for the selected franchise."""
        start_year = FRANCHISE_START_YEARS.get(team_name, EARLIEST_FRANCHISE_YEAR)
        return list(range(self.current_year, start_year - 1, -1))

    def _update_year_dropdown(self, team_name: str) -> None:
        """Refresh year dropdown values when a team changes."""
        years = self._years_for_team(team_name)
        current_year = self._get_selected_year()
        if current_year is None or current_year not in years:
            current_year = self.current_year if self.current_year in years else years[0]
        self._populate_year_dropdown(years, current_year)

    def _set_current_year_data(self, year: int) -> None:
        """Update the active dataset to match the currently selected year."""
        if year in self.team_data_by_year:
            self.team_data = self.team_data_by_year[year]
        else:
            self.team_data = dict(FALLBACK_TEAM_DATA)

    def _on_team_change(self) -> None:
        """Handle team dropdown changes by adjusting year range and output."""
        self._update_year_dropdown(self.selected_team.get())
        self.showing_standings = False
        self._refresh_current_view(fetch_if_missing=True)

    def _on_year_change(self) -> None:
        """Handle year dropdown changes by fetching data and refreshing output."""
        self.showing_standings = False
        self._refresh_current_view(fetch_if_missing=True)

    def _refresh_current_view(self, fetch_if_missing: bool = False) -> None:
        """Refresh the display based on the selected team/year."""
        year = self._get_selected_year() or self.current_year
        if fetch_if_missing and year not in self.team_data_by_year:
            self._start_background_fetch(year)
        self._set_current_year_data(year)
        if self.showing_standings:
            self.display_standings()
        else:
            self.display_selected_team()

    def _set_status(self, message: str) -> None:
        """Small helper to update the status label."""
        self.status_text.set(message)

    # ------------------------------------------------------------
    # Data fetching and UI updates
    # ------------------------------------------------------------
    def _start_background_fetch(self, year: int | None = None, force: bool = False) -> None:
        """
        Spawn a thread to fetch live data. This prevents the GUI from freezing
        while waiting on the network.
        """
        fetch_year = year or self._get_selected_year() or self.current_year
        if not force and fetch_year in self.team_data_by_year:
            return
        if fetch_year in self.active_fetches:
            return
        self.active_fetches.add(fetch_year)
        # Disable the refresh button to avoid overlapping requests.
        self.refresh_button.config(state="disabled")
        self._set_status(f"Fetching standings for {fetch_year} from ESPN...")

        threading.Thread(
            target=self._fetch_and_apply_data,
            args=(fetch_year,),
            daemon=True,
        ).start()

    def _fetch_and_apply_data(self, year: int) -> None:
        """Worker thread: fetch data, parse it, then hand it back to the UI."""
        try:
            payload = fetch_json(standings_url_for_year(year))
            parsed = parse_standings(payload)
            if not parsed:
                raise ValueError("Standings payload missing expected fields.")
        except (HTTPError, URLError, TimeoutError, ValueError, json.JSONDecodeError) as exc:
            # Swallow errors and fall back to local placeholders.
            parsed = None
            error_msg = f"Live fetch failed: {exc}"
        else:
            error_msg = ""

        # Move back onto the Tk thread to touch widgets.
        self.root.after(0, lambda: self._apply_new_data(year, parsed, error_msg))

    def _apply_new_data(self, year: int, parsed: dict | None, error_msg: str) -> None:
        """Update widgets after a background fetch completes."""
        is_selected_year = year == (self._get_selected_year() or self.current_year)
        if parsed:
            self.team_data_by_year[year] = parsed
            if is_selected_year:
                self.team_data = parsed
                if self.showing_standings:
                    self.display_standings()
                else:
                    self.display_selected_team()
                self._set_status(f"Standings refreshed for {year}.")
        else:
            # Keep existing data (likely the fallback) and warn the user.
            if is_selected_year:
                self._set_status(
                    f"Using offline fallback data for {year}. "
                    "Connect to the internet and press 'Refresh Data'."
                )
                if error_msg:
                    # Show a dialog sparingly so it does not spam the user.
                    messagebox.showwarning("HUDDLE", f"Could not fetch live data.\n\n{error_msg}")

        self.active_fetches.discard(year)
        # Re-enable the refresh button now that the fetch is done.
        if not self.active_fetches:
            self.refresh_button.config(state="normal")

    def display_selected_team(self) -> None:
        """Render the selected team's numbers in the output text box."""
        self.showing_standings = False
        team_name = self.selected_team.get()
        team_stats = self.team_data.get(team_name)
        season_year = self._get_selected_year() or self.current_year

        if not team_stats:
            # This should not happen, but guard against it.
            self._set_status("No data for that team yet; try refreshing.")
            return

        # Compute a points differential when we have both values.
        pf = team_stats.get("points_for")
        pa = team_stats.get("points_against")
        differential = pf - pa if pf is not None and pa is not None else None

        # Build display lines. Simple strings keep this readable.
        lines = [
            f"Team: {team_name}",
            f"Season: {season_year}",
            f"Record: {team_stats.get('record', 'N/A')}",
            f"Wins: {team_stats.get('wins', 'N/A')} | "
            f"Losses: {team_stats.get('losses', 'N/A')} | "
            f"Ties: {team_stats.get('ties', 'N/A')}",
            f"Win %: {team_stats.get('win_pct', 'N/A')}",
            f"Points For: {pf if pf is not None else 'N/A'}",
            f"Points Against: {pa if pa is not None else 'N/A'}",
            f"Point Differential: {differential if differential is not None else 'N/A'}",
            f"Streak: {team_stats.get('streak', 'N/A')}",
        ]

        note = team_stats.get("note")
        if note:
            lines.append(f"Note: {note}")

        # Update the text widget safely.
        self.output.config(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.insert("1.0", "\n".join(lines))
        self.output.config(state="disabled")

        self._set_status(f"Showing data for {team_name} ({season_year}).")

    def display_standings(self) -> None:
        """List conference standings (1-16) with division breakdowns side by side."""
        self.showing_standings = True
        if not self.team_data:
            self._set_status("No standings available; try refreshing.")
            return
        season_year = self._get_selected_year() or self.current_year

        conferences: dict[str, list[tuple[str, dict]]] = {}
        divisions: dict[str, list[tuple[str, dict]]] = {}
        for team, data in self.team_data.items():
            conference = data.get("conference") or "Unknown Conference"
            conferences.setdefault(conference, []).append((team, data))
            division = data.get("division") or "Unknown Division"
            divisions.setdefault(division, []).append((team, data))

        def sort_key(item: tuple[str, dict]) -> tuple:
            team, data = item
            rank = data.get("conference_rank")
            win_pct = data.get("win_pct")
            try:
                win_pct_val = float(win_pct) if win_pct is not None else 0.0
            except (TypeError, ValueError):
                win_pct_val = 0.0
            wins = data.get("wins") or 0
            losses = data.get("losses")
            losses_val = losses if losses is not None else 0
            return (
                0 if rank is not None else 1,  # Put ranked teams first.
                rank if rank is not None else 999,
                -win_pct_val,
                -wins,
                losses_val,
                team,
            )

        timestamp = datetime.now().strftime("%Y-%m-%d %I:%M %p")
        left_lines = [f"Conference Standings ({season_year})", ""]
        for conference in sorted(conferences.keys()):
            left_lines.append(f"{conference} Standings")
            sorted_teams = sorted(conferences[conference], key=sort_key)
            for idx, (team, data) in enumerate(sorted_teams, start=1):
                seed = data.get("conference_rank") or idx
                left_lines.append(f"{seed}. {team} ({data.get('record', 'N/A')})")
            left_lines.append("")  # Spacer between conferences.
        if left_lines and left_lines[-1] == "":
            left_lines.pop()

        division_order = [
            "AFC East",
            "AFC North",
            "AFC South",
            "AFC West",
            "NFC East",
            "NFC North",
            "NFC South",
            "NFC West",
        ]
        extras = [d for d in sorted(divisions.keys()) if d not in division_order]
        right_lines = ["Division Standings", ""]
        for division in division_order + extras:
            teams = divisions.get(division)
            if not teams:
                continue
            right_lines.append(division)
            sorted_division = sorted(teams, key=sort_key)
            for idx, (team, data) in enumerate(sorted_division, start=1):
                right_lines.append(f"{idx}. {team} ({data.get('record', 'N/A')})")
            right_lines.append("")
        if right_lines and right_lines[-1] == "":
            right_lines.pop()

        col_width = 42
        combined_lines = [f"Season: {season_year}", f"Current as of: {timestamp}", ""]
        max_lines = max(len(left_lines), len(right_lines))
        for i in range(max_lines):
            left = left_lines[i] if i < len(left_lines) else ""
            right = right_lines[i] if i < len(right_lines) else ""
            combined_lines.append(f"{left.ljust(col_width)}{right}")

        self.output.config(state="normal")
        self.output.delete("1.0", tk.END)
        self.output.insert("1.0", "\n".join(combined_lines))
        self.output.config(state="disabled")

        self._set_status(f"Showing conference standings for {season_year}.")


def main() -> None:
    """Entry point that launches the Tkinter event loop."""
    root = tk.Tk()
    app = HuddleApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
