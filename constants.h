/*
 * constants.h — Tax rules and regulatory constants.
 *
 * These values reflect current Canadian legislation and CRA rules.
 * Update when the government changes rates, limits, or thresholds —
 * not for routine scenario changes.  For personal financial parameters
 * see scenario.h.
 */

#pragma once

/* -------------------------------------------------------------------------
 * Code infrastructure
 * ---------------------------------------------------------------------- */

#define MAX_ASSETS 20

/* Sentinel value for assets that have no end year. */
#define YEAR_NEVER 9999

/* Spouse array indices. */
#define RALPH_IDX 0
#define SARAH_IDX 1

/* -------------------------------------------------------------------------
 * TFSA and RRSP rules
 * ---------------------------------------------------------------------- */

/* Annual TFSA contribution-room increment.
 * This is the CURRENT_YEAR base; the simulation indexes it to inflation each
 * year so the limit grows in real terms at the same rate as other indexed
 * amounts (matching CRA's annual CPI-indexing, rounded to the nearest $500
 * in practice but applied continuously here for projection accuracy). */
#define TFSA_ANNUAL_ROOM_BASE 7000

#define RRSP_ROOM_RATE 0.18     /* CRA: 18% of prior-year net earned income */
#define RRSP_ANNUAL_LIMIT 33302 /* 2026 estimate: $32,490 × 1.025 (CRA uses AIW; verify when published) */

/* -------------------------------------------------------------------------
 * LIF / DCPP rules
 * ---------------------------------------------------------------------- */

#define LIF_MIX_AGE 55
#define PENSION_SPLIT_AGE 65

/* -------------------------------------------------------------------------
 * Capital gains inclusion rates (two-tier, effective June 25 2024)
 * ---------------------------------------------------------------------- */

/*
 * Individuals receive a $250,000 annual threshold below which only 50% of
 * capital gains are included in taxable income.  Gains above that threshold
 * are included at two-thirds (≈ 66.67%).  The threshold is not indexed to
 * inflation.
 *
 *   First $250,000 of net capital gains / year:  50.00% included
 *   Above $250,000 / year:                       66.67% included
 *
 * Use calc_capital_gain_inclusion(gain) for the forward (gain → income)
 * direction and calc_capital_gain_max_from_room(room, current_gains) for
 * the inverse (income room → max raw gain) used during harvesting.
 */
#define CAPITAL_GAIN_INCLUSION_RATE_LOW 0.5000       /* ≤ threshold */
#define CAPITAL_GAIN_INCLUSION_RATE_HIGH (2.0 / 3.0) /* > threshold */
#define CAPITAL_GAIN_ANNUAL_THRESHOLD 250000.0       /* per individual, per year; not indexed */

/* -------------------------------------------------------------------------
 * Basic Personal Amount (BPA) non-refundable tax credits
 * ---------------------------------------------------------------------- */

/*
 * Each level of government grants a credit equal to (BPA × lowest bracket rate).
 * Both amounts are indexed to inflation annually from CURRENT_YEAR.
 *
 * Federal  BPA 2026: $16,452  @ 15.00%  →  ~$2,468 credit / person
 * Ontario  BPA 2026: ~$12,399 @  5.05%  →  ~$  626 credit / person
 *                                            ──────────────────────
 *                                Total:      ~$3,094 credit / person
 *
 * Omitting the Ontario credit overstates taxes by ~$626/person/year
 * (~$1,252/year for both spouses, ~$54k undiscounted over the simulation).
 */
#define FEDERAL_PERSONAL_AMOUNT 16452 /* 2026 federal BPA                      */
#define FEDERAL_BPA_RATE 0.1500       /* Lowest federal bracket rate           */
#define ONTARIO_PERSONAL_AMOUNT 12399 /* 2026 Ontario BPA (estimated)          */
#define ONTARIO_BPA_RATE 0.0505       /* Lowest Ontario bracket rate           */

/* -------------------------------------------------------------------------
 * Pension Income Amount credit (federal + Ontario provincial)
 * ---------------------------------------------------------------------- */

/*
 * Up to $2,000 of eligible pension income (RRSP/RRIF and LIF withdrawals
 * at age 65+) qualifies for a non-refundable credit.
 *   Federal rate:  15.00%  ->  max credit $300
 *   Ontario rate:   5.05%  ->  max credit $101
 *   Combined:      20.05%  ->  max credit $401
 */
#define PENSION_INCOME_CREDIT_AGE 65
#define PENSION_INCOME_CREDIT_CEILING 2000.0
#define PENSION_INCOME_CREDIT_RATE 0.2005 /* Federal 15% + Ontario 5.05% */

