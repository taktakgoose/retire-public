/*
 * params.h - Simulation parameters.
 *
 * Edit these constants to model different scenarios, then recompile.
 */

#pragma once

/* -------------------------------------------------------------------------
 * Starting balances
 * ---------------------------------------------------------------------- */

/* Assets */

#define GENERICBANK_TFSA_RALPH 10000
#define GENERICBANK_RRSP_RALPH 600000
#define GENERICBANK_LIRA_RALPH 10000
#define GENERICBANK_RRSP_SARAH 400000
#define GENERICBANK_LIRA_SARAH 40000
#define GENERICBANK_TFSA_SARAH 50000
#define GENERICBANK_CHEQUING_ACCT 1000

#define WORKPLACE_AFTER_TAX_STOCK 50000
#define WORKPLACE_BONUS_SARAH 0
#define CRA_REFUND 0

#define SUNLIFE_DCPP_RALPH 200000
#define SUNLIFE_DCPP_SARAH 200000
#define SUNLIFE_RRSP_SARAH 5000

#define GENERICBROKER_RRSP_RALPH 10000
#define GENERICBROKER_TFSA_RALPH 80000
#define GENERICBROKER_NONREG_RALPH 2000
#define GENERICBROKER_CRYPTO_RALPH 4000
#define GENERICBROKER_TFSA_SARAH 30000

/* Liabilities */

#define AMEX_BALANCE -1000
#define VISA_BALANCE -1000
#define MASTERCARD_BALANCE -200
#define DISCOVERY_BALANCE -200

#define ROBERTSON_MORTGAGE_BALANCE 20000
#define MOONBROOK_MORTGAGE_BALANCE 80000

/* Totals */

#define STARTING_CASH (GENERICBANK_CHEQUING_ACCT + WORKPLACE_AFTER_TAX_STOCK + CRA_REFUND + AMEX_BALANCE + VISA_BALANCE + MASTERCARD_BALANCE + DISCOVERY_BALANCE)
#define STARTING_RALPH_DCPP (GENERICBANK_LIRA_RALPH + SUNLIFE_DCPP_RALPH)
#define STARTING_SARAH_DCPP (GENERICBANK_LIRA_SARAH + SUNLIFE_DCPP_SARAH)
#define STARTING_RALPH_RRSP (GENERICBANK_RRSP_RALPH + GENERICBROKER_RRSP_RALPH)
#define STARTING_SARAH_RRSP (GENERICBANK_RRSP_SARAH + SUNLIFE_RRSP_SARAH)
#define STARTING_RALPH_TFSA (GENERICBANK_TFSA_RALPH + GENERICBROKER_TFSA_RALPH)
#define STARTING_SARAH_TFSA (GENERICBANK_TFSA_SARAH + GENERICBROKER_TFSA_SARAH)
#define STARTING_RALPH_NONREG (GENERICBROKER_NONREG_RALPH + GENERICBROKER_CRYPTO_RALPH)

/* Real estate */

#define MOONBROOK_CURRENT_VALUE 500000
#define MOONBROOK_ANNUAL_GROWTH 0.0500 /* Annual property value appreciation rate */
#define ROBERTSON_CURRENT_VALUE 700000
#define ROBERTSON_ANNUAL_GROWTH 0.0300 /* Annual property value appreciation rate */

/* Contribution Room */

#define STARTING_RALPH_TFSA_ROOM 30000
#define STARTING_SARAH_TFSA_ROOM 30000

/* Annual TFSA contribution-room increment.
 * This is the CURRENT_YEAR base; the simulation indexes it to inflation each
 * year so the limit grows in real terms at the same rate as other indexed
 * amounts (matching CRA's annual CPI-indexing, rounded to the nearest $500
 * in practice but applied continuously here for projection accuracy). */
#define TFSA_ANNUAL_ROOM_BASE 7000

#define STARTING_RALPH_RRSP_ROOM 10000 /* Available RRSP contribution room (from CRA NOA) */
#define STARTING_SARAH_RRSP_ROOM 10000

/* -------------------------------------------------------------------------
 * Simulation time-span and retirement dates
 * ---------------------------------------------------------------------- */

#define CURRENT_YEAR 2026
#define DEATH_YEAR 2069

