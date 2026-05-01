/*
 * output.h - CSV output functions for the retirement simulation.
 */

#pragma once

#include <stdio.h>
#include "types.h"

/* Compute after-tax net worth at simulation year index i.
 * Terminal taxes are estimated per spouse using one stacked tax return that
 * combines ordinary income sources and included capital gains. */
double sum_networth(const year_t *years, int i);

/* Write the best simulation result to the given CSV file.
 * Call "make xlsx" or run retirement_to_xlsx.py separately to produce Excel.
 * survivor_mode / survivor_spouse / survivor_year: pass the matching g_survivor_*
 * globals so the CSV summary carries a "Survivor" row that both post-processing
 * scripts can read back for the Assumptions tab and milestones banner. */
/* Write the simulation result and optional P10/P50/P90 band rows to filename.
 * Pass p10/p50/p90 as NULL to omit the band rows (e.g. if the percentile
 * pass was skipped due to an allocation failure). */
void write_simulation_to_file(const year_t *years, int num_years,
                              const char *filename,
                              int  representative_successes,
                              int  representative_total_paths,
                              double representative_mean_networth,
                              double representative_path_networth,
                              bool survivor_mode,
                              int  survivor_spouse,
                              int  survivor_year,
                              const double *p10,
                              const double *p50,
                              const double *p90);
