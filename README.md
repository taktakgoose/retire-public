# retirement-sim

A Monte Carlo retirement financial simulator written in C, with Python scripts to convert
the simulation output into a formatted Excel workbook and to save per-year RRSP withdrawal
schedules for use as warm-start seeds in subsequent refinement runs.

## Overview

The simulator models year-by-year cash flow, asset growth, tax, and withdrawal strategy for
two spouses from the current year through a projected end-of-life year. It runs Monte Carlo
trials with stochastic annual return paths plus varying RRSP withdrawal targets across both
spouses and writes the scenario with the highest chance of success to `retirement.csv`,
breaking ties by mean terminal net worth.

Three Python scripts post-process the simulation output:

`retirement_to_xlsx.py` reads the CSV and produces a polished Excel workbook with three sheets:

- **Simulation** — year-by-year data table with conditional formatting and frozen panes
- **Summary** — per-person and combined lifetime totals
- **Charts** — Net Worth & Expenses over time, Annual Income Sources (stacked bar), and Asset
  Values over time (stacked area)

`retirement_to_docx.py` reads the CSV and produces a Word action plan (`retirement_plan.docx`)
with one section per simulated year. Each year shows a tax summary, recommended actions (RRSP
withdrawals, T1032 pension-income splitting, tax installment reminders), account opening events
(LIF at 55, RRIF when first withdrawals begin, mandatory RRIF at 71), and a milestones banner
covering retirement, property payoffs, and children's education timelines.

`save_rrsp_withdrawals.py` extracts the per-year RRSP withdrawal amounts for both spouses from
`retirement.csv` and saves them as a dated seed file (`saved_rrsp_withdrawals_YYYY-MM-DD.csv`).
This seed file can be passed back to the simulator via `--rrsp` to run a targeted refinement
pass that perturbs the known-good schedule rather than searching blind.

## Assets & accounts modelled

- RRSP (registered retirement savings plan)
- DCPP / LIRA (defined contribution pension plan / locked-in retirement account)
- TFSA (tax-free savings account)
- Non-registered investments
- Real-estate properties (rental income and capital appreciation)
- CPP, OAS, and other income streams

## Requirements

**C simulator**

- GCC (or any C11-compatible compiler)
- Standard math library (`-lm`)

**Python scripts**