/*
 * Per-spouse retirement years.  Set to different values to model staggered
 * retirements (e.g. one spouse works an extra year or two).
 */
#define RALPH_RETIREMENT_YEAR 2030
#define SARAH_RETIREMENT_YEAR 2030

/*
 * Month (1–12) in which each spouse retires in their retirement year.
 * They work through the month before their RETIREMENT_MONTH
 * (e.g. month 8 means last pay is end-of-July).
 * CURRENT_MONTH is the calendar month in which the simulation is being run;
 * starting cash already reflects salary received and expenses paid through
 * the end of CURRENT_MONTH - 1.
 */
#define CURRENT_MONTH 5          /* May 2026 — analysis date                      */
#define RALPH_RETIREMENT_MONTH 11 /* Last pay cheque is October */
#define SARAH_RETIREMENT_MONTH 11  /* Last pay cheque is October */

/*
 * Months remaining in the start year (CURRENT_MONTH through December,
 * inclusive).  April → December = 9 months.
 * PARTIAL_YEAR_FRACTION scales annual amounts (expenses, growth, income)
 * down to cover only the simulated portion of the first calendar year.
 */
#define PARTIAL_YEAR_MONTHS (13 - CURRENT_MONTH)
#define PARTIAL_YEAR_FRACTION (PARTIAL_YEAR_MONTHS / 12.0)

/*
 * Months each spouse will still work in their retirement year.
 * If retiring in CURRENT_YEAR: count from CURRENT_MONTH to RETIREMENT_MONTH.
 * If retiring in a future year: count from January (month 1) to RETIREMENT_MONTH.
 */
#define RALPH_MONTHS_TO_WORK (RALPH_RETIREMENT_YEAR == CURRENT_YEAR          \
                                  ? (RALPH_RETIREMENT_MONTH - CURRENT_MONTH) \
                                  : (RALPH_RETIREMENT_MONTH - 1))
#define SARAH_MONTHS_TO_WORK (SARAH_RETIREMENT_YEAR == CURRENT_YEAR          \
                                 ? (SARAH_RETIREMENT_MONTH - CURRENT_MONTH) \
                                 : (SARAH_RETIREMENT_MONTH - 1))

#define SIMULATED_YEARS (DEATH_YEAR - CURRENT_YEAR + 2)

/* Sentinel value for assets that have no end year. */
#define YEAR_NEVER 9999

/*
 * Number of Monte Carlo trials in the optimisation search.
 *
 * Wrapped in #ifndef so Makefile profile targets (dev / plan / hq) can
 * override this from the compile command line via -DN_TRIALS=....
 */
#ifndef N_TRIALS
#define N_TRIALS 10000
#endif
#define N_TRIALS_PRINT_PROG (N_TRIALS / 1000)

/*
 * Maximum OpenMP threads.  Limiting this below the physical core count
 * reduces heat while still completing in reasonable time.
 * Set to 0 to let OpenMP use all available cores.
 */
#define MAX_OMP_THREADS 4

/*
 * Number of independent market paths used during the OPTIMISATION search —
 * i.e. how many paths each candidate (ralph_adj, sarah_adj) is averaged across
 * when picking the apparent winner for a given moonbrook-sale year.  Lower values
 * speed up the 10k-trial search but increase "optimization-by-luck" risk;
 * higher values slow the search but make the chosen adjustments more robust.
 *
 * See also EVALUATION_PATHS_PER_STRATEGY below, which controls the separate
 * honest-reporting pass that re-tests the winner on a wider panel.
 */
#ifndef RETURN_PATHS_PER_STRATEGY
#define RETURN_PATHS_PER_STRATEGY 4
#endif

/*
 * Number of independent market paths used during the EVALUATION re-test.
 *
 * After the optimisation loop selects the best (ralph_adj, sarah_adj) for a
 * given moonbrook-sale year, that single candidate is re-run against this many
 * fresh market paths to produce an honest mean terminal net worth and
 * success rate.  Because only one candidate is evaluated per fsy (vs N_TRIALS
 * candidates during the search), this knob is nearly free — the total
 * evaluation cost is CCA_passes × moonbrook-year-range × EVALUATION_PATHS, which
 * is dwarfed by the optimisation work.  Crank it up without worry.
 *
 * This separates "we searched cheaply" from "we report honestly": the CSV
 * representative path, the mean NW in the summary, and the cross-fsy
 * ranking all come from this pass.
 */
