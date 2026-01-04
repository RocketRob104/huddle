# HUDDLE

HUDDLE — HUYDDLE Unifies Data for Deep Logical Evaluation

Overview
- This small Python app shows a GUI to select NFL team names and view
  basic performance metrics and a small chart.

Getting started
1. Create a virtual environment (recommended):

```bash
python -m venv .venv
source .venv/bin/activate
```

2. Install dependencies:

```bash
pip install -r requirements.txt
```

3. Run the app:

```bash
python huddle.py
```

Usage notes
- The app includes a sample CSV at `data/teams_sample.csv` so it runs
  immediately.
- Use the "Import CSV" button to load your own dataset. Your CSV
  should contain a `Team` column; common metric column names used by
  the app are `Wins`, `Losses`, `PointsFor`, `PointsAgainst`,
  `Yards`, `Turnovers`.

If you want live NFL stats or API integration, tell me which API you
prefer and I can extend HUDDLE to fetch live data.