- Python 3.8+
- [openpyxl](https://openpyxl.readthedocs.io/) — Excel workbook writer
- [python-docx](https://python-docx.readthedocs.io/) — Word action plan writer

```
pip install openpyxl python-docx
```

## Building and running

### With Make (recommended)

```bash
# Build the C binary
make

# Build with debug symbols and no optimisation
make debug

# Run the simulation, convert to Excel, and save an RRSP seed file
make run

# Refine from the most-recent seed file (seeded MC mode)
make refine

# Run both CCA-disabled and CCA-enabled passes and compare (takes ~2× as long)
make cca-sweep

# Convert an existing retirement.csv to Excel (skip re-running the simulation)
make xlsx

# Generate the Word action plan from an existing retirement.csv (skip re-running)
make docx

# Save RRSP withdrawals from an existing retirement.csv (skip re-running)
make save

# Remove the binary, object files, and all generated files
make clean
```

### Simulation profiles

The simulator compiles different trial counts into the binary depending on the
profile. Each profile does a clean rebuild, runs the simulation, and then
automatically generates the Excel workbook and Word action plan.

```bash
# Fast iteration — 2 000 trials, narrow moonbrook-sale window (quick feedback)
make dev

# Planning quality — 10 000 trials, moderate moonbrook-sale window
make plan

# High quality — 30 000 trials, full moonbrook-sale sweep (slow; use for final runs)
make hq

# High quality + sequence-of-returns stress test
# Same trial counts as hq but applies −10 % then −5 % on investment assets in
# the two years immediately following the first spouse's retirement.
# Return-neutral: the deficit is spread back as a +0.58 %/yr boost across all
# other investment years so the long-run average return stays on target.
make hq-sor
```

### Manually

```bash
# Compile
gcc -O3 -fopenmp -Wall -std=c11 -o retire retire.c tax.c tables.c output.c -lm -fopenmp

# Run the simulation  →  produces retirement.csv
./retire

# Run with verbose year-by-year output printed to the terminal
./retire -v

# Run in seeded refinement mode using a previously saved RRSP schedule
./retire --rrsp saved_rrsp_withdrawals_2026-04-05.csv

# Also run the CCA-enabled pass and print a strategy comparison table
./retire --cca

# Convert to Excel  →  produces retirement_<date>.xlsx
python3 retirement_to_xlsx.py retirement.csv

# Generate Word action plan  →  produces retirement_plan_<date>.docx
python3 retirement_to_docx.py retirement.csv

# Save per-year RRSP withdrawals  →  produces saved_rrsp_withdrawals_<date>.csv
python3 save_rrsp_withdrawals.py retirement.csv
```

### With VS Code

A `tasks.json` is included in `.vscode/`. Press **Ctrl+Shift+B** to run the default
**Build** task, or open the Command Palette → *Tasks: Run Task* to access all five tasks:
Build, Build (debug), Run simulation, Convert CSV to Excel, and Clean.

## Output

| File | Description |
|---|---|
| `retirement.csv` | Raw simulation output (semicolon-delimited) |
| `retirement_<date>.xlsx` | Formatted Excel workbook with charts |
| `retirement_plan_<date>.docx` | Year-by-year Word action plan |
| `saved_rrsp_withdrawals_<date>.csv` | Per-year RRSP withdrawal seed file |

All generated files are excluded from version control via `.gitignore` since they can be
recreated at any time by running the simulator.

## Seeded refinement workflow

Running `make run` always produces a seed file alongside the Excel output. To iterate toward
a higher net worth, pass that seed back to the simulator:

```bash
make run     # initial search — saves retirement.csv, xlsx, docx, and seed file
make refine  # perturb the known-good schedule; overwrites output if improved
make refine  # repeat as many times as desired
```

`make refine` automatically picks up the most-recently dated seed file. Each refinement run
that finds a strategy with a higher success rate, or the same success rate with a higher mean
terminal net worth, overwrites `retirement.csv`, regenerates the Excel workbook, and saves an
updated seed file for the next pass.

## Customising the simulation

`scenario.h` is the primary file to edit. It contains all the personal and
financial parameters you'll adjust regularly:

| Constant | Description |
|---|---|
| `CURRENT_YEAR` / `DEATH_YEAR` | Simulation time span |
| `RALPH_YOB` / `SARAH_YOB` | Spouse birth years |
| `RALPH_RETIREMENT_YEAR` / `SARAH_RETIREMENT_YEAR` | Retirement dates |
| `STARTING_RALPH_RRSP` etc. | Starting account balances |
| `RALPH_SALARY` / `SARAH_SALARY` | Annual salaries |
| `RALPH_GROWTH_RATE` / `SARAH_GROWTH_RATE` etc. | Asset growth rates |
| `FINANCIAL_RETURN_VOLATILITY` etc. | Asset-class return volatility assumptions |
| `GO_GO_SPENDING` / `SLOW_GO_SPENDING` etc. | Projected yearly spending by phase |

`constants.h` holds stable values that change only when legislation changes —
tax rates, CRA limits (TFSA room, RRSP annual limit), capital-gains inclusion
rates, BPA amounts, and OAS/CPP survivor rules. You shouldn't need to touch
this file between scenario runs.

`params.h` is a thin shim that includes both files; existing tooling that
references `params.h` continues to work without modification.

Edit the constants in `scenario.h` and recompile to model different scenarios.

## Project structure

```
retirement-sim/
├── scenario.h                  # Personal parameters — edit for each scenario
├── constants.h                 # Tax rules and regulatory constants
├── params.h                    # Shim: #includes scenario.h and constants.h
├── types.h                     # Shared structs and enums
├── tax.h / tax.c               # Tax brackets, credits, and OAS clawback
├── tables.h / tables.c         # LIF and RRIF regulatory rate tables
├── output.h / output.c         # CSV writer and net-worth helper
├── retire.c                    # Simulation engine and main entry point
├── retirement_to_xlsx.py       # CSV → formatted Excel workbook
├── retirement_to_docx.py       # CSV → year-by-year Word action plan
├── save_rrsp_withdrawals.py    # Extract per-year RRSP withdrawals → seed CSV
├── Makefile                    # Build, run, refine, and clean targets
├── .vscode/tasks.json          # VS Code build and run tasks
├── .gitignore                  # Excludes binaries and generated files
└── README.md
```

## License

MIT