#ifndef EVALUATION_PATHS_PER_STRATEGY
#define EVALUATION_PATHS_PER_STRATEGY 64
#endif

/* -------------------------------------------------------------------------
 * Salaries and growth rates
 * ---------------------------------------------------------------------- */

#define RALPH_SALARY 100000
#define SARAH_SALARY 110000

/*
 * Employer DCPP matching / contribution rate.
 * Applied to each spouse's salary while they are still working.
 * In the retirement year the contribution is automatically prorated because
 * the spouse's salary field is already set to the remaining-year salary (rys).
 * Set to 0.0 to disable employer contributions entirely.
 */
#define DCPP_EMPLOYER_CONTRIBUTION_RATE  0.04

#define INFLATION 0.0250
#define RALPH_GROWTH_RATE 0.0500
#define SARAH_GROWTH_RATE 0.0500

/*
 * Monte Carlo return-path volatility.
 *
 * The simulation now varies annual market returns per trial instead of
 * holding all asset growth rates fixed.  percent_growth remains the expected
 * long-run return / appreciation rate for each asset, and these volatility
 * values control the year-to-year standard deviation around that mean.
 *
 * Set any volatility to 0.0 to make that asset class deterministic again.
 */
#define FINANCIAL_RETURN_VOLATILITY 0.0900 /* RRSP, LIF/DCPP, TFSA, non-reg */
#define PROPERTY_RETURN_VOLATILITY  0.0800 /* Robertson / Moonbrook appreciation   */
#define RENT_RETURN_VOLATILITY      0.0500 /* Moonbrook net-rent growth          */

/* -------------------------------------------------------------------------
 * Real-estate
 * ---------------------------------------------------------------------- */

#define ROBERTSON_SALE_YEAR 2066
/*
 * MOONBROOK_SALE_YEAR is kept as the template default and as the detection
 * sentinel in compute_asset_indices().  At runtime the simulation sweeps
 * every year in [MOONBROOK_SALE_YEAR_MIN, MOONBROOK_SALE_YEAR_MAX] and reports
 * which sale year maximises net worth.
 */
#define MOONBROOK_SALE_YEAR 2035     /* template default / detection sentinel */
/* Guarded so Makefile profiles can widen / narrow the search range. */
#ifndef MOONBROOK_SALE_YEAR_MIN
#define MOONBROOK_SALE_YEAR_MIN 2030 /* earliest year to consider selling     */
#endif
#ifndef MOONBROOK_SALE_YEAR_MAX
#define MOONBROOK_SALE_YEAR_MAX 2038 /* latest year to consider selling       */
#endif

/*
 * Average net rental income for the year. It is the rent minus the operating
 * expenses and mortgage interest. Note: the principle portion of any mortgage
 * payments is not a part of this calculation since it's going towards the
 * value of the property.
 */
/*
 * Fraction of Robertson Court mortgage interest that is attributable to the
 * Moonbrook Street rental property (the LOC drawn against the primary residence
 * was used to help finance the Moonbrook purchase).  This portion appears on the
 * Moonbrook balance sheet and should feed into the rent-profit interest-saving
 * adjustment as the Robertson mortgage is paid down.
 */
#define MOONBROOK_ANNUAL_RENT_PROFIT 5010
#define MOONBROOK_LOC_ROBERTSON_FRACTION 0.375
/*
 * Net annual growth rate for Moonbrook rent profit.  Rental income grows roughly
 * with inflation, but operating expenses (condo fees, insurance, property
 * taxes, etc.) grow at a similar pace, so the net profit growth rate is
 * lower — and can be negative if expenses outpace rent increases.
 * Mortgage interest savings are tracked separately in grow_assets() and added
 * on top of this baseline rate each year.
 */
#define MOONBROOK_RENT_NET_GROWTH -0.0050

