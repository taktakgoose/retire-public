/*
 * tables.h - LIF and RRIF regulatory lookup tables.
 */

#pragma once

/* Return the maximum LIF withdrawal rate for the given age (Ontario).
 * Returns 0 if the age is below the minimum in the table. */
double get_lif_max_rate(int age);

/* Return the LIF annual dollar withdrawal limit for a given calendar year.
 * Returns 0 if the year is not in the table. */
double get_lif_withdrawal_limit(int year);

/* Return the minimum RRIF withdrawal rate for the given age.
 * Returns 0 if the age is below the minimum in the table. */
double get_rrif_min_rate(int age);