/* -------------------------------------------------------------------------
 * Age Amount credit (federal + Ontario provincial)
 * ---------------------------------------------------------------------- */

/*
 * Available at age 65+. The base amount is reduced by 15 cents for every
 * dollar of net income above the phase-out threshold, and disappears
 * entirely once income exceeds the ceiling.
 *
 * All figures are 2026 values (2024 CRA base × 1.025²), matching
 * CURRENT_YEAR. The simulation indexes them forward from here.
 *
 *   Federal base:         $9,235  at 15.00%  ->  max credit ~$1,385
 *   Ontario base:         $6,197  at  5.05%  ->  max credit ~$  313
 *   Combined max credit:                              ~$1,698 / year
 *
 * Both the base amounts and the phase-out threshold are indexed to
 * inflation annually, matching the pattern used for tax brackets.
 */
#define AGE_AMOUNT_CREDIT_AGE 65

#define AGE_AMOUNT_FEDERAL_BASE 9235.0        /* 2026: $8,790 × 1.025²        */
#define AGE_AMOUNT_FEDERAL_RATE 0.1500

#define AGE_AMOUNT_ONTARIO_BASE 6197.0        /* 2026: $5,898 × 1.025²        */
#define AGE_AMOUNT_ONTARIO_RATE 0.0505

#define AGE_AMOUNT_PHASEOUT_THRESHOLD 44479.0 /* 2026: $42,335 × 1.025²       */
#define AGE_AMOUNT_PHASEOUT_RATE 0.1500       /* 15 cents per dollar over threshold */

/* -------------------------------------------------------------------------
 * OAS clawback (Recovery Tax) and age-75 supplement
 * ---------------------------------------------------------------------- */

/*
 * When net income exceeds the threshold, OAS benefits are reduced by 15 cents
 * per dollar of excess income.  The clawback is capped at the OAS amount
 * actually received — it cannot exceed what was paid out.
 * Both the threshold and the OAS amount are indexed to inflation annually.
 *
 *   2026 clawback threshold:  $95,323  (CRA published; 2025 base $93,454 × 1.020)
 *   Clawback rate:             15% of income above threshold
 */
#define OAS_CLAWBACK_THRESHOLD 95323.0 /* 2026 CRA published figure; indexed annually at INFLATION */
#define OAS_CLAWBACK_RATE 0.1500

/*
 * At age 75, OAS automatically increases by OAS_AGE75_SUPPLEMENT (10%),
 * a permanent enhancement introduced in July 2022.
 */
#define OAS_AGE75_SUPPLEMENT 0.10 /* Automatic 10 % step-up at age 75 */

/* -------------------------------------------------------------------------
 * CPP survivor benefit rules
 * ---------------------------------------------------------------------- */

/*
 * When --survivor <name> <age> is passed, the named spouse dies at the given
 * age.  Their assets roll over to the surviving spouse under the standard
 * Canadian spousal rollover rules (no tax at death on registered accounts,
 * TFSA, or principal residence; ACB carries over on non-reg and rental property).
 *
 * CPP survivor benefit: the surviving spouse receives 60% of the deceased's
 * CPP pension, capped so the combined (own CPP + survivor benefit) does not
 * exceed CPP_SURVIVOR_MAX_COMBINED_ANNUAL in CURRENT_YEAR dollars (both
 * amounts are indexed to inflation at the same rate, so the real-dollar cap
 * stays constant).
 *
 * Estimated 2026 cap: max CPP retirement pension at 65 (~$1,364/month) ×
 * 1.42 deferral factor (0.7%/month × 60 months) × 12 = ~$23,252/year.
 * For Ralph and Sarah this limits the survivor benefit to roughly
 * $1,400–$2,200/year depending on who survives.
 *
 * SURVIVOR_EXPENSE_FACTOR: household spending as a fraction of the previous
 * joint spending level.  70% is a common actuarial rule of thumb.
 */
#define CPP_SURVIVOR_RATE 0.60
#define CPP_SURVIVOR_MAX_COMBINED_ANNUAL 23256 /* 2026 estimate; indexed at INFLATION */
#define SURVIVOR_EXPENSE_FACTOR 0.70

/* -------------------------------------------------------------------------
 * Miscellaneous thresholds
 * ---------------------------------------------------------------------- */

/* Lowest combined federal+Ontario marginal bracket — used as the default
 * RRSP withdrawal target so draws stay in the bottom tax band. */
#define LOWEST_TAX_THRESHOLD  53891
