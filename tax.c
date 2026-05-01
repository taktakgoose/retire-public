/*
 * tax.c - Tax bracket calculations for the retirement simulation.
 */

#include "tax.h"
#include "params.h"

/* =========================================================================
 * TAX BRACKETS
 * ====================================================================== */

typedef struct {
    double threshold;
    double rate;
} tax_bracket_t;

/*
 * Combined federal + Ontario marginal tax brackets.
 * Thresholds are inflated each year by increment_tax_brackets() and reset
 * to base values at the start of each simulation trial by reset_tax_brackets().
 */
static tax_bracket_t brackets[] = {
    { .threshold = 0,           .rate = 0.1905 },
    { .threshold = 53891,       .rate = 0.2315 },
    { .threshold = 58523,       .rate = 0.2965 },
    { .threshold = 94907,       .rate = 0.3148 },
    { .threshold = 107785,      .rate = 0.3389 },
    { .threshold = 111814,      .rate = 0.3791 },
    { .threshold = 117045,      .rate = 0.4341 },
    { .threshold = 150000,      .rate = 0.4497 },
    { .threshold = 181440,      .rate = 0.4826 },
    { .threshold = 220000,      .rate = 0.4982 },
    { .threshold = 258482,      .rate = 0.5353 },
    { .threshold = 999999999,   .rate = 0.5353 },
};
/* Each OpenMP thread runs an independent simulation that inflates brackets[]
 * via increment_tax_brackets().  threadprivate gives every thread its own
 * copy so those mutations never race with other threads. */
#pragma omp threadprivate(brackets)

/*
 * Snapshot of the base-year (CURRENT_YEAR) bracket thresholds.
 * Used by reset_tax_brackets() to restore state between simulation trials.
 */
static const tax_bracket_t brackets_base[] = {
    { .threshold = 0,           .rate = 0.1905 },
    { .threshold = 53891,       .rate = 0.2315 },
    { .threshold = 58523,       .rate = 0.2965 },
    { .threshold = 94907,       .rate = 0.3148 },
    { .threshold = 107785,      .rate = 0.3389 },
    { .threshold = 111814,      .rate = 0.3791 },
    { .threshold = 117045,      .rate = 0.4341 },
    { .threshold = 150000,      .rate = 0.4497 },
    { .threshold = 181440,      .rate = 0.4826 },
    { .threshold = 220000,      .rate = 0.4982 },
    { .threshold = 258482,      .rate = 0.5353 },
    { .threshold = 999999999,   .rate = 0.5353 },
};

static int num_brackets = (int)(sizeof(brackets) / sizeof(brackets[0]));

/*
 * Per-year compound inflation factor, indexed by (calendar_year - CURRENT_YEAR).
 * inflation_factor[k] = (1 + INFLATION)^k.
 * Populated once by init_tax_tables() before the simulation loop, avoiding
 * repeated inner loops inside the three inflation-dependent tax functions.
 */
static double inflation_factor[SIMULATED_YEARS + 2];

void init_tax_tables(void)
{
    double f = 1.0;
    for (int k = 0; k < SIMULATED_YEARS + 2; k++) {
        inflation_factor[k] = f;
        f *= (1.0 + INFLATION);
    }
}

/* Inflate all bracket thresholds by one year's inflation. */
void increment_tax_brackets(void)
{
    for (int i = 0; i < num_brackets; i++) {
        brackets[i].threshold += brackets[i].threshold * INFLATION;
    }
}

/* Restore bracket thresholds to their CURRENT_YEAR base values. */
void reset_tax_brackets(void)
{
    for (int i = 0; i < num_brackets; i++) {
        brackets[i].threshold = brackets_base[i].threshold;
    }
}

/* Return total taxes owed on the given income using the current brackets. */
double taxes_owing_on_income(double income)
{
    if (income <= 0.0) return 0.0;   /* capital losses can produce negative income; no tax owing */

    int i = 0;

    for (i = 0; i < num_brackets; i++) {
        if (income < brackets[i].threshold) break;
    }

    if (i >= num_brackets) {
        i = num_brackets - 1;
    }

    double taxes = (income - brackets[i - 1].threshold) * brackets[i - 1].rate;

    for (int j = 0; j < (i - 1); j++) {
        taxes += (brackets[j + 1].threshold - brackets[j].threshold) * brackets[j].rate;
    }

    return taxes;
}

