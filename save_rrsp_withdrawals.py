"""
save_rrsp_withdrawals.py — Extract per-year RRSP withdrawal amounts from
retirement.csv and save them as a dated seed file for warm-start simulation runs.

The output CSV has three rows:
    Year,  2026, 2027, ..., 2069
    Ralph,    0, <wd>, ...,    0
    Sarah,     0, <wd>, ...,    0

Usage:
    python3 save_rrsp_withdrawals.py [retirement.csv]

The output file is written to the same directory as the input and is named:
    saved_rrsp_withdrawals_<YYYY-MM-DD>.csv
"""

import sys
import csv
import datetime
import os


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "retirement.csv"

    if not os.path.exists(src):
        print(f"Error: '{src}' not found.")
        sys.exit(1)

    # -------------------------------------------------------------------------
    # Read the semicolon-delimited retirement.csv
    # -------------------------------------------------------------------------
    with open(src, newline='', encoding='utf-8') as f:
        reader = csv.reader(f, delimiter=';')
        all_rows = list(reader)

    if not all_rows:
        print("Error: retirement.csv is empty.")
        sys.exit(1)

    header = all_rows[0]

    # Locate the debit columns for Ralph RRSP and Sarah RRSP.
    # They appear early in the header (before the per-asset detail block)
    # as plain labels written by write_asset_debits_to_file().
    try:
        year_col      = header.index("Year")
        ralph_rrsp_col = header.index("Ralph RRSP")
        sarah_rrsp_col  = header.index("Sarah RRSP")
    except ValueError as e:
        print(f"Error: could not find expected column in header: {e}")
        sys.exit(1)

    # -------------------------------------------------------------------------
    # Collect per-year data rows (skip blank lines and summary rows).
    # A data row starts with a 4-digit simulation year number.
    # -------------------------------------------------------------------------
    years_out = []
    ralph_out = []
    sarah_out  = []

    for row in all_rows[1:]:
        if not row:
            continue
        first = row[0].strip()
        if not first.isdigit() or len(first) != 4:
            continue  # summary label row or blank

        year_val  = first
        ralph_val = row[ralph_rrsp_col].strip() if ralph_rrsp_col < len(row) else "0"
        sarah_val  = row[sarah_rrsp_col].strip()  if sarah_rrsp_col  < len(row) else "0"

        # Strip any trailing decimal zeros produced by %.0f formatting.
        ralph_val = str(int(float(ralph_val))) if ralph_val else "0"
        sarah_val  = str(int(float(sarah_val)))  if sarah_val  else "0"

        years_out.append(year_val)
        ralph_out.append(ralph_val)
        sarah_out.append(sarah_val)

    if not years_out:
        print("Error: no simulation data rows found in retirement.csv.")
        sys.exit(1)

    # -------------------------------------------------------------------------
    # Write output CSV (comma-delimited, three rows)
    # -------------------------------------------------------------------------
    today    = datetime.date.today().isoformat()
    out_dir  = os.path.dirname(os.path.abspath(src))
    out_path = os.path.join(out_dir, f"saved_rrsp_withdrawals_{today}.csv")

    with open(out_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(["Year"]  + years_out)
        writer.writerow(["Ralph"] + ralph_out)
        writer.writerow(["Sarah"]  + sarah_out)

    print(f"Saved RRSP withdrawals → {out_path}  ({len(years_out)} years)")


if __name__ == "__main__":
    main()