/*
 * Mortgage details for Moonbrook Street (rental property).
 * Set MOONBROOK_MORTGAGE_BALANCE to 0 if the property is mortgage-free.
 * MOONBROOK_MORTGAGE_BALANCE         – current outstanding principal (total, split 50/50 per spouse)
 * MOONBROOK_MORTGAGE_MONTHLY_PAYMENT – fixed monthly payment (total, split 50/50 per spouse)
 * MOONBROOK_MORTGAGE_INTEREST_RATE   – annual interest rate (e.g. 0.0550 for 5.50%)
 */
#define MOONBROOK_MORTGAGE_MONTHLY_PAYMENT 1501
#define MOONBROOK_MORTGAGE_INTEREST_RATE 0.0360

/*
 * Mortgage details for Robertson Court (principal residence).
 * Set ROBERTSON_MORTGAGE_BALANCE to 0 if the property is mortgage-free.
 * ROBERTSON_MORTGAGE_BALANCE         – current outstanding principal (total, split 50/50 per spouse)
 * ROBERTSON_MORTGAGE_MONTHLY_PAYMENT – fixed monthly payment (total, split 50/50 per spouse)
 * ROBERTSON_MORTGAGE_INTEREST_RATE   – annual interest rate (e.g. 0.0550 for 5.50%)
 */
#define ROBERTSON_MORTGAGE_MONTHLY_PAYMENT 514
#define ROBERTSON_MORTGAGE_INTEREST_RATE 0.0380

/* -------------------------------------------------------------------------
 * Withdrawal / cash preferences
 * ---------------------------------------------------------------------- */

#define PREFERRED_CASH 2000
#define NONREG_CASH_THRESHOLD 200000 /* invest cash above this into non-reg */
#define RRSP_EXTRA_DEBIT 1.3000
#define NONREG_EXTRA_DEBIT 1.1000

/* -------------------------------------------------------------------------
 * Spending phases (lifestyle transitions)
 * ---------------------------------------------------------------------- */

#define KIDS_HOME_SPENDING 110000
#define GO_GO_SPENDING 100000
#define SLOW_GO_SPENDING 90000
#define NO_GO_SPENDING 80000

#define CURRENT_SPENDING KIDS_HOME_SPENDING

#define GO_GO_YEAR 2032
#define SLOW_GO_YEAR 2050
#define NO_GO_YEAR 2060

/*
 * Senior living / rental cost that begins once Robertson Court is sold.
 * Covers rent, condo fees, or assisted-living charges.
 * Expressed in CURRENT_YEAR dollars; inflated annually at INFLATION.
 */
#define SENIOR_LIVING_MONTHLY_RENT 4000
#define SENIOR_LIVING_ANNUAL_RENT (SENIOR_LIVING_MONTHLY_RENT * 12)

/* -------------------------------------------------------------------------
 * Miscellaneous constants
 * ---------------------------------------------------------------------- */

#define MAX_ASSETS 20

#define PENSION_SPLIT_AGE 65

#define RRSP_ROOM_RATE 0.18     /* CRA: 18% of prior-year net earned income */
#define RRSP_ANNUAL_LIMIT 33302 /* 2026 estimate: $32,490 × 1.025 (CRA uses AIW; verify when published) */

#define LIF_MIX_AGE 55

/*
 * Capital gains inclusion rates (two-tier, effective June 25 2024).
 *
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

/*
 * Basic Personal Amount (BPA) non-refundable tax credits.
 *
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

/*
 * Surviving-spouse scenario parameters.
 *
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

/*
 * Sequence-of-Returns (SOR) stress-test returns.
 * Applied to financial investment assets only (RRSP, LIF/DCPP, TFSA, non-reg)
 * for the two years immediately following the first spouse's retirement year.
 * Real estate and income-flow assets (OAS, CPP, rent) are unaffected.
 *
 * Enable with:  ./retire --sor
 */
#define SOR_YEAR_1_RETURN (-0.10) /* −10 % in year 1 after first retirement */
#define SOR_YEAR_2_RETURN (-0.05) /* − 5 % in year 2 after first retirement */