/*
 * Return the combined federal + Ontario basic personal amount (BPA) tax
 * credits for the given calendar year and income level.
 *
 * Both BPAs are non-refundable and inflation-indexed.  Each credit equals
 * (BPA × lowest bracket rate).  When income is below a BPA the credit is
 * prorated to income so taxes never go negative from this credit alone.
 *
 *   Federal credit:  FEDERAL_PERSONAL_AMOUNT  × FEDERAL_BPA_RATE  (~$2,468 in 2026)
 *   Ontario credit:  ONTARIO_PERSONAL_AMOUNT  × ONTARIO_BPA_RATE  (~$  626 in 2026)
 *   Combined:                                                        ~$3,094 in 2026
 */
double calc_personal_exemption(int cy, double income)
{
    double factor    = inflation_factor[cy - CURRENT_YEAR];

    double fed_bpa   = FEDERAL_PERSONAL_AMOUNT * factor;
    if (income < fed_bpa) fed_bpa = income;
    double fed_credit = fed_bpa * FEDERAL_BPA_RATE;

    double ont_bpa   = ONTARIO_PERSONAL_AMOUNT * factor;
    if (income < ont_bpa) ont_bpa = income;
    double ont_credit = ont_bpa * ONTARIO_BPA_RATE;

    return fed_credit + ont_credit;
}

/*
 * Return the combined federal + Ontario Age Amount non-refundable tax credit
 * for a spouse of the given age with the given net income in calendar year cy.
 * Returns 0 if the spouse is under 65 or the credit is fully phased out.
 */
double calc_age_amount_credit(int age, double net_income, int cy)
{
    if (age < AGE_AMOUNT_CREDIT_AGE) return 0.0;

    /* Scale the base amounts and phase-out threshold to the given year. */
    double factor       = inflation_factor[cy - CURRENT_YEAR];
    double federal_base = AGE_AMOUNT_FEDERAL_BASE * factor;
    double ontario_base = AGE_AMOUNT_ONTARIO_BASE * factor;
    double threshold    = AGE_AMOUNT_PHASEOUT_THRESHOLD * factor;

    /* Reduce the base amounts by 15% of income exceeding the threshold. */
    if (net_income > threshold) {
        double reduction = (net_income - threshold) * AGE_AMOUNT_PHASEOUT_RATE;
        federal_base -= reduction;
        ontario_base -= reduction;
        if (federal_base < 0.0) federal_base = 0.0;
        if (ontario_base < 0.0) ontario_base = 0.0;
    }

    return (federal_base * AGE_AMOUNT_FEDERAL_RATE)
         + (ontario_base * AGE_AMOUNT_ONTARIO_RATE);
}

/*
 * Return the remaining income capacity in the first combined bracket for
 * the given calendar year.
 *
 * The first bracket runs from $0 to brackets[1].threshold (e.g. ~$53,891
 * in 2026, inflated each year).  Any RRSP/RRIF income drawn within this
 * band is taxed at the lowest combined federal+Ontario marginal rate
 * (~19.05%).  Filling this capacity now is preferable to leaving assets
 * in registered accounts where they will eventually be drawn at higher
 * marginal rates once RRIF minimums, CPP, and OAS stack up.
 *
 * During working years, salary alone typically exceeds brackets[1].threshold,
 * so the function returns 0 and has no effect.  After retirement, when
 * income is modest, it quantifies how much extra RRSP income can be taken
 * at the lowest available rate.
 */
double calc_first_bracket_room(double current_income)
{
    double room = brackets[1].threshold - current_income;
    return (room > 0.0) ? room : 0.0;
}

/*
 * Return the OAS Recovery Tax (clawback) for a spouse with the given net
 * income and OAS amount received in calendar year cy.
 * The clawback threshold is indexed to inflation from CURRENT_YEAR.
 * The result is capped at oas_received.
 */
