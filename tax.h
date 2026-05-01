/*
 * tax.h - Tax bracket calculations for the retirement simulation.
 */

#pragma once

/* Pre-compute per-year inflation factors used by the tax functions.
 * Must be called once before the simulation loop. */
void   init_tax_tables(void);

/* Inflate all bracket thresholds by one year's inflation. */
void   increment_tax_brackets(void);

/* Reset all bracket thresholds to their base (CURRENT_YEAR) values.
 * Must be called at the start of each simulation trial to ensure each
 * Monte Carlo run uses the same starting brackets. */
void   reset_tax_brackets(void);

/* Return total taxes owed on the given income using the current brackets. */
double taxes_owing_on_income(double income);

/* Return the basic personal amount non-refundable tax credit for the
 * given calendar year. */
double calc_personal_exemption(int cy, double income);

/* Return the combined federal + Ontario Age Amount non-refundable tax
 * credit for a spouse of the given age and net income in calendar year cy.
 * Returns 0 if age < 65 or the credit is fully phased out. */
double calc_age_amount_credit(int age, double net_income, int cy);

/* Return the OAS Recovery Tax (clawback) for a spouse with the given net
 * income and OAS amount received in calendar year cy.
 * The result is capped at oas_received. */
double calc_oas_clawback(double net_income, double oas_received, int cy);

/* Return the inflation-indexed OAS clawback threshold for calendar year cy.
 * Use this to estimate clawback risk before committing to a withdrawal. */
double get_oas_clawback_threshold(int cy);

/* Return the cumulative inflation factor for calendar year cy
 * (i.e. (1 + INFLATION) ^ (cy - CURRENT_YEAR)).  Used by callers that
 * want to scale a CURRENT_YEAR-dollar amount into year-cy nominal
 * dollars without recomputing pow() themselves. */
double get_inflation_factor(int cy);

/* Return the already-inflated income ceiling below which capital-gain
 * harvesting is attractive (top of the second combined bracket, ~$58,523
 * in 2026, ~29.65% combined rate starts above this). */
double get_harvest_income_ceiling(void);

/*
 * Return the remaining income capacity in the first combined bracket.
 *
 * The first bracket ceiling (brackets[1].threshold, ~$53,891 in 2026,
 * inflation-indexed each year) is the point at which the marginal rate
 * jumps from ~19.05% to ~23.15%.  Income below this ceiling is the
 * cheapest available rate for registered withdrawals.
 *
 * During working years salary already exceeds the ceiling, so this
 * returns 0.  In retirement, when income is modest, it tells the caller
 * how much more RRSP/RRIF income can be taken at the lowest rate.
 */
double calc_first_bracket_room(double current_income);

/*
 * Return the portion of a capital gain that is included in taxable income
 * under the two-tier 2024+ rules:
 *   ≤ CAPITAL_GAIN_ANNUAL_THRESHOLD  →  gain × RATE_LOW  (50%)
 *   > threshold                      →  threshold × RATE_LOW
 *                                     + (gain − threshold) × RATE_HIGH (66.67%)
 */
double calc_capital_gain_inclusion(double gain);

/*
 * Inverse of calc_capital_gain_inclusion: given a budget of included income
 * (room) and the capital gains already realized by this individual this year
 * (current_gains), return the maximum additional raw gain that fits within
 * that room, correctly accounting for any threshold crossover.
 */
double calc_capital_gain_max_from_room(double room, double current_gains);
