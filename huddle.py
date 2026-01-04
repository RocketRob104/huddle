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
    }
    for name in (
        "Arizona Cardinals",
        "Atlanta Falcons",
        "Baltimore Ravens",
        "Buffalo Bills",
        "Carolina Panthers",
        "Chicago Bears",
        "Cincinnati Bengals",
        "Cleveland Browns",
        "Dallas Cowboys",
        "Denver Broncos",
        "Detroit Lions",
        "Green Bay Packers",
        "Houston Texans",
        "Indianapolis Colts",
        "Jacksonville Jaguars",
        "Kansas City Chiefs",
        "Las Vegas Raiders",
        "Los Angeles Chargers",
        "Los Angeles Rams",
        "Miami Dolphins",
        "Minnesota Vikings",
        "New England Patriots",
        "New Orleans Saints",
        "New York Giants",
        "New York Jets",
        "Philadelphia Eagles",
        "Pittsburgh Steelers",
        "San Francisco 49ers",
        "Seattle Seahawks",
        "Tampa Bay Buccaneers",
        "Tennessee Titans",
        "Washington Commanders",
    )
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


def _collect_entries(node: dict) -> list:
    """
    Walk the standings payload and return the first list of 'entries' found.

    ESPN wraps standings differently over time (sometimes under "children",
    other times under "standings"). This recursive search keeps the parser
    resilient to small schema shifts.
    """
    if not isinstance(node, dict):
        return []

    # If this node already has 'entries', we are done.
    if "entries" in node and isinstance(node["entries"], list):
        return node["entries"]

    # Sometimes standings are nested inside a 'standings' dict.
    if "standings" in node:
        entries = _collect_entries(node["standings"])
        if entries:
            return entries

    # Other common wrappers: children, groups, leagues, conferences, divisions.
    for key in ("children", "groups", "leagues", "conferences", "divisions"):
        if key in node:
            container = node[key]
            if isinstance(container, list):
                for child in container:
                    entries = _collect_entries(child)
                    if entries:
                        return entries
            elif isinstance(container, dict):
                entries = _collect_entries(container)
                if entries:
                    return entries

    # Nothing found at this level.
    return []


def parse_standings(raw_payload: dict) -> dict:
    """
    Convert the ESPN standings JSON into a simple mapping:
        { "Team Name": { "record": "10-7", "wins": 10, ... }, ... }
    """
    entries = _collect_entries(raw_payload)
    parsed = {}

    for entry in entries:
        team = entry.get("team", {})
        display_name = (
            team.get("displayName")
            or " ".join(filter(None, [team.get("location"), team.get("name")])).strip()
            or team.get("name", "Unknown Team")
        )

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
        self.status_text = tk.StringVar(value="Loading fallback teams...")

        # Data cache: team name -> performance dict.
        self.team_data = dict(FALLBACK_TEAM_DATA)

        # Build the interface before we hit the network so the app feels snappy.
        self._build_layout()
        self._populate_dropdown(sorted(self.team_data.keys()))

        # Kick off a background fetch so the UI thread never blocks.
        self._start_background_fetch()

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

        # Buttons for showing and refreshing data.
        self.show_button = tk.Button(
            row,
            text="Show Team Data",
            command=self.display_selected_team,
            bg=SNES_COLORS["purple"],
            fg="white",
            activebackground=SNES_COLORS["lavender"],
            activeforeground="black",
            relief="flat",
            padx=12,
            pady=6,
        )
        self.show_button.pack(side="left", padx=(0, 6))

        self.refresh_button = tk.Button(
            row,
            text="Refresh Data",
            command=self._start_background_fetch,
            bg=SNES_COLORS["dark_gray"],
            fg="white",
            activebackground=SNES_COLORS["lavender"],
            activeforeground="black",
            relief="flat",
            padx=10,
            pady=6,
        )
        self.refresh_button.pack(side="left")

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

    def _set_status(self, message: str) -> None:
        """Small helper to update the status label."""
        self.status_text.set(message)

    # ------------------------------------------------------------
    # Data fetching and UI updates
    # ------------------------------------------------------------
    def _start_background_fetch(self) -> None:
        """
        Spawn a thread to fetch live data. This prevents the GUI from freezing
        while waiting on the network.
        """
        # Disable the refresh button to avoid overlapping requests.
        self.refresh_button.config(state="disabled")
        self._set_status("Fetching latest standings from ESPN...")

        threading.Thread(target=self._fetch_and_apply_data, daemon=True).start()

    def _fetch_and_apply_data(self) -> None:
        """Worker thread: fetch data, parse it, then hand it back to the UI."""
        try:
            payload = fetch_json(TEAM_STANDINGS_URL)
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
        self.root.after(0, lambda: self._apply_new_data(parsed, error_msg))

    def _apply_new_data(self, parsed: dict | None, error_msg: str) -> None:
        """Update widgets after a background fetch completes."""
        if parsed:
            self.team_data = parsed
            names = sorted(parsed.keys())
            self._populate_dropdown(names)
            self._set_status("Standings refreshed from ESPN.")
        else:
            # Keep existing data (likely the fallback) and warn the user.
            self._set_status(
                "Using offline fallback data. Connect to the internet and press 'Refresh Data'."
            )
            if error_msg:
                # Show a dialog sparingly so it does not spam the user.
                messagebox.showwarning("HUDDLE", f"Could not fetch live data.\n\n{error_msg}")

        # Re-enable the refresh button now that the fetch is done.
        self.refresh_button.config(state="normal")

    def display_selected_team(self) -> None:
        """Render the selected team's numbers in the output text box."""
        team_name = self.selected_team.get()
        team_stats = self.team_data.get(team_name)

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

        self._set_status(f"Showing data for {team_name}.")


def main() -> None:
    """Entry point that launches the Tkinter event loop."""
    root = tk.Tk()
    app = HuddleApp(root)
    # Immediately show the first team's data so the screen is not empty.
    app.display_selected_team()
    root.mainloop()


if __name__ == "__main__":
    main()