double calc_oas_clawback(double net_income, double oas_received, int cy)
{
    if (oas_received <= 0.0) return 0.0;

    double threshold = OAS_CLAWBACK_THRESHOLD * inflation_factor[cy - CURRENT_YEAR];

    if (net_income <= threshold) return 0.0;

    double clawback = (net_income - threshold) * OAS_CLAWBACK_RATE;
    if (clawback > oas_received) clawback = oas_received;
    return clawback;
}

/*
 * Return the inflation-indexed OAS clawback threshold for calendar year cy.
 * Mirrors the threshold computation inside calc_oas_clawback() so callers
 * can estimate clawback risk before committing to a withdrawal.
 */
double get_oas_clawback_threshold(int cy)
{
    return OAS_CLAWBACK_THRESHOLD * inflation_factor[cy - CURRENT_YEAR];
}

/*
 * Public accessor for the pre-computed cumulative inflation factor.
 * Lets callers in other translation units (retire.c, output.c, ...)
 * scale CURRENT_YEAR-dollar amounts into year-cy nominal dollars
 * without paying for pow() on every call.
 */
double get_inflation_factor(int cy)
{
    return inflation_factor[cy - CURRENT_YEAR];
}

/*
 * Return the current (already inflation-adjusted) income ceiling up to which
 * capital-gain harvesting is attractive.  This is the top of the second
 * combined federal+Ontario bracket ($58,523 in 2026).  Below this line the
 * effective rate on capital gains is at most ~11.6%; above it the combined
 * rate jumps roughly 6 percentage points to 29.65%.
 *
 * brackets[] is threadprivate and is already inflated for the current
 * simulation year, so no additional indexing is required.
 */
double get_harvest_income_ceiling(void)
{
    /* brackets[2].threshold is the start of the third combined bracket —
     * the ceiling we want to stay below when harvesting gains. */
    return brackets[2].threshold;
}

/*
 * Forward: raw capital gain → included (taxable) income.
 *
 * Two-tier rule effective June 25 2024:
 *   ≤ $250,000 / individual / year  →  50.00% included
 *   > $250,000                      →  66.67% included on the excess
 *
 * The $250,000 annual threshold is not indexed to inflation.
 */
double calc_capital_gain_inclusion(double gain)
{
    if (gain <= CAPITAL_GAIN_ANNUAL_THRESHOLD)
        return gain * CAPITAL_GAIN_INCLUSION_RATE_LOW;

    return CAPITAL_GAIN_ANNUAL_THRESHOLD * CAPITAL_GAIN_INCLUSION_RATE_LOW
         + (gain - CAPITAL_GAIN_ANNUAL_THRESHOLD) * CAPITAL_GAIN_INCLUSION_RATE_HIGH;
}

/*
 * Inverse: income room → maximum additional raw capital gain.
 *
 * Given a budget of included income (room) still available before hitting
 * a ceiling, and the capital gains already realized by this individual this
 * year (current_gains), return the largest additional raw gain that fits.
 *
 * Handles three cases:
 *   1. current_gains already above threshold — all new gains at RATE_HIGH.
 *   2. All additional gain fits within the remaining low-rate room.
 *   3. Gain spills across the threshold boundary into the high-rate zone.
 */
double calc_capital_gain_max_from_room(double room, double current_gains)
{
    double remaining_low = CAPITAL_GAIN_ANNUAL_THRESHOLD - current_gains;

    if (remaining_low <= 0.0) {
        /* Already above threshold — all new gains taxed at the high rate. */
        return room / CAPITAL_GAIN_INCLUSION_RATE_HIGH;
    }

    /* Try to fit entirely within the remaining low-rate zone. */
    double max_in_low = room / CAPITAL_GAIN_INCLUSION_RATE_LOW;
    if (max_in_low <= remaining_low)
        return max_in_low;

    /* Gain spills into the high-rate zone.
     * Solve: room = remaining_low × RATE_LOW + (gain − remaining_low) × RATE_HIGH */
    double low_income = remaining_low * CAPITAL_GAIN_INCLUSION_RATE_LOW;
    return remaining_low + (room - low_income) / CAPITAL_GAIN_INCLUSION_RATE_HIGH;
}