/* Return-neutral compensation for SOR mode.
 *
 * The two stress years inject returns below the expected baseline
 * (percent_growth), creating a total drag of:
 *   (RALPH_GROWTH_RATE − SOR_YEAR_1_RETURN) + (RALPH_GROWTH_RATE − SOR_YEAR_2_RETURN)
 *   = (0.05 − (−0.10)) + (0.05 − (−0.05))  =  0.15 + 0.10  =  0.25
 *
 * Spreading that 0.25 across the remaining (SIMULATED_YEARS − 2) investment
 * years keeps the long-run average return equal to the un-stressed baseline,
 * so --sor tests pure sequence-of-returns risk rather than also being a
 * lower-total-return scenario.
 *
 * ≈ 0.25 / 43  ≈  +0.58 % per non-SOR investment year.
 */
#define SOR_ANNUAL_COMPENSATION \
    (((RALPH_GROWTH_RATE - SOR_YEAR_1_RETURN) + \
      (RALPH_GROWTH_RATE - SOR_YEAR_2_RETURN)) \
     / (double)(SIMULATED_YEARS - 2))

/*
 * RRIF cash-bucket strategy parameters.
 *
 * When --bucket is enabled, a portion of each spouse's RRIF (modelled as a
 * subset of ASSET_RRSP.value — a RRIF is just an RRSP in withdrawal mode) is
 * earmarked as a conservative "cash bucket" equal to RRIF_BUCKET_YEARS_TARGET
 * years of expected annual minimum withdrawals.  Mechanics per year:
 *
 *   Seeding:   On each spouse's first withdrawal year the bucket is
 *              initialised at the target size (accounting-only — total RRSP
 *              value is unchanged, the reclassification just earmarks a
 *              conservative allocation inside the RRIF).
 *   Growth:    The bucket portion earns RRIF_BUCKET_RETURN with σ =
 *              RRIF_BUCKET_VOLATILITY; the equity portion (value - bucket)
 *              continues to earn percent_growth with the financial shock.
 *   Withdraw:  Mandatory RRIF draws are taken from the bucket first; only
 *              if the bucket is exhausted does equity need to be sold.
 *   Refill:    At year end, top up the bucket to target ONLY if the equity
 *              shock was positive.  In a down year the bucket is allowed to
 *              run low while equity recovers — this is the Kitces rebalancing
 *              discipline that insulates forced sales from market drawdowns.
 *
 * Tax treatment is unchanged: the entire RRSP value remains a RRIF, so
 * withdrawals are still fully taxable as pension income regardless of which
 * internal bucket they came from.
 *
 * Enable with:  ./retire --bucket
 */
#define RRIF_BUCKET_YEARS_TARGET 2.5    /* years of min-withdrawal coverage     */
#define RRIF_BUCKET_RETURN       0.0300 /* real return on bucket (HISA/GIC)     */
#define RRIF_BUCKET_VOLATILITY   0.0150 /* σ of bucket returns (small but > 0)  */

/* Spouse array indices. */
#define RALPH_IDX 0
#define SARAH_IDX 1

/*
 * Pension Income Amount credit (federal + Ontario provincial).
 * Up to $2,000 of eligible pension income (RRSP/RRIF and LIF withdrawals
 * at age 65+) qualifies for a non-refundable credit.
 *   Federal rate:  15.00%  ->  max credit $300
 *   Ontario rate:   5.05%  ->  max credit $101
 *   Combined:      20.05%  ->  max credit $401
 */
#define PENSION_INCOME_CREDIT_AGE 65
#define PENSION_INCOME_CREDIT_CEILING 2000.0
#define PENSION_INCOME_CREDIT_RATE 0.2005 /* Federal 15% + Ontario 5.05% */

/*
 * Age Amount credit (federal + Ontario provincial).
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

/*
 * OAS Recovery Tax (clawback).
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
/* Buffer below the clawback threshold — stop RRIF draws that would
 * push net income into clawback territory by more than this margin. */
#define OAS_CLAWBACK_BUFFER  15000.0

/*
 * Half-range of the per-year random walk applied to the RRSP withdrawal
 * target in run_simulation().  Shared by both the default (walk from
 * previous year's target) and --oas-target (re-anchor to OAS/bottom-
 * bracket ceiling each year) modes so they explore the same ±-dollar
 * neighbourhood.
 *
 * EXPRESSED IN CURRENT_YEAR (real) DOLLARS.  At each simulated year the
 * constant is scaled by get_inflation_factor(cy) before being applied,
 * so the nominal walk grows alongside the indexed anchors (bottom-tax
 * threshold, OAS clawback threshold, expenses).  This keeps the MC
 * exploration width constant in PURCHASING POWER across the horizon
 * rather than shrinking in real terms — a fixed nominal ±$15k was only
 * ~$5.6k real by year 44 at 2.5% inflation.
 *
 * A value of 50000 means each year's target gets a perturbation in
 * [−$50,000, +$50,000] in CURRENT_YEAR dollars on top of its mode-
 * specific anchor (≈ [−$148k, +$148k] nominal at DEATH_YEAR under
 * 2.5% inflation).  Wider ranges let the MC search consider more
 * aggressive / conservative targets at the cost of less focused
 * optimisation; compensate with proportionally more N_TRIALS.
 */
#define RRSP_TARGET_WALK_RANGE 10000

/* -------------------------------------------------------------------------
 * CCA / Undepreciated Capital Cost
 * ---------------------------------------------------------------------- */

/*
 * Moonbrook Street rental property cost basis and CCA tracking.
 *
 * MOONBROOK_PURCHASE_COST — original purchase price (ACB for capital gains).
 * MOONBROOK_UCC           — undepreciated capital cost after CCA claims.
 * MOONBROOK_CCA_RATE      — Class 1 declining-balance rate (4 % for post-1987
 *                       residential rental buildings in Ontario).
 *
 * On sale, the spread (MOONBROOK_PURCHASE_COST − MOONBROOK_UCC) = $61,377 is fully
 * taxable as CCA recapture income.  Any proceeds above MOONBROOK_PURCHASE_COST
 * are a capital gain taxed at the inclusion rate.
 * Both figures are split 50/50 between spouses.
 *
 * When g_cca_enabled is true the simulation claims CCA annually, reducing
 * rental taxable income each year but increasing recapture at sale.
 */
#define MOONBROOK_PURCHASE_COST 250000
#define MOONBROOK_UCC 200000
#define MOONBROOK_CCA_RATE 0.04 /* Class 1, 4 % declining balance */

/* -------------------------------------------------------------------------
 * OAS benefits
 * ---------------------------------------------------------------------- */

/*
 * OAS base rate (ages 65-74) as of Q1 2026: $742.31/month = $8,908/year.
 * Deferred to age 70 (36% enhancement): $742.31 × 1.36 = $1,009.54/month
 * = $12,115/year.  Both figures grow with inflation (percent_growth = INFLATION).
 *
 * At age 75, OAS automatically increases by OAS_AGE75_SUPPLEMENT (10%),
 * a permanent enhancement introduced in July 2022.
 */
#define OAS_DEFERRED_ANNUAL 12115 /* Annual OAS at 70 in Q1-2026 dollars  */
#define OAS_START_AGE 70          /* Deferring to 70 for maximum benefit   */
#define OAS_AGE75_SUPPLEMENT 0.10 /* Automatic 10 % step-up at age 75      */

/* -------------------------------------------------------------------------
 * CPP benefits
 * ---------------------------------------------------------------------- */

#define RALPH_CPP_MONTHLY_PAYMENT 1600
#define RALPH_CPP_START_AGE 70 /* Age at which Ralph begins receiving CPP */
#define SARAH_CPP_MONTHLY_PAYMENT 1500
#define SARAH_CPP_START_AGE 70 /* Age at which Sarah begins receiving CPP  */

/* -------------------------------------------------------------------------
 * Miscellaneous thresholds
 * ---------------------------------------------------------------------- */

/* Lowest combined federal+Ontario marginal bracket — used as the default
 * RRSP withdrawal target so draws stay in the bottom tax band. */
#define LOWEST_TAX_THRESHOLD  53891

/* -------------------------------------------------------------------------
 * Percentile band pass
 * ---------------------------------------------------------------------- */

/* Number of independent market paths used to compute P10/P50/P90 net-worth
 * bands.  500 gives smooth curves; reduce for faster builds during dev. */
#ifndef N_PERCENTILE_PATHS
#define N_PERCENTILE_PATHS 500
#endif
