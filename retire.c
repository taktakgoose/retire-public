/*
 * retire.c - Retirement financial simulation.
 *
 * Simulates yearly cash-flow, asset growth, tax, and withdrawal strategy
 * for two spouses from the current year until a projected end-of-life year.
 * The main() loop runs Monte-Carlo trials varying RRSP withdrawal targets
 * and writes the highest-success scenario to "retirement.csv", breaking
 * ties by mean terminal net worth.
 *
 * Usage:
 *   ./retire        — run simulation, write retirement.csv
 *   ./retire -v     — same, but print per-year tax details to stdout
 */

#include "params.h"
#include "types.h"
#include "tax.h"
#include "tables.h"
#include "output.h"

#include <math.h>
#include <omp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Enable/disable verbose per-year printf output (-v flag). */
static bool prints_on = false;

/*
 * Seeded-mode state.
 *
 * When --rrsp <file> is passed, g_seed_mode is set to true and the per-year
 * RRSP withdrawal targets loaded from the file are stored here.  The Monte
 * Carlo loop then perturbs those values instead of doing a blind random walk
 * from the initial RRSP target.
 *
 * Both arrays are indexed by simulation year offset (0 = CURRENT_YEAR).
 * They are read-only after main() loads them, so they are safely shared
 * across OpenMP threads without synchronisation.
 */
static bool   g_seed_mode                  = false;
static double g_ralph_tgt[SIMULATED_YEARS] = {0};
static double g_sarah_tgt[SIMULATED_YEARS]  = {0};

/*
 * Runtime moonbrook sale year.  Set to each candidate year in the sweep loop in
 * main(); read inside run_simulation() via init_years().  Safe to share
 * across OpenMP threads because it is written only between parallel blocks.
 */
static int  g_moonbrook_sale_year = MOONBROOK_SALE_YEAR;

/*
 * CCA deferral flag.  When true the simulation claims annual CCA on the
 * Moonbrook Street building (MOONBROOK_CCA_RATE × UCC, capped at rental income to
 * avoid creating a net rental loss), reducing taxable rental income each year
 * but increasing CCA recapture income in the sale year.
 * Written only between parallel blocks; safe to read from all threads.
 */
static bool g_cca_enabled = false;

/*
 * Sequence-of-Returns (SOR) stress-test mode.
 *
 * When --sor is passed, financial investment assets (RRSP, LIF/DCPP, TFSA,
 * non-reg) receive fixed negative returns for two consecutive years starting
 * the year after the first spouse retires.  Real estate and income-flow
 * assets are unaffected.  All Monte Carlo trials use the same bad years so
 * the optimiser finds the best RRSP strategy *given* the stress scenario.
 *
 * g_sor_year_1 / g_sor_year_2 are computed once in main() from the
 * RALPH/SARAH_RETIREMENT_YEAR defines; they are written before the first
 * parallel block and read-only thereafter.
 */
static bool g_sor_mode  = false;
static int  g_sor_year_1 = 0;
static int  g_sor_year_2 = 0;

/*
 * RRIF cash-bucket mode.
 *
 * When --bucket is passed, each spouse's RRSP/RRIF is split at first withdrawal
 * into an equity portion (normal volatility) and a conservative cash bucket
 * (RRIF_BUCKET_RETURN / RRIF_BUCKET_VOLATILITY).  Mandatory RRIF minimums are
 * drawn from the bucket first; the bucket is only refilled in years with
 * positive equity shocks (Kitces rebalancing discipline).  Total RRSP value
 * and tax treatment are unchanged — the bucket is an internal allocation,
 * not a separate account.  See params.h for full mechanics.
 *
 * Written before the parallel block; read-only thereafter.
 */
static bool g_rrif_bucket_enabled = false;

/*
 * OAS-target RRSP withdrawal mode.
 *
 * When --oas-target is passed, each spouse's RRSP target_withdrawal is
 * rebased every simulation year to the inflation-indexed OAS clawback
 * threshold (tax.c:get_oas_clawback_threshold), rather than being seeded
 * at the first-bracket ceiling and random-walking from year to year.  The
 * ±RRSP_TARGET_WALK_RANGE MC perturbation is still applied each year so the
 * optimiser can find the exact sweet spot just below the threshold where
 * RRSP draws maximise bracket-fill without triggering OAS recovery tax.
 *
 * Rationale: once OAS starts flowing (age 70 in this plan), every
 * dollar of taxable income above the threshold claws back 15c of OAS
 * on top of the marginal tax rate.  Before age 70, the threshold is
 * still a useful soft ceiling because larger RRIF draws early shrink
 * the forced-mandatory base later.
 *
 * Written before the parallel block; read-only thereafter.
 */
static bool g_oas_target_enabled = false;

/*
 * Surviving-spouse scenario mode.
 *
 * When --survivor <ralph|sarah> <age> is passed, the named spouse dies at the
 * given age.  In the death year:
 *   - All of their registered accounts (RRSP, LIF/DCPP) roll into the
 *     surviving spouse's RRSP tax-free (CRA "refund of premiums" rollover).
 *   - TFSA rolls to the survivor as successor holder (no TFSA room consumed).
 *   - Non-registered account transfers at ACB (spousal rollover election —
 *     no deemed disposition at death).
 *   - Both properties (Moonbrook and Robertson) roll to the survivor, including
 *     cost basis, UCC, and any remaining mortgage obligation.
 *   - A CPP survivor benefit is added to the surviving spouse's CPP income,
 *     capped at the combined-pension maximum (see CPP_SURVIVOR_MAX_COMBINED_ANNUAL).
 *   - All remaining ASSET_OTHER_INCOME (OAS, rental, Polaron) of the deceased
 *     is zeroed.
 *   - Household expenses drop to SURVIVOR_EXPENSE_FACTOR (70%) of the prior
 *     joint amount, effective immediately in the death year.
 *
 * g_survivor_year is computed from the deceased's year-of-birth + age.
 * Written once in main(), read-only during the parallel MC loop.
 */
static bool g_survivor_mode   = false;
static int  g_survivor_spouse = RALPH_IDX;   /* who dies: RALPH_IDX or SARAH_IDX */
static int  g_survivor_year   = 0;           /* calendar year of death           */

/*
 * Per-spouse retirement year.  Defaults to the compile-time #define constants
 * and can be overridden at runtime via --retire <name> <age>.
 * Written once in main(), read-only during the parallel MC loop.
 */
static int g_retire_year[2] = { RALPH_RETIREMENT_YEAR, SARAH_RETIREMENT_YEAR };

/*
 * Portable per-thread LCG.  Using a thread-local seed avoids races on the
 * global rand() state without requiring the POSIX-only rand_r() extension.
 * Quality is sufficient for the uniform grid sampling in the MC loop.
 */
static inline int local_rand(unsigned int *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (int)((*seed >> 1) & 0x7fffffff);
}

static inline double local_rand_unit(unsigned int *seed)
{
    return ((double)local_rand(seed) + 1.0) / 2147483648.0;
}

static double local_rand_normal(unsigned int *seed)
{
    const double two_pi = 6.28318530717958647693;
    double u1 = local_rand_unit(seed);
    double u2 = local_rand_unit(seed);

    if (u1 < 1e-12) u1 = 1e-12;

    return sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
}

/* Fractional change applied when entering each spending phase. */
static double go_go_spending_pct   = ((double)GO_GO_SPENDING   / (double)KIDS_HOME_SPENDING);
static double slow_go_spending_pct = ((double)SLOW_GO_SPENDING / (double)GO_GO_SPENDING);
static double no_go_spending_pct   = ((double)NO_GO_SPENDING   / (double)SLOW_GO_SPENDING);

/* =========================================================================
 * INITIAL SPOUSE AND ASSET TEMPLATES
 * ====================================================================== */

static spouse_t ralph_template = {
    .name     = "Ralph",
    .id       = RALPH_IDX,
    .yob      = 1979,
    .yor      = RALPH_RETIREMENT_YEAR,
    .salary   = RALPH_SALARY,
    /* Remaining salary: months still worked from CURRENT_MONTH up to
     * (but not including) RALPH_RETIREMENT_MONTH.  April–June = 3 months. */
    .rys      = (double)RALPH_SALARY * RALPH_MONTHS_TO_WORK / 12.0,
    /* Income earned Jan 1 – (CURRENT_MONTH-1) before the simulation starts.
     * Added to current_year_income in year 0 for correct bracket placement
     * but NOT to cash (already in the starting balance).  Only non-zero when
     * retiring in CURRENT_YEAR; non-retiring spouses receive a full-year
     * salary via the normal path and need no correction. */
    .ytd_income = (RALPH_RETIREMENT_YEAR == CURRENT_YEAR)
                  ? (double)RALPH_SALARY * (CURRENT_MONTH - 1) / 12.0
                  : 0.0,
    /* Each spouse holds half the combined family spending.  subtract_expenses()
     * sums both halves to deduct the full annual amount.  Partial-year
     * prorating for the first calendar year is applied in subtract_expenses(). */
    .expenses = (double)CURRENT_SPENDING / 2.0,
};

static spouse_t sarah_template = {
    .name     = "Sarah",
    .id       = SARAH_IDX,
    .yob      = 1972,
    .yor      = SARAH_RETIREMENT_YEAR,
    .salary   = SARAH_SALARY,
    /* Remaining salary: months still worked from CURRENT_MONTH up to
     * (but not including) SARAH_RETIREMENT_MONTH.  April–June = 3 months. */
    .rys      = (double)SARAH_SALARY * SARAH_MONTHS_TO_WORK / 12.0,
    .ytd_income = (SARAH_RETIREMENT_YEAR == CURRENT_YEAR)
                  ? (double)SARAH_SALARY * (CURRENT_MONTH - 1) / 12.0
                  : 0.0,
    /* Each spouse holds half the combined family spending.  subtract_expenses()
     * sums both halves to deduct the full annual amount.  Partial-year
     * prorating for the first calendar year is applied in subtract_expenses(). */
    .expenses = (double)CURRENT_SPENDING / 2.0,
};

/* --- Asset templates ---------------------------------------------------- */

static asset_t ralph_cpp_template = {
    .name           = "Ralph CPP",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = RALPH_IDX,
    .is_cpp         = true,
    .value          = (RALPH_CPP_MONTHLY_PAYMENT * 12),
    .percent_growth = INFLATION,
    .min_age        = RALPH_CPP_START_AGE,
    .last_year      = YEAR_NEVER,
};

static asset_t sarah_cpp_template = {
    .name           = "Sarah CPP",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = SARAH_IDX,
    .is_cpp         = true,
    .value          = (SARAH_CPP_MONTHLY_PAYMENT * 12),
    .percent_growth = INFLATION,
    .min_age        = SARAH_CPP_START_AGE,
    .last_year      = YEAR_NEVER,
};

static asset_t ralph_polaron_template = {
    .name       = "Ralph Polaron",
    .type       = ASSET_OTHER_INCOME,
    .spouse_idx = RALPH_IDX,
    .value      = 150,
    .last_year  = 2038,
};

static asset_t sarah_polaron_template = {
    .name       = "Sarah Polaron",
    .type       = ASSET_OTHER_INCOME,
    .spouse_idx = SARAH_IDX,
    .value      = 150,
    .last_year  = 2038,
};

static asset_t ralph_rent_template = {
    .name           = "Ralph Moonbrook Rent",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = RALPH_IDX,
    .is_moonbrook_rent  = true,
    .value          = MOONBROOK_ANNUAL_RENT_PROFIT / 2.0,
    .last_year      = MOONBROOK_SALE_YEAR - 1,
    .percent_growth = MOONBROOK_RENT_NET_GROWTH,
};

static asset_t sarah_rent_template = {
    .name           = "Sarah Moonbrook Rent",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = SARAH_IDX,
    .is_moonbrook_rent  = true,
    .value          = MOONBROOK_ANNUAL_RENT_PROFIT / 2.0,
    .last_year      = MOONBROOK_SALE_YEAR - 1,
    .percent_growth = MOONBROOK_RENT_NET_GROWTH,
};

static asset_t ralph_oas_template = {
    .name           = "Ralph OAS",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = RALPH_IDX,
    .is_oas         = true,
    .value          = OAS_DEFERRED_ANNUAL,
    .min_age        = OAS_START_AGE,
    .percent_growth = INFLATION,
    .no_year        = MOONBROOK_SALE_YEAR,
    .last_year      = YEAR_NEVER,
};

static asset_t sarah_oas_template = {
    .name           = "Sarah OAS",
    .type           = ASSET_OTHER_INCOME,
    .spouse_idx     = SARAH_IDX,
    .is_oas         = true,
    .value          = OAS_DEFERRED_ANNUAL,
    .min_age        = OAS_START_AGE,
    .percent_growth = INFLATION,
    .no_year        = MOONBROOK_SALE_YEAR,
    .last_year      = YEAR_NEVER,
};

static asset_t ralph_moonbrook_template = {
    .name                     = "Ralph Moonbrook Street",
    .type                     = ASSET_PROPERTY,
    .spouse_idx               = RALPH_IDX,
    .value                    = MOONBROOK_CURRENT_VALUE / 2.0,
    .percent_growth           = MOONBROOK_ANNUAL_GROWTH,
    .sale_year                = MOONBROOK_SALE_YEAR,
    .capital_gains            = true,
    .cost                     = MOONBROOK_PURCHASE_COST / 2.0,   /* ACB — original purchase price */
    .ucc                      = MOONBROOK_UCC / 2.0,
    .last_year                = YEAR_NEVER,
    .mortgage_balance         = MOONBROOK_MORTGAGE_BALANCE / 2.0,
    .mortgage_monthly_payment = MOONBROOK_MORTGAGE_MONTHLY_PAYMENT / 2.0,
    .mortgage_interest_rate   = MOONBROOK_MORTGAGE_INTEREST_RATE,
};

static asset_t sarah_moonbrook_template = {
    .name                     = "Sarah Moonbrook Street",
    .type                     = ASSET_PROPERTY,
    .spouse_idx               = SARAH_IDX,
    .value                    = MOONBROOK_CURRENT_VALUE / 2.0,
    .percent_growth           = MOONBROOK_ANNUAL_GROWTH,
    .sale_year                = MOONBROOK_SALE_YEAR,
    .capital_gains            = true,
    .cost                     = MOONBROOK_PURCHASE_COST / 2.0,   /* ACB — original purchase price */
    .ucc                      = MOONBROOK_UCC / 2.0,
    .last_year                = YEAR_NEVER,
    .mortgage_balance         = MOONBROOK_MORTGAGE_BALANCE / 2.0,
    .mortgage_monthly_payment = MOONBROOK_MORTGAGE_MONTHLY_PAYMENT / 2.0,
    .mortgage_interest_rate   = MOONBROOK_MORTGAGE_INTEREST_RATE,
};

static asset_t ralph_robertson_template = {
    .name                    = "Ralph Robertson Court",
    .type                    = ASSET_PROPERTY,
    .spouse_idx              = RALPH_IDX,
    .value                   = ROBERTSON_CURRENT_VALUE / 2.0,
    .percent_growth          = ROBERTSON_ANNUAL_GROWTH,
    .sale_year               = ROBERTSON_SALE_YEAR,
    .tax_free                = true,
    .last_year               = YEAR_NEVER,
    .mortgage_balance        = ROBERTSON_MORTGAGE_BALANCE / 2.0,
    .mortgage_monthly_payment = ROBERTSON_MORTGAGE_MONTHLY_PAYMENT / 2.0,
    .mortgage_interest_rate  = ROBERTSON_MORTGAGE_INTEREST_RATE,
};

static asset_t sarah_robertson_template = {
    .name                    = "Sarah Robertson Court",
    .type                    = ASSET_PROPERTY,
    .spouse_idx              = SARAH_IDX,
    .value                   = ROBERTSON_CURRENT_VALUE / 2.0,
    .percent_growth          = ROBERTSON_ANNUAL_GROWTH,
    .sale_year               = ROBERTSON_SALE_YEAR,
    .tax_free                = true,
    .last_year               = YEAR_NEVER,
    .mortgage_balance        = ROBERTSON_MORTGAGE_BALANCE / 2.0,
    .mortgage_monthly_payment = ROBERTSON_MORTGAGE_MONTHLY_PAYMENT / 2.0,
    .mortgage_interest_rate  = ROBERTSON_MORTGAGE_INTEREST_RATE,
};

static asset_t ralph_dcpp_template = {
    .name           = "Ralph LIRA and DCPP",
    .type           = ASSET_LOCKED_IN,
    .spouse_idx     = RALPH_IDX,
    .value          = STARTING_RALPH_DCPP,
    .min_age        = LIF_MIX_AGE,
    .percent_growth = RALPH_GROWTH_RATE,
    .last_year      = YEAR_NEVER,
};

static asset_t sarah_dcpp_template = {
    .name           = "Sarah LIRA and DCPP",
    .type           = ASSET_LOCKED_IN,
    .spouse_idx     = SARAH_IDX,
    .value          = STARTING_SARAH_DCPP,
    .min_age        = LIF_MIX_AGE,
    .percent_growth = SARAH_GROWTH_RATE,
    .last_year      = YEAR_NEVER,
};

static asset_t ralph_rrsp_template = {
    .name              = "Ralph RRSP",
    .type              = ASSET_RRSP,
    .spouse_idx        = RALPH_IDX,
    .value             = STARTING_RALPH_RRSP,
    .room              = STARTING_RALPH_RRSP_ROOM,
    .percent_growth    = RALPH_GROWTH_RATE,
    .target_withdrawal = LOWEST_TAX_THRESHOLD,
    .first_year        = RALPH_RETIREMENT_YEAR + 1,
    .last_year         = YEAR_NEVER,
};

static asset_t sarah_rrsp_template = {
    .name                = "Sarah RRSP",
    .type                = ASSET_RRSP,
    .spouse_idx          = SARAH_IDX,
    .value               = STARTING_SARAH_RRSP,
    .room                = STARTING_SARAH_RRSP_ROOM,
    .percent_growth      = SARAH_GROWTH_RATE,
    .target_withdrawal   = LOWEST_TAX_THRESHOLD,
    .first_year          = SARAH_RETIREMENT_YEAR + 1,
    .last_year           = YEAR_NEVER,
    /* Sarah (born 1972) elects to use Ralph's age (born 1979) for RRIF minimum
     * withdrawals.  Ralph is 7 years younger, reducing Sarah's mandatory minimum
     * from 5.28% (at her age 71) to 3.85% (Ralph's age 64) — a 27% reduction
     * that grows gradually as both spouses age together. */
    .use_spouse_rrif_age = true,
};

static asset_t ralph_tfsa_template = {
    .name             = "Ralph TFSA",
    .type             = ASSET_TFSA,
    .spouse_idx       = RALPH_IDX,
    .tax_free         = true,
    .accept_deposits  = true,
    .value            = STARTING_RALPH_TFSA,
    .room             = STARTING_RALPH_TFSA_ROOM,
    .flat_room_growth = TFSA_ANNUAL_ROOM_BASE,
    .percent_growth   = RALPH_GROWTH_RATE,
    .last_year        = YEAR_NEVER,
};

static asset_t sarah_tfsa_template = {
    .name             = "Sarah TFSA",
    .type             = ASSET_TFSA,
    .spouse_idx       = SARAH_IDX,
    .tax_free         = true,
    .accept_deposits  = true,
    .value            = STARTING_SARAH_TFSA,
    .room             = STARTING_SARAH_TFSA_ROOM,
    .flat_room_growth = TFSA_ANNUAL_ROOM_BASE,
    .percent_growth   = SARAH_GROWTH_RATE,
    .last_year        = YEAR_NEVER,
};

static asset_t ralph_nonreg_template = {
    .name            = "Ralph NonReg",
    .type            = ASSET_NONREG,
    .spouse_idx      = RALPH_IDX,
    .capital_gains   = true,
    .accept_deposits = true,
    .value           = 1700,
    .cost            = 1700,   /* 1000 + 700 */
    .percent_growth  = RALPH_GROWTH_RATE,
    .last_year       = YEAR_NEVER,
};

static asset_t sarah_nonreg_template = {
    .name            = "Sarah NonReg",
    .type            = ASSET_NONREG,
    .spouse_idx      = SARAH_IDX,
    .capital_gains   = true,
    .accept_deposits = true,
    .value           = 0,
    .percent_growth  = RALPH_GROWTH_RATE,
    .last_year       = YEAR_NEVER,
};

/* =========================================================================
 * YEAR STATE
 * ====================================================================== */

static year_t years[SIMULATED_YEARS + 5];
/* Each thread runs an independent simulation; threadprivate gives every
 * thread its own years[] so concurrent trials never alias each other. */
#pragma omp threadprivate(years)

/*
 * Cached indices into the assets[] array for the six account types accessed
 * by the withdrawal helpers and split_pension_income().  The asset layout is
 * fixed at initialisation time, so these indices never change between trials.
 * Populated once by compute_asset_indices() before the parallel loop;
 * read-only thereafter (no synchronisation needed).
 */
typedef struct {
    int ralph_rrsp,        sarah_rrsp;
    int ralph_dcpp,        sarah_dcpp;
    int ralph_tfsa,        sarah_tfsa;
    int ralph_nonreg,      sarah_nonreg;
    int ralph_cpp,         sarah_cpp;
    int ralph_moonbrook_prop,  sarah_moonbrook_prop;
    int ralph_moonbrook_rent,  sarah_moonbrook_rent;
    int ralph_robertson_prop, sarah_robertson_prop;
} asset_idx_t;

static asset_idx_t aidx;

/* =========================================================================
 * FORWARD DECLARATIONS
 * ====================================================================== */

static void   compute_asset_indices(void);
static void   init_years(void);
static void   copy_previous_year(int i);
static void   age_spouse(int i, int j);
static void   advance_assets(int i);
static void   subtract_expenses(int i);
static void   subtract_mortgage_payments(int i);
static void   make_mandatory_debits(int i);
static void   make_rrsp_contributions(int i);
static void   apply_survivor_death(int i);
static void   calculate_taxes(int i, int j);
static void   grow_cash(int i);
static void   grow_assets(int i, unsigned int *ext_seed);
static void   make_next_safest_debits(int i, double needed_cash);
static bool   make_non_reg_debits(int i, double needed_cash);
static bool   make_last_resort_debits(int i, double needed_cash);
static void   invest_extra_cash(int i);
static void   split_pension_income(int i);
static void   top_up_pension_income_credit(int i);
static void   fill_first_bracket(int i);
static void   harvest_non_reg_gains(int i);
static double default_rrsp_target_for_year(void);
/* Returns 0 on success, or the calendar year cash ran out on failure. */
static int    run_simulation(double ralph_adj, double sarah_adj,
                             unsigned int *strategy_seed,
                             unsigned int *market_seed);

/* =========================================================================
 * ASSET INDEX CACHE
 * ====================================================================== */

/*
 * Populate aidx by calling init_years() once to establish the asset layout,
 * then scanning years[0] for each account type.  Must be called from the
 * primary thread before the parallel loop; aidx is read-only thereafter.
 */
static void compute_asset_indices(void)
{
    init_years();   /* fill years[0] so we can inspect asset types */

    for (int j = 0; j < MAX_ASSETS; j++) {
        switch (years[0].assets[j].type) {
        case ASSET_RRSP:
            if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_rrsp  = j;
            else                                             aidx.sarah_rrsp   = j;
            break;
        case ASSET_LOCKED_IN:
            if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_dcpp  = j;
            else                                             aidx.sarah_dcpp   = j;
            break;
        case ASSET_TFSA:
            if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_tfsa  = j;
            else                                             aidx.sarah_tfsa   = j;
            break;
        case ASSET_NONREG:
            if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_nonreg = j;
            else                                             aidx.sarah_nonreg  = j;
            break;
        case ASSET_OTHER_INCOME:
            if (years[0].assets[j].is_cpp) {
                if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_cpp        = j;
                else                                             aidx.sarah_cpp         = j;
            } else if (years[0].assets[j].is_moonbrook_rent) {
                if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_moonbrook_rent = j;
                else                                             aidx.sarah_moonbrook_rent  = j;
            }
            break;
        case ASSET_PROPERTY:
            if (years[0].assets[j].sale_year == MOONBROOK_SALE_YEAR) {
                if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_moonbrook_prop   = j;
                else                                             aidx.sarah_moonbrook_prop    = j;
            } else if (years[0].assets[j].sale_year == ROBERTSON_SALE_YEAR) {
                if (years[0].assets[j].spouse_idx == RALPH_IDX) aidx.ralph_robertson_prop = j;
                else                                             aidx.sarah_robertson_prop  = j;
            }
            break;
        default:
            break;
        }
    }
}

/* =========================================================================
 * YEAR INITIALISATION
 * ====================================================================== */

static void init_years(void)
{
    years[0].current_year = CURRENT_YEAR - 1;

    years[0].spouse[0] = ralph_template;
    years[0].spouse[1] = sarah_template;

    years[0].spouse[0].age = years[0].current_year - years[0].spouse[0].yob;
    years[0].spouse[1].age = years[0].current_year - years[0].spouse[1].yob;

    years[0].cash = STARTING_CASH;

    /* Asset layout must match the header column order in write_header_to_file(). */
    years[0].assets[0]  = ralph_rrsp_template;
    years[0].assets[1]  = sarah_rrsp_template;
    years[0].assets[2]  = ralph_dcpp_template;
    years[0].assets[3]  = sarah_dcpp_template;
    years[0].assets[4]  = ralph_nonreg_template;
    years[0].assets[5]  = sarah_nonreg_template;
    years[0].assets[6]  = ralph_tfsa_template;
    years[0].assets[7]  = sarah_tfsa_template;
    years[0].assets[8]  = ralph_robertson_template;
    years[0].assets[9]  = sarah_robertson_template;
    years[0].assets[10] = ralph_moonbrook_template;
    years[0].assets[11] = sarah_moonbrook_template;
    years[0].assets[12] = ralph_rent_template;
    years[0].assets[13] = sarah_rent_template;
    years[0].assets[14] = ralph_oas_template;
    years[0].assets[15] = sarah_oas_template;
    years[0].assets[16] = ralph_cpp_template;
    years[0].assets[17] = sarah_cpp_template;
    years[0].assets[18] = ralph_polaron_template;
    years[0].assets[19] = sarah_polaron_template;

    for (int j = 0; j < MAX_ASSETS; j++) {
        years[0].assets[j].owner = &years[0].spouse[years[0].assets[j].spouse_idx];
        /* Seed cy_realized_growth with the expected base rate so the year-0
         * CSV row (the starting-balance snapshot, before any grow_assets call)
         * shows the base assumption rather than a misleading 0.00%. */
        years[0].assets[j].cy_realized_growth = years[0].assets[j].percent_growth;
    }

    /*
     * Override moonbrook-related fields with the runtime moonbrook sale year so that
     * the sweep across candidate years works correctly.  Template values use
     * the sentinel MOONBROOK_SALE_YEAR; we patch them here to g_moonbrook_sale_year:
     *   • Moonbrook property assets:  sale_year (triggers the sale/gain calculation)
     *   • Moonbrook rent income:      last_year  (stops rental income after sale)
     *   • OAS assets:             no_year    (OAS skipped in the sale year due
     *                                         to large capital-gain income)
     */
    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[0].assets[j].sale_year == MOONBROOK_SALE_YEAR)
            years[0].assets[j].sale_year = g_moonbrook_sale_year;
        if (years[0].assets[j].is_moonbrook_rent)
            years[0].assets[j].last_year = g_moonbrook_sale_year - 1;
        if (years[0].assets[j].no_year == MOONBROOK_SALE_YEAR)
            years[0].assets[j].no_year = g_moonbrook_sale_year;
    }
}

/* =========================================================================
 * YEAR ADVANCEMENT
 * ====================================================================== */

/* Update one spouse's age, salary, and expenses for year i. */
static void age_spouse(int i, int j)
{
    years[i].spouse[j].age = years[i].current_year - years[i].spouse[j].yob;

    years[i].spouse[j].salary   += years[i].spouse[j].salary   * INFLATION;
    years[i].spouse[j].rys      += years[i].spouse[j].rys      * INFLATION;

    if (years[i].current_year == years[i].spouse[j].yor) {
        years[i].spouse[j].salary = years[i].spouse[j].rys;
    } else if (years[i].current_year > years[i].spouse[j].yor) {
        years[i].spouse[j].salary = 0;
    }

    years[i].spouse[j].current_year_income        = 0;
    years[i].spouse[j].current_year_capital_gains = 0;
    years[i].spouse[j].cy_pension_income          = 0;
    years[i].spouse[j].cy_oas_income              = 0;
    years[i].spouse[j].cy_cpp_income              = 0;
    years[i].spouse[j].cy_harvest_sell_value      = 0;
    years[i].spouse[j].cy_harvest_capital_gains   = 0;

    years[i].spouse[j].expenses += years[i].spouse[j].expenses * INFLATION;

    /* Step down spending at each lifestyle-phase transition. */
    if (years[i].current_year == GO_GO_YEAR) {
        years[i].spouse[j].expenses *= go_go_spending_pct;
    } else if (years[i].current_year == SLOW_GO_YEAR) {
        years[i].spouse[j].expenses *= slow_go_spending_pct;
    } else if (years[i].current_year == NO_GO_YEAR) {
        years[i].spouse[j].expenses *= no_go_spending_pct;
    }
}

/* Reset per-year withdrawal fields and update LIF/RRIF rates for year i. */
static void advance_assets(int i)
{
    double sarah_lif_transfer  = 0;
    double ralph_lif_transfer = 0;

    /* Capture prior-year net rental income before clearing withdrawal_amount.
     * CRA: 18% of prior-year net earned income becomes new RRSP room. */
    double prior_ralph_rental = years[i].assets[aidx.ralph_moonbrook_rent].withdrawal_amount;
    double prior_sarah_rental  = years[i].assets[aidx.sarah_moonbrook_rent].withdrawal_amount;

    for (int j = 0; j < MAX_ASSETS; j++) {
        years[i].assets[j].withdrawal_amount = 0;
        years[i].assets[j].after_tax_amount  = 0;
        years[i].assets[j].tax_amount        = 0;

        if (years[i].assets[j].type == ASSET_TFSA &&
                years[i].assets[j].flat_room_growth > 0.0) {
            years[i].assets[j].room += years[i].assets[j].flat_room_growth
                                       * get_inflation_factor(years[i].current_year);
        } else {
            years[i].assets[j].room += years[i].assets[j].flat_room_growth;
        }
        years[i].assets[j].owner = &years[i].spouse[years[i].assets[j].spouse_idx];

        /* Update maximum LIF withdrawal rate. */
        if (years[i].assets[j].type == ASSET_LOCKED_IN) {
            years[i].assets[j].max_percent_withdrawal =
                get_lif_max_rate(years[i].assets[j].owner->age);

            /* Transfer half the DCPP balance to RRSP on the first year that
             * BOTH conditions are met: the owner has reached LIF eligibility
             * age AND has retired.  Two cases:
             *   - Retire before LIF age: fires in the LIF-age year
             *     (hits_lif_age=true, already retired).
             *   - Retire after LIF age:  fires in the retirement year
             *     (hits_retirement=true, already past LIF age).
             * Using exact-year flags keeps the transfer one-shot. */
            {
                bool hits_lif_age    = (years[i].assets[j].owner->age == years[i].assets[j].min_age);
                bool hits_retirement = (years[i].current_year         == years[i].assets[j].owner->yor);
                bool lif_eligible    = (years[i].assets[j].owner->age >= years[i].assets[j].min_age);
                bool retired         = (years[i].current_year         >= years[i].assets[j].owner->yor);

                if (retired && lif_eligible && (hits_lif_age || hits_retirement)) {
                    if (years[i].assets[j].owner->id == SARAH_IDX) {
                        sarah_lif_transfer = years[i].assets[j].value / 2.0;
                        years[i].assets[j].value -= sarah_lif_transfer;
                    } else {
                        ralph_lif_transfer = years[i].assets[j].value / 2.0;
                        years[i].assets[j].value -= ralph_lif_transfer;
                    }
                }
            }
        }

        /* Update minimum RRIF withdrawal rate.
         * If use_spouse_rrif_age is set, look up the rate using the other
         * spouse's age instead of the owner's age.  This implements the CRA
         * election that lets the older spouse use the younger spouse's age to
         * reduce mandatory minimums (beneficial when one spouse is significantly
         * younger). */
        if (years[i].assets[j].type == ASSET_RRSP) {
            int rate_age;
            if (years[i].assets[j].use_spouse_rrif_age) {
                int spouse_idx = 1 - years[i].assets[j].spouse_idx;
                rate_age = years[i].spouse[spouse_idx].age;
            } else {
                rate_age = years[i].assets[j].owner->age;
            }
            years[i].assets[j].min_percent_withdrawal = get_rrif_min_rate(rate_age);
        }
    }

    /* Credit LIF transfers into the matching RRSP. */
    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[i].assets[j].type != ASSET_RRSP) continue;

        if (years[i].assets[j].owner->id == SARAH_IDX) {
            years[i].assets[j].value += sarah_lif_transfer;
            sarah_lif_transfer = 0;
        } else {
            years[i].assets[j].value += ralph_lif_transfer;
            ralph_lif_transfer = 0;
        }
    }

    /* Add new RRSP contribution room earned from prior-year net rental income.
     * CRA rule: 18% of net earned income from the previous year becomes
     * available room on January 1 of the current year. */
    if (prior_ralph_rental > 0.0)
        years[i].assets[aidx.ralph_rrsp].room += RRSP_ROOM_RATE * prior_ralph_rental;
    if (prior_sarah_rental > 0.0)
        years[i].assets[aidx.sarah_rrsp].room  += RRSP_ROOM_RATE * prior_sarah_rental;

    /* Add new RRSP room earned from prior-year salary.
     * Skip the first simulation year: STARTING_*_RRSP_ROOM already includes
     * 18% of the prior calendar year's employment income. */
    if (years[i].current_year > CURRENT_YEAR) {
        int   rrsp_idx[2] = { aidx.ralph_rrsp, aidx.sarah_rrsp };
        for (int s = 0; s < 2; s++) {
            double prior_salary = years[i-1].spouse[s].salary;
            if (prior_salary <= 0.0) continue;
            double new_room = RRSP_ROOM_RATE * prior_salary;
            if (new_room > RRSP_ANNUAL_LIMIT) new_room = RRSP_ANNUAL_LIMIT;
            years[i].assets[rrsp_idx[s]].room += new_room;
        }
    }
}

/* Carry state from year i-1 to year i, then age spouses and advance assets. */
static void copy_previous_year(int i)
{
    years[i] = years[i - 1];
    years[i].current_year++;

    age_spouse(i, RALPH_IDX);
    age_spouse(i, SARAH_IDX);

    advance_assets(i);
}

/* =========================================================================
 * CASH FLOW HELPERS
 * ====================================================================== */

static void subtract_expenses(int i)
{
    /* In the first calendar year the simulation only covers CURRENT_MONTH
     * through December — starting cash already reflects Jan–(CURRENT_MONTH-1)
     * spending.  Scale the full-annual expense figure accordingly. */
    double frac = (years[i].current_year == CURRENT_YEAR) ? PARTIAL_YEAR_FRACTION : 1.0;
    years[i].cash -= years[i].spouse[0].expenses * frac;
    years[i].cash -= years[i].spouse[1].expenses * frac;

    /* Once Robertson Court is sold the household pays rent or assisted-living
     * fees.  The constant is expressed in CURRENT_YEAR dollars, so inflate it
     * forward to the simulation year using compound inflation.
     * Track the amount in cy_senior_living so it appears in the Expenses column. */
    years[i].cy_senior_living = 0.0;
    if (years[i].current_year > ROBERTSON_SALE_YEAR) {
        double inflation_factor = pow(1.0 + INFLATION,
                                      years[i].current_year - CURRENT_YEAR);
        years[i].cy_senior_living = SENIOR_LIVING_ANNUAL_RENT * inflation_factor * frac;
        years[i].cash -= years[i].cy_senior_living;
    }
}

/*
 * Subtract annual mortgage payments for all ASSET_PROPERTY holdings that
 * still carry an outstanding balance.  Payments are amortised month-by-month
 * (12 iterations) so that principal reduction and interest accrual are
 * accurate throughout the year.  The cash account is debited by the total
 * amount actually paid; the mortgage_balance is reduced by the principal
 * portion only.  Once the balance reaches zero the loop exits early and no
 * further payments are made.
 */
static void subtract_mortgage_payments(int i)
{
    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[i].assets[j].type != ASSET_PROPERTY) continue;

        /* Reset annual totals unconditionally so stale values from a previous
         * year never carry forward after the mortgage is paid off or the
         * property is sold. */
        years[i].assets[j].cy_mortgage_interest  = 0.0;
        years[i].assets[j].cy_mortgage_principal = 0.0;

        if (years[i].assets[j].mortgage_balance <= 0.0) continue;
        if (years[i].assets[j].value            == 0.0) continue; /* already sold */

        double monthly_rate      = years[i].assets[j].mortgage_interest_rate / 12.0;
        double monthly_payment   = years[i].assets[j].mortgage_monthly_payment;
        double balance           = years[i].assets[j].mortgage_balance;
        double total_paid        = 0.0;
        double total_interest    = 0.0;
        double total_principal   = 0.0;

        /* Partial first year: only make the remaining monthly payments. */
        int n_payments = (years[i].current_year == CURRENT_YEAR) ? PARTIAL_YEAR_MONTHS : 12;

        for (int m = 0; m < n_payments; m++) {
            if (balance <= 0.0) break;
            double interest  = balance * monthly_rate;
            double principal = monthly_payment - interest;
            if (principal >= balance) {
                /* Final payoff — only pay what is left. */
                total_interest  += interest;
                total_principal += balance;
                total_paid      += balance + interest;
                balance          = 0.0;
            } else {
                total_interest  += interest;
                total_principal += principal;
                total_paid      += monthly_payment;
                balance         -= principal;
            }
        }

        /* Always debit only the PRINCIPAL portion from cash.  The interest
         * portion is expected to be accounted for in each property's income
         * or expense figures (e.g. MOONBROOK_ANNUAL_RENT_PROFIT is set to cover
         * interest, and Robertson's interest is part of living expenses).
         * Principal is forced savings that will be recovered at sale. */
        years[i].cash -= total_principal;
        years[i].assets[j].mortgage_balance      = balance;
        years[i].assets[j].cy_mortgage_interest  = total_interest;
        years[i].assets[j].cy_mortgage_principal = total_principal;
    }
}

static void grow_cash(int i)
{
    /* Prorate cash growth to the fraction of the year actually modelled. */
    double frac = (years[i].current_year == CURRENT_YEAR) ? PARTIAL_YEAR_FRACTION : 1.0;
    years[i].cash += years[i].cash * INFLATION * frac;
}

static void grow_assets(int i, unsigned int *ext_seed)
{
    /* Prorate asset growth in the partial first year. */
    double frac = (years[i].current_year == CURRENT_YEAR) ? PARTIAL_YEAR_FRACTION : 1.0;
    double financial_shock = local_rand_normal(ext_seed) * FINANCIAL_RETURN_VOLATILITY;
    double property_shock  = local_rand_normal(ext_seed) * PROPERTY_RETURN_VOLATILITY;
    double rent_shock      = local_rand_normal(ext_seed) * RENT_RETURN_VOLATILITY;
    /* Independent shock for the RRIF cash bucket — cash/GIC returns are
     * not perfectly correlated with equities.  Drawn even when --bucket is
     * disabled so the random stream is identical across modes (makes the
     * bucket-on vs bucket-off comparison apples-to-apples). */
    double bucket_shock    = local_rand_normal(ext_seed) * RRIF_BUCKET_VOLATILITY;

    for (int j = 0; j < MAX_ASSETS; j++) {
        /* One-time 10 % OAS step-up at age 75 (permanent enhancement since 2022).
         * Applied here so the higher amount takes effect from the following year
         * onward, compounding with inflation as normal. */
        if (years[i].assets[j].is_oas &&
            years[i].assets[j].owner->age == 75) {
            years[i].assets[j].value *= (1.0 + OAS_AGE75_SUPPLEMENT);
        }

        /* Under SOR stress mode, override the return rate for investment
         * accounts in the two bad years.  Property and income-flow assets
         * (OAS, CPP, rent) keep their normal percent_growth. */
        double growth_rate = years[i].assets[j].percent_growth;
        asset_type_e t = years[i].assets[j].type;
        bool is_inv = (t == ASSET_RRSP || t == ASSET_LOCKED_IN ||
                       t == ASSET_TFSA || t == ASSET_NONREG);

        if (g_sor_mode && is_inv) {
            if (years[i].current_year == g_sor_year_1)
                growth_rate = SOR_YEAR_1_RETURN;
            else if (years[i].current_year == g_sor_year_2)
                growth_rate = SOR_YEAR_2_RETURN;
            else
                growth_rate += financial_shock + SOR_ANNUAL_COMPENSATION;
        } else if (is_inv) {
            growth_rate += financial_shock;
        } else if (t == ASSET_PROPERTY) {
            growth_rate += property_shock;
        } else if (years[i].assets[j].is_moonbrook_rent) {
            growth_rate += rent_shock;
        }

        if (growth_rate < -0.95)
            growth_rate = -0.95;

        /* Record the realized rate so write_asset_to_file can emit it to the
         * CSV (stored pre-frac — this is the annual rate, not prorated). */
        years[i].assets[j].cy_realized_growth = growth_rate;

        /* RRIF cash-bucket split: when --bucket is enabled, grow the equity
         * portion (value - bucket) at the financial/SOR rate and the bucket
         * portion at the conservative rate.  SOR years DO split — protecting
         * the bucket from the stress return is the whole point of the
         * strategy; without that the bucket would crash alongside equity. */
        if (g_rrif_bucket_enabled && t == ASSET_RRSP) {
            int s = years[i].assets[j].spouse_idx;
            double bucket = years[i].rrif_bucket[s];
            if (bucket > years[i].assets[j].value)
                bucket = years[i].assets[j].value;   /* safety clamp */
            double equity = years[i].assets[j].value - bucket;

            double bucket_rate = RRIF_BUCKET_RETURN + bucket_shock;
            if (bucket_rate < -0.20) bucket_rate = -0.20;  /* cash can't crash */

            double new_equity = equity + equity * growth_rate * frac;
            double new_bucket = bucket + bucket * bucket_rate * frac;

            years[i].assets[j].value = new_equity + new_bucket;
            years[i].rrif_bucket[s]  = new_bucket;
        } else {
            years[i].assets[j].value += years[i].assets[j].value * growth_rate * frac;
        }
    }

    /*
     * RRIF bucket refill (Kitces discipline).
     *
     * At year end, if the equity portion of the RRIF had a positive shock,
     * top the bucket back up to target = RRIF_BUCKET_YEARS_TARGET × (value ×
     * min_percent_withdrawal).  In a down year (financial_shock ≤ 0) the
     * bucket is left alone so forced mandatory withdrawals next year still
     * come from cash rather than crashed equity — the whole point of the
     * strategy.  SOR-stress years also skip refill.
     *
     * This is an internal reallocation: total RRSP.value is unchanged, only
     * the earmark between equity and bucket shifts.
     */
    if (g_rrif_bucket_enabled && financial_shock > 0.0 &&
        !(g_sor_mode && (years[i].current_year == g_sor_year_1 ||
                         years[i].current_year == g_sor_year_2)))
    {
        int rrsp_idx[2] = { aidx.ralph_rrsp, aidx.sarah_rrsp };
        for (int s = 0; s < 2; s++) {
            int  ridx  = rrsp_idx[s];
            double val = years[i].assets[ridx].value;
            double rate = years[i].assets[ridx].min_percent_withdrawal;
            if (val <= 0.0 || rate <= 0.0) continue;   /* not yet in RRIF phase */

            double target = RRIF_BUCKET_YEARS_TARGET * val * rate;
            if (target > val) target = val;
            if (years[i].rrif_bucket[s] < target) {
                years[i].rrif_bucket[s] = target;
            }
        }
    }

    /*
     * Moonbrook rent profit adjustment for mortgage interest savings.
     *
     * As the Moonbrook mortgage is paid down, the interest portion of each
     * monthly payment shrinks, meaning the net rental profit grows beyond
     * plain inflation.  After subtract_mortgage_payments() runs, each Moonbrook
     * property asset holds cy_mortgage_interest for the current year.
     * We compare it to the prior year's interest; any reduction is pure
     * additional profit, split evenly between the two rent income assets.
     *
     * Special case: year 1 (CURRENT_YEAR) has only PARTIAL_YEAR_MONTHS of
     * payments, so its interest is smaller by construction.  We normalise it
     * to a full-year equivalent before comparing to the next year's full-year
     * interest, avoiding a false "negative saving".
     */
    if (i > 1 &&
        years[i].current_year <= g_moonbrook_sale_year - 1 &&
        years[i].assets[aidx.ralph_moonbrook_prop].mortgage_balance > 0.0)
    {
        /* Moonbrook mortgage interest saving. */
        double prev_moonbrook = years[i-1].assets[aidx.ralph_moonbrook_prop].cy_mortgage_interest
                          + years[i-1].assets[aidx.sarah_moonbrook_prop].cy_mortgage_interest;
        double curr_moonbrook = years[i].assets[aidx.ralph_moonbrook_prop].cy_mortgage_interest
                          + years[i].assets[aidx.sarah_moonbrook_prop].cy_mortgage_interest;

        /* 37.5% of Robertson interest also appears on the Moonbrook balance sheet
         * (the LOC borrowed against the primary residence was used to finance
         * the Moonbrook purchase).  Include its reduction as additional saving. */
        double prev_robertson = years[i-1].assets[aidx.ralph_robertson_prop].cy_mortgage_interest
                            + years[i-1].assets[aidx.sarah_robertson_prop].cy_mortgage_interest;
        double curr_robertson = years[i].assets[aidx.ralph_robertson_prop].cy_mortgage_interest
                            + years[i].assets[aidx.sarah_robertson_prop].cy_mortgage_interest;

        /* Normalise partial first year to a full-year basis for both loans. */
        if (years[i-1].current_year == CURRENT_YEAR) {
            prev_moonbrook   = prev_moonbrook   * 12.0 / PARTIAL_YEAR_MONTHS;
            prev_robertson = prev_robertson * 12.0 / PARTIAL_YEAR_MONTHS;
        }

        double interest_saving = (prev_moonbrook - curr_moonbrook)
                               + MOONBROOK_LOC_ROBERTSON_FRACTION * (prev_robertson - curr_robertson);
        if (interest_saving > 0.0) {
            years[i].assets[aidx.ralph_moonbrook_rent].value += interest_saving / 2.0;
            years[i].assets[aidx.sarah_moonbrook_rent].value  += interest_saving / 2.0;
        }
    }
}

/* =========================================================================
 * MANDATORY INCOME AND WITHDRAWALS
 * ====================================================================== */

static void make_mandatory_debits(int i)
{
    /* Add salary income for all years.
     * age_spouse() already inflates salary each year, sets it to rys (partial
     * year) in the retirement year, and zeroes it afterwards — no extra guard
     * needed here. */
    for (int s = 0; s < 2; s++) {
        years[i].spouse[s].current_year_income += years[i].spouse[s].salary;
    }

    /* Year 0 bracket correction: add the salary earned Jan–(CURRENT_MONTH-1)
     * before the simulation start.  This income is already in the opening cash
     * balance so we do NOT touch cash — only taxable income, so that calculate_
     * taxes() places each spouse in the correct marginal bracket for the year. */
    if (years[i].current_year == CURRENT_YEAR) {
        for (int s = 0; s < 2; s++) {
            years[i].spouse[s].current_year_income += years[i].spouse[s].ytd_income;
        }
    }

    for (int j = 0; j < MAX_ASSETS; j++) {

        /* --- Other income (CPP, OAS, rent, etc.) --- */
        if (years[i].assets[j].type == ASSET_OTHER_INCOME) {
            if (years[i].assets[j].owner->age < years[i].assets[j].min_age)      continue;
            if (years[i].current_year == years[i].assets[j].no_year)             continue;
            if (years[i].current_year > years[i].assets[j].last_year)            continue;

            /* In the first calendar year only cover the months from
             * CURRENT_MONTH through December; the starting cash already
             * reflects income received before the simulation start date. */
            double income_frac = (years[i].current_year == CURRENT_YEAR)
                               ? PARTIAL_YEAR_FRACTION : 1.0;
            double income = years[i].assets[j].value * income_frac;

            /* CCA deferral: claim Class-1 CCA against Moonbrook rental income.
             * The deduction is capped at the rental income for this year so
             * that CCA cannot create a net rental loss (CRA restriction).
             * UCC is stored on the Moonbrook property asset (per-spouse half). */
            if (years[i].assets[j].is_moonbrook_rent && g_cca_enabled) {
                int prop_idx = (years[i].assets[j].spouse_idx == RALPH_IDX)
                               ? aidx.ralph_moonbrook_prop
                               : aidx.sarah_moonbrook_prop;
                double *ucc = &years[i].assets[prop_idx].ucc;
                if (*ucc > 0.0) {
                    double cca = *ucc * MOONBROOK_CCA_RATE;
                    if (cca > income) cca = income;   /* can't exceed rental income */
                    *ucc   -= cca;
                    income -= cca;
                }
            }

            years[i].assets[j].owner->current_year_income += income;

            /* Record net rental income so advance_assets() can credit 18%
             * as new RRSP contribution room in the following year. */
            if (years[i].assets[j].is_moonbrook_rent) {
                years[i].assets[j].withdrawal_amount = income;
            }

            /* Track OAS separately for the clawback calculation. */
            if (years[i].assets[j].is_oas) {
                years[i].assets[j].owner->cy_oas_income += income;
            }
            /* Track CPP separately for the pension assignment calculation. */
            if (years[i].assets[j].is_cpp) {
                years[i].assets[j].owner->cy_cpp_income += income;
            }
        }

        if (years[i].assets[j].value == 0) continue;

        /* --- Property sales --- */
        if (years[i].assets[j].type == ASSET_PROPERTY) {
            if (years[i].current_year == years[i].assets[j].sale_year) {
                /* Any remaining mortgage balance is paid off from sale proceeds. */
                double remaining_mortgage = years[i].assets[j].mortgage_balance;
                years[i].assets[j].mortgage_balance = 0.0;

                if (years[i].assets[j].tax_free) {
                    years[i].cash += years[i].assets[j].value - remaining_mortgage;
                } else {
                    double sale_value = years[i].assets[j].value;
                    double cost       = years[i].assets[j].cost;
                    double ucc        = years[i].assets[j].ucc;

                    /* Capital gain: appreciation above the original ACB. */
                    if (sale_value > cost) {
                        years[i].assets[j].owner->current_year_capital_gains +=
                            sale_value - cost;
                    }

                    /* CCA recapture: the spread between UCC and the lesser of
                     * sale price or original cost is 100 % taxable as regular
                     * income.  The same amount is withheld from the immediate
                     * cash proceeds so it flows through the tax pipeline and
                     * is not double-counted. */
                    double recapture = 0.0;
                    if (ucc > 0.0) {
                        double proceeds_capped = (sale_value < cost) ? sale_value : cost;
                        recapture = proceeds_capped - ucc;
                        if (recapture < 0.0) recapture = 0.0;
                        if (recapture > 0.0) {
                            years[i].assets[j].owner->current_year_income += recapture;
                        }
                    }

                    /* Net cash = cost basis - recapture withheld - mortgage repaid.
                     * (Capital gain flows through current_year_capital_gains above
                     *  and is converted to cash via the tax pipeline.) */
                    years[i].cash += cost - recapture - remaining_mortgage;
                }
                years[i].assets[j].withdrawal_amount = years[i].assets[j].value;
                years[i].assets[j].value = 0;
            }
        }

        /* --- LIF mandatory withdrawal --- */
        if (years[i].assets[j].type == ASSET_LOCKED_IN) {
            if (years[i].assets[j].owner->age < years[i].assets[j].min_age) continue;
            if (years[i].current_year < years[i].assets[j].owner->yor)       continue;

            double debit      = years[i].assets[j].value * years[i].assets[j].max_percent_withdrawal;
            double half_limit = get_lif_withdrawal_limit(years[i].current_year) / 2.0;

            /* Draw down entirely if balance is below the annual limit. */
            if (years[i].assets[j].value < half_limit) {
                debit = years[i].assets[j].value;
            }

            years[i].assets[j].owner->current_year_income += debit;
            years[i].assets[j].owner->cy_pension_income   += debit;
            years[i].assets[j].value                      -= debit;
            years[i].assets[j].withdrawal_amount          += debit;
        }

        /* --- RRSP/RRIF target withdrawal --- */
        if (years[i].assets[j].type == ASSET_RRSP) {
            if (years[i].current_year < years[i].assets[j].first_year) continue;

            /* Skip all RRSP/RRIF withdrawals in the Moonbrook sale year — capital
             * gains from that sale already push marginal rates high enough that
             * any RRSP income would be taxed heavily. */
            if (years[i].current_year == g_moonbrook_sale_year) continue;

            double taxable_income =
                calc_capital_gain_inclusion(years[i].assets[j].owner->current_year_capital_gains)
                + years[i].assets[j].owner->current_year_income;

            /* Use the remaining first-bracket room as a minimum withdrawal
             * floor so mandatory debits at least fill the cheapest rate band,
             * but still allow the optimizer's target to ask for more when a
             * higher withdrawal is strategically useful. */
            double first_bracket_room = calc_first_bracket_room(taxable_income);

            double target_withdrawal = years[i].assets[j].target_withdrawal;
            if (first_bracket_room > target_withdrawal) {
                target_withdrawal = first_bracket_room;
            }

            double debit = years[i].assets[j].value * years[i].assets[j].min_percent_withdrawal;

            if (debit < target_withdrawal) {
                debit = target_withdrawal;
            }

            if (debit > years[i].assets[j].value) {
                debit = years[i].assets[j].value;
            }

            /* RRIF cash-bucket seeding + drain.  On the first withdrawal year
             * the bucket starts at 0; seed it to target so the very first
             * forced withdrawal has cash backing and isn't crystallising equity.
             * Then drain the bucket by the current debit before the equity
             * portion is touched.  Tax treatment is unchanged — the whole
             * debit still flows into current_year_income. */
            if (g_rrif_bucket_enabled) {
                int s = years[i].assets[j].spouse_idx;
                if (years[i].rrif_bucket[s] <= 0.0) {
                    double seed = RRIF_BUCKET_YEARS_TARGET
                                * years[i].assets[j].value
                                * years[i].assets[j].min_percent_withdrawal;
                    if (seed > years[i].assets[j].value) seed = years[i].assets[j].value;
                    years[i].rrif_bucket[s] = seed;
                }
                double from_bucket = (debit < years[i].rrif_bucket[s])
                                   ? debit : years[i].rrif_bucket[s];
                years[i].rrif_bucket[s] -= from_bucket;
            }

            years[i].assets[j].owner->current_year_income += debit;
            years[i].assets[j].owner->cy_pension_income   += debit;
            years[i].assets[j].value                      -= debit;
            years[i].assets[j].withdrawal_amount          += debit;
        }
    }
}

/* =========================================================================
 * RRSP CONTRIBUTIONS
 * ====================================================================== */

/*
 * Make RRSP contributions during high-income years to defer tax at the
 * highest marginal rate, improving retirement net worth.
 *
 * Triggered for each spouse when:
 *   - It is a working year (current_year <= yor): salary is in income and
 *     the marginal rate is higher than it will be in retirement.
 *   - It is the moonbrook sale year: property disposition generates a capital-
 *     gains spike; contributing soaks up ordinary income to reduce the tax
 *     burden (capital gains themselves are unaffected).
 *
 * Contribution = full available room, capped at the spouse's current-year
 * ordinary income so taxable income never goes negative.  The deduction is
 * applied directly to current_year_income before calculate_taxes() runs.
 */
static void make_rrsp_contributions(int i)
{
    int rrsp_idx[2] = { aidx.ralph_rrsp, aidx.sarah_rrsp };

    for (int s = 0; s < 2; s++) {
        bool is_working    = years[i].current_year <= years[i].spouse[s].yor;
        bool is_moonbrook_year = years[i].current_year == g_moonbrook_sale_year;

        if (!is_working && !is_moonbrook_year) continue;

        double room   = years[i].assets[rrsp_idx[s]].room;
        if (room <= 0.0) continue;

        double income = years[i].spouse[s].current_year_income;
        if (income <= 0.0) continue;

        double contribution = (room < income) ? room : income;

        years[i].spouse[s].current_year_income        -= contribution;
        years[i].assets[rrsp_idx[s]].value            += contribution;
        years[i].assets[rrsp_idx[s]].room             -= contribution;
        /* Negative withdrawal_amount signals a contribution in the CSV so the
         * docx / xlsx can surface it as an action item. */
        years[i].assets[rrsp_idx[s]].withdrawal_amount -= contribution;
    }
}

/*
 * Deposit the employer's DCPP contribution for each working spouse.
 *
 * The contribution is DCPP_EMPLOYER_CONTRIBUTION_RATE × salary, credited
 * directly to the DCPP/LIF asset with no effect on taxable income (employer
 * DCPP contributions are not a T4 benefit to the employee).
 *
 * Prorating is automatic: age_spouse() already sets each spouse's .salary
 * to .rys (remaining-year salary) in the retirement year and to 0 afterwards,
 * so the correct partial-year amount flows through without any extra logic.
 *
 * Survivor mode: if the spouse is deceased this year their salary is 0 and
 * the condition guards against posting a contribution.
 */
static void make_dcpp_employer_contributions(int i)
{
    int dcpp_idx[2] = { aidx.ralph_dcpp, aidx.sarah_dcpp };

    for (int s = 0; s < 2; s++) {
        double sal = years[i].spouse[s].salary;
        if (sal <= 0.0) continue;   /* retired or deceased — no contribution */

        double contribution = sal * DCPP_EMPLOYER_CONTRIBUTION_RATE;
        years[i].assets[dcpp_idx[s]].value += contribution;
    }
}

/* =========================================================================
 * SURVIVING-SPOUSE DEATH EVENT
 * ====================================================================== */

/*
 * Apply the one-time death event for the g_survivor_spouse in year i.
 *
 * Must be called immediately after copy_previous_year(i) and before any
 * income, expense, or withdrawal processing for year i.  The changes are
 * written into years[i] and carry forward automatically via copy_previous_year
 * in subsequent years.
 *
 * Asset transfers follow standard Canadian spousal rollover rules:
 *   - RRSP + LIF/DCPP → surviving spouse's RRSP (refund-of-premiums rollover)
 *   - TFSA             → surviving spouse's TFSA (successor-holder designation)
 *   - Non-reg          → surviving spouse's non-reg at ACB (no deemed disposition)
 *   - Properties       → surviving spouse, including ACB, UCC, and mortgage
 *   - CPP survivor benefit added (60%, capped at combined-pension maximum)
 *   - All other ASSET_OTHER_INCOME for the deceased zeroed (OAS, rent, Polaron)
 *   - Household expenses reduced to SURVIVOR_EXPENSE_FACTOR of joint amount
 */
static void apply_survivor_death(int i)
{
    int d = g_survivor_spouse;      /* deceased */
    int s = 1 - d;                  /* survivor */

    /* Resolve asset indices for both spouses. */
    int d_rrsp    = (d == RALPH_IDX) ? aidx.ralph_rrsp        : aidx.sarah_rrsp;
    int s_rrsp    = (d == RALPH_IDX) ? aidx.sarah_rrsp         : aidx.ralph_rrsp;
    int d_dcpp    = (d == RALPH_IDX) ? aidx.ralph_dcpp        : aidx.sarah_dcpp;
    int d_tfsa    = (d == RALPH_IDX) ? aidx.ralph_tfsa        : aidx.sarah_tfsa;
    int s_tfsa    = (d == RALPH_IDX) ? aidx.sarah_tfsa         : aidx.ralph_tfsa;
    int d_nonreg  = (d == RALPH_IDX) ? aidx.ralph_nonreg      : aidx.sarah_nonreg;
    int s_nonreg  = (d == RALPH_IDX) ? aidx.sarah_nonreg       : aidx.ralph_nonreg;
    int d_cpp     = (d == RALPH_IDX) ? aidx.ralph_cpp         : aidx.sarah_cpp;
    int s_cpp     = (d == RALPH_IDX) ? aidx.sarah_cpp          : aidx.ralph_cpp;
    int d_moonbrook   = (d == RALPH_IDX) ? aidx.ralph_moonbrook_prop  : aidx.sarah_moonbrook_prop;
    int s_moonbrook   = (d == RALPH_IDX) ? aidx.sarah_moonbrook_prop   : aidx.ralph_moonbrook_prop;
    int d_frent   = (d == RALPH_IDX) ? aidx.ralph_moonbrook_rent  : aidx.sarah_moonbrook_rent;
    int s_frent   = (d == RALPH_IDX) ? aidx.sarah_moonbrook_rent   : aidx.ralph_moonbrook_rent;
    int d_man     = (d == RALPH_IDX) ? aidx.ralph_robertson_prop : aidx.sarah_robertson_prop;
    int s_man     = (d == RALPH_IDX) ? aidx.sarah_robertson_prop  : aidx.ralph_robertson_prop;

    /* --- RRSP + LIF/DCPP → survivor's RRSP (tax-free rollover) --- */
    years[i].assets[s_rrsp].value += years[i].assets[d_rrsp].value
                                   + years[i].assets[d_dcpp].value;
    years[i].assets[d_rrsp].value  = 0.0;
    years[i].assets[d_rrsp].room   = 0.0;   /* RRSP contribution room does not transfer */
    years[i].assets[d_dcpp].value  = 0.0;

    /* The deceased spouse's RRIF bucket rolls over with the balance.  DCPP
     * value arrives as equity (no earmark), so the surviving bucket simply
     * adds the deceased bucket; the refill rule will right-size it next
     * year if it drifts below the new value × min-rate target. */
    years[i].rrif_bucket[s] += years[i].rrif_bucket[d];
    years[i].rrif_bucket[d]  = 0.0;

    /* --- TFSA → survivor's TFSA (successor-holder, no room consumed) --- */
    years[i].assets[s_tfsa].value += years[i].assets[d_tfsa].value;
    years[i].assets[d_tfsa].value           = 0.0;
    years[i].assets[d_tfsa].room            = 0.0;
    years[i].assets[d_tfsa].flat_room_growth = 0.0;  /* No post-mortem TFSA room accumulation */

    /* --- Non-reg → survivor at ACB (spousal rollover, no deemed disposition) --- */
    years[i].assets[s_nonreg].value += years[i].assets[d_nonreg].value;
    years[i].assets[s_nonreg].cost  += years[i].assets[d_nonreg].cost;
    years[i].assets[d_nonreg].value  = 0.0;
    years[i].assets[d_nonreg].cost   = 0.0;

    /* --- Moonbrook Street property → survivor (spousal rollover) ---
     * Survivor now owns 100%: value, ACB, UCC, and full mortgage obligation. */
    years[i].assets[s_moonbrook].value                    += years[i].assets[d_moonbrook].value;
    years[i].assets[s_moonbrook].cost                     += years[i].assets[d_moonbrook].cost;
    years[i].assets[s_moonbrook].ucc                      += years[i].assets[d_moonbrook].ucc;
    years[i].assets[s_moonbrook].mortgage_balance         += years[i].assets[d_moonbrook].mortgage_balance;
    years[i].assets[s_moonbrook].mortgage_monthly_payment += years[i].assets[d_moonbrook].mortgage_monthly_payment;
    years[i].assets[d_moonbrook].value                     = 0.0;
    years[i].assets[d_moonbrook].cost                      = 0.0;
    years[i].assets[d_moonbrook].ucc                       = 0.0;
    years[i].assets[d_moonbrook].mortgage_balance          = 0.0;
    years[i].assets[d_moonbrook].mortgage_monthly_payment  = 0.0;

    /* Survivor receives 100% of Moonbrook rental income going forward. */
    years[i].assets[s_frent].value += years[i].assets[d_frent].value;

    /* --- Robertson Court property → survivor (principal-residence exemption) --- */
    years[i].assets[s_man].value                    += years[i].assets[d_man].value;
    years[i].assets[s_man].mortgage_balance         += years[i].assets[d_man].mortgage_balance;
    years[i].assets[s_man].mortgage_monthly_payment += years[i].assets[d_man].mortgage_monthly_payment;
    years[i].assets[d_man].value                     = 0.0;
    years[i].assets[d_man].mortgage_balance          = 0.0;
    years[i].assets[d_man].mortgage_monthly_payment  = 0.0;

    /* --- CPP survivor benefit ---
     * Computed before the income-stream zero-out below.
     * The combined maximum (own CPP + survivor benefit) is capped at
     * CPP_SURVIVOR_MAX_COMBINED_ANNUAL indexed to the death year. */
    {
        double dec_cpp = years[i].assets[d_cpp].value;
        double sur_cpp = years[i].assets[s_cpp].value;
        double cap_now = CPP_SURVIVOR_MAX_COMBINED_ANNUAL;
        int    elapsed = years[i].current_year - CURRENT_YEAR;
        for (int k = 0; k < elapsed; k++) cap_now *= (1.0 + INFLATION);
        double boost = dec_cpp * CPP_SURVIVOR_RATE;
        double room  = cap_now - sur_cpp;
        if (room  < 0.0)  room  = 0.0;
        if (boost > room) boost = room;
        years[i].assets[s_cpp].value += boost;
    }

    /* --- Zero all remaining ASSET_OTHER_INCOME for the deceased ---
     * (CPP, OAS, Moonbrook rent, Polaron — anything this loop catches.) */
    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[i].assets[j].spouse_idx == d &&
            years[i].assets[j].type == ASSET_OTHER_INCOME) {
            years[i].assets[j].value = 0.0;
        }
    }

    /* --- Stop the deceased's employment ---
     * Setting yor to the death year makes age_spouse() zero salary in all
     * future years (current_year > yor). */
    years[i].spouse[d].salary = 0.0;
    years[i].spouse[d].rys    = 0.0;
    years[i].spouse[d].yor    = years[i].current_year;

    /* --- Reduce household expenses to SURVIVOR_EXPENSE_FACTOR of joint ---
     * age_spouse() already inflated and phase-stepped both halves; capture
     * the combined total and assign the scaled amount to the survivor only. */
    double joint = years[i].spouse[RALPH_IDX].expenses
                 + years[i].spouse[SARAH_IDX].expenses;
    years[i].spouse[d].expenses = 0.0;
    years[i].spouse[s].expenses = joint * SURVIVOR_EXPENSE_FACTOR;
}

/* =========================================================================
 * TAX CALCULATION
 * ====================================================================== */

/* Compute taxes, take-home, and net for spouse j in simulation year i. */
static void calculate_taxes(int i, int j)
{
    double taxable_income = years[i].spouse[j].current_year_income;

    taxable_income +=
        calc_capital_gain_inclusion(years[i].spouse[j].current_year_capital_gains);

    /* Capital losses on non-reg positions can pull taxable_income below zero.
     * Clamp here so the value is always meaningful before it is logged and
     * passed downstream.  taxes_owing_on_income() also guards against this,
     * but an explicit clamp catches the problem at the source. */
    if (taxable_income < 0.0) taxable_income = 0.0;

    years[i].spouse[j].cy_taxes = 0;

    if (prints_on) {
        printf("   taxable_income = %16.2f\n", taxable_income);
    }

    double taxes     = taxes_owing_on_income(taxable_income);
    double exemption = calc_personal_exemption(years[i].current_year, taxable_income);

    if (taxes > exemption) {
        years[i].spouse[j].cy_taxes = taxes - exemption;
    }

    /* Pension Income Amount credit: applies at age 65+ on up to $2,000 of
     * eligible pension income (RRSP/RRIF and LIF withdrawals).
     * Non-refundable — cannot reduce taxes below zero. */
    if (years[i].spouse[j].age >= PENSION_INCOME_CREDIT_AGE &&
        years[i].spouse[j].cy_pension_income > 0)
    {
        double eligible = years[i].spouse[j].cy_pension_income;
        if (eligible > PENSION_INCOME_CREDIT_CEILING) {
            eligible = PENSION_INCOME_CREDIT_CEILING;
        }
        double pension_credit = eligible * PENSION_INCOME_CREDIT_RATE;
        if (pension_credit > years[i].spouse[j].cy_taxes) {
            pension_credit = years[i].spouse[j].cy_taxes;
        }
        years[i].spouse[j].cy_taxes -= pension_credit;
    }

    /* Age Amount credit: applies at age 65+, phases out with net income.
     * Non-refundable — cannot reduce taxes below zero. */
    double age_credit = calc_age_amount_credit(
        years[i].spouse[j].age, taxable_income, years[i].current_year);
    if (age_credit > years[i].spouse[j].cy_taxes) {
        age_credit = years[i].spouse[j].cy_taxes;
    }
    years[i].spouse[j].cy_taxes -= age_credit;

    /* OAS Recovery Tax (clawback): reduces take-home by 15% of net income
     * above the threshold, capped at the OAS amount received.
     * Unlike the credits above this increases the effective tax burden. */
    double oas_clawback = calc_oas_clawback(
        taxable_income,
        years[i].spouse[j].cy_oas_income,
        years[i].current_year);
    years[i].spouse[j].cy_taxes += oas_clawback;

    if (prints_on) {
        printf("   taxes          = %16.2f\n", years[i].spouse[j].cy_taxes);
    }

    years[i].spouse[j].cy_take_home =
        years[i].spouse[j].current_year_capital_gains
        + years[i].spouse[j].current_year_income
        - years[i].spouse[j].cy_taxes;
}

/* =========================================================================
 * DISCRETIONARY WITHDRAWALS
 * ====================================================================== */

/* Withdraw needed_cash from TFSA accounts split proportionally by balance. */
static void make_next_safest_debits(int i, double needed_cash)
{
    int    cti = aidx.ralph_tfsa;
    int    lti = aidx.sarah_tfsa;
    double ralph_tfsa_value = years[i].assets[cti].value;
    double sarah_tfsa_value  = years[i].assets[lti].value;

    double total_value = ralph_tfsa_value + sarah_tfsa_value;

    /* Drain both TFSAs entirely if combined balance is insufficient. */
    if (total_value < needed_cash) {
        years[i].cash                          += ralph_tfsa_value + sarah_tfsa_value;
        years[i].assets[cti].withdrawal_amount += ralph_tfsa_value;
        years[i].assets[lti].withdrawal_amount += sarah_tfsa_value;
        years[i].assets[cti].room              += ralph_tfsa_value;
        years[i].assets[lti].room              += sarah_tfsa_value;
        years[i].assets[cti].value              = 0;
        years[i].assets[lti].value              = 0;
        return;
    }

    double ralph_ratio  = ralph_tfsa_value / total_value;
    double ralph_debit  = ralph_ratio * needed_cash;
    double sarah_debit   = needed_cash - ralph_debit;

    years[i].assets[cti].withdrawal_amount += ralph_debit;
    years[i].assets[lti].withdrawal_amount += sarah_debit;
    years[i].assets[cti].room              += ralph_debit;
    years[i].assets[lti].room              += sarah_debit;
    years[i].assets[cti].value             -= ralph_debit;
    years[i].assets[lti].value             -= sarah_debit;
    years[i].cash                          += needed_cash;
}

/*
 * Withdraw needed_cash from RRSP/RRIF as a last resort.
 * Returns true if both accounts are empty (caller should stop retrying).
 */
static bool make_last_resort_debits(int i, double needed_cash)
{
    int    cti = aidx.ralph_rrsp;
    int    lti = aidx.sarah_rrsp;
    double ralph_rrsp_value = years[i].assets[cti].value;
    double sarah_rrsp_value  = years[i].assets[lti].value;

    double total_value = ralph_rrsp_value + sarah_rrsp_value;

    if (total_value == 0) {
        return true;   /* exhausted */
    }

    double ralph_needed = needed_cash / 2.0;
    double sarah_needed  = needed_cash - ralph_needed;

    if (ralph_rrsp_value == 0) { sarah_needed = needed_cash; ralph_needed = 0; }
    if (sarah_rrsp_value  == 0) { ralph_needed = needed_cash; sarah_needed = 0; }

    if (ralph_rrsp_value < ralph_needed) ralph_needed = ralph_rrsp_value;
    if (sarah_rrsp_value  < sarah_needed)  sarah_needed  = sarah_rrsp_value;

    years[i].assets[cti].owner->current_year_income += ralph_needed;
    years[i].assets[cti].owner->cy_pension_income   += ralph_needed;
    years[i].assets[cti].withdrawal_amount          += ralph_needed;
    years[i].assets[cti].value                      -= ralph_needed;

    years[i].assets[lti].owner->current_year_income += sarah_needed;
    years[i].assets[lti].owner->cy_pension_income   += sarah_needed;
    years[i].assets[lti].withdrawal_amount          += sarah_needed;
    years[i].assets[lti].value                      -= sarah_needed;

    /* Keep the RRIF bucket ≤ remaining RRSP value.  Last-resort draws happen
     * when all cheaper sources are exhausted — the bucket portion, if any,
     * has already been drained for the normal minimum; this clamp just
     * covers the edge case where the remaining value shrinks below the
     * bucket balance. */
    if (g_rrif_bucket_enabled) {
        if (years[i].rrif_bucket[RALPH_IDX] > years[i].assets[cti].value)
            years[i].rrif_bucket[RALPH_IDX] = years[i].assets[cti].value;
        if (years[i].rrif_bucket[SARAH_IDX]  > years[i].assets[lti].value)
            years[i].rrif_bucket[SARAH_IDX]  = years[i].assets[lti].value;
    }

    return false;
}

/*
 * Withdraw needed_cash from non-registered accounts.
 * Capital gains are computed using the correct proportional ACB
 * (adjusted cost base) for each withdrawal.
 * Returns true if both accounts are empty (caller should stop retrying).
 */
static bool make_non_reg_debits(int i, double needed_cash)
{
    int    cti = aidx.ralph_nonreg;
    int    lti = aidx.sarah_nonreg;
    double ralph_nonreg_value = years[i].assets[cti].value;
    double sarah_nonreg_value  = years[i].assets[lti].value;

    double total_value = ralph_nonreg_value + sarah_nonreg_value;

    if (total_value == 0) {
        return true;   /* exhausted */
    }

    double ralph_needed = needed_cash / 2.0;
    double sarah_needed  = needed_cash - ralph_needed;

    if (ralph_nonreg_value == 0) { sarah_needed = needed_cash; ralph_needed = 0; }
    if (sarah_nonreg_value  == 0) { ralph_needed = needed_cash; sarah_needed = 0; }

    if (ralph_nonreg_value < ralph_needed) ralph_needed = ralph_nonreg_value;
    if (sarah_nonreg_value  < sarah_needed)  sarah_needed  = sarah_nonreg_value;

    if (ralph_needed > 0) {
        /* Capital gain = proceeds − proportional ACB of the withdrawn portion. */
        double ralph_ratio = ralph_needed / years[i].assets[cti].value;
        double ralph_acb   = years[i].assets[cti].cost * ralph_ratio;
        years[i].assets[cti].owner->current_year_capital_gains += ralph_needed - ralph_acb;
        years[i].assets[cti].cost              -= ralph_acb;
        years[i].assets[cti].withdrawal_amount += ralph_needed;
        years[i].assets[cti].value             -= ralph_needed;
    }

    if (sarah_needed > 0) {
        /* Capital gain = proceeds − proportional ACB of the withdrawn portion. */
        double sarah_ratio = sarah_needed / years[i].assets[lti].value;
        double sarah_acb   = years[i].assets[lti].cost * sarah_ratio;
        years[i].assets[lti].owner->current_year_capital_gains += sarah_needed - sarah_acb;
        years[i].assets[lti].cost              -= sarah_acb;
        years[i].assets[lti].withdrawal_amount += sarah_needed;
        years[i].assets[lti].value             -= sarah_needed;
    }

    return false;
}

/* Invest surplus cash into TFSAs up to available contribution room. */
static void invest_extra_cash(int i)
{
    double infl_factor          = get_inflation_factor(years[i].current_year);
    double preferred_cash       = PREFERRED_CASH       * infl_factor;
    double nonreg_cash_threshold = NONREG_CASH_THRESHOLD * infl_factor;

    if (years[i].cash < preferred_cash) return;

    double extra_cash   = years[i].cash - preferred_cash;
    double ralph_invest = extra_cash / 2.0;
    double sarah_invest  = extra_cash - ralph_invest;

    /* In survivor mode, redirect the deceased spouse's share to the survivor.
     * Use >= so the redirect also fires in the death year itself: apply_survivor_death
     * has already zeroed the deceased's non-reg, so any surplus cash deposited later
     * in the same year must go to the survivor rather than back into a dead account. */
    if (g_survivor_mode && years[i].current_year >= g_survivor_year) {
        if (g_survivor_spouse == RALPH_IDX) {
            sarah_invest  += ralph_invest;
            ralph_invest  = 0.0;
        } else {
            ralph_invest += sarah_invest;
            sarah_invest   = 0.0;
        }
    }

    int    cti        = aidx.ralph_tfsa;
    int    lti        = aidx.sarah_tfsa;
    double ralph_room = years[i].assets[cti].room;
    double sarah_room  = years[i].assets[lti].room;

    if (sarah_room > 0 || ralph_room > 0) {
        /* Deposit up to the available room for each spouse. */
        double sarah_deposit  = (sarah_room  < sarah_invest)  ? sarah_room  : sarah_invest;
        double ralph_deposit = (ralph_room < ralph_invest) ? ralph_room : ralph_invest;

        years[i].assets[lti].value += sarah_deposit;
        years[i].assets[lti].room  -= sarah_deposit;
        years[i].cash              -= sarah_deposit;

        years[i].assets[cti].value += ralph_deposit;
        years[i].assets[cti].room  -= ralph_deposit;
        years[i].cash              -= ralph_deposit;
    }

    /* Invest any cash above the non-reg threshold into non-registered accounts.
     * ACB increases by the full deposit amount (cost basis = purchase price). */
    if (years[i].cash > nonreg_cash_threshold) {
        double to_invest     = years[i].cash - nonreg_cash_threshold;
        double ralph_deposit = to_invest / 2.0;
        double sarah_deposit  = to_invest - ralph_deposit;

        /* In survivor mode, all non-reg investment goes to the surviving spouse.
         * >= catches the death year itself (apply_survivor_death has already zeroed
         * the deceased's account, so we must not re-deposit into it). */
        if (g_survivor_mode && years[i].current_year >= g_survivor_year) {
            if (g_survivor_spouse == RALPH_IDX) {
                sarah_deposit  += ralph_deposit;
                ralph_deposit  = 0.0;
            } else {
                ralph_deposit += sarah_deposit;
                sarah_deposit   = 0.0;
            }
        }

        int cnri = aidx.ralph_nonreg;
        int lnri = aidx.sarah_nonreg;

        years[i].assets[cnri].value += ralph_deposit;
        years[i].assets[cnri].cost  += ralph_deposit;
        years[i].assets[lnri].value += sarah_deposit;
        years[i].assets[lnri].cost  += sarah_deposit;
        years[i].cash               -= to_invest;
    }
}

/* =========================================================================
 * PENSION INCOME SPLITTING
 * ====================================================================== */

/*
 * Apply both CPP pension assignment and T1032 pension income splitting.
 *
 * CPP pension assignment (done first):
 *   When both spouses are receiving CPP, each may assign up to 50% of their
 *   CPP retirement pension to the other (CRA pension assignment rules).  We
 *   shift CPP income from the higher-earning spouse toward the lower-earning
 *   one, capped at 50% of the donor's CPP, to reduce combined taxes.
 *
 * T1032 pension income splitting (done second, on post-CPP incomes):
 *   After age 65, up to half of eligible pension income (RRSP/RRIF and LIF
 *   withdrawals) may be attributed to the lower-income spouse.  cy_pension_income
 *   is adjusted proportionally so the Pension Income Amount credit in
 *   calculate_taxes() is computed on the post-split figures.
 */
static void split_pension_income(int i)
{
    /* ------------------------------------------------------------------
     * Part 1: CPP pension assignment.
     * Both spouses must be currently receiving CPP (cy_cpp_income > 0).
     * ------------------------------------------------------------------ */
    double ralph_cpp = years[i].spouse[RALPH_IDX].cy_cpp_income;
    double sarah_cpp  = years[i].spouse[SARAH_IDX].cy_cpp_income;

    if (ralph_cpp > 0.0 && sarah_cpp > 0.0) {
        for (int s = 0; s < 2; s++) {
            int    donor     = s;
            int    recipient = 1 - s;
            double donor_cpp = (s == RALPH_IDX) ? ralph_cpp : sarah_cpp;

            /* Only the higher-income spouse acts as donor. */
            if (years[i].spouse[donor].current_year_income <=
                years[i].spouse[recipient].current_year_income) continue;

            double income_gap    = (years[i].spouse[donor].current_year_income
                                   - years[i].spouse[recipient].current_year_income) / 2.0;
            double max_assign    = donor_cpp * 0.5;
            double shift_amt     = (max_assign < income_gap) ? max_assign : income_gap;

            if (shift_amt <= 0.0) continue;

            years[i].spouse[donor].current_year_income     -= shift_amt;
            years[i].spouse[recipient].current_year_income += shift_amt;

            /* Keep cy_cpp_income in sync with what was shifted. */
            years[i].spouse[donor].cy_cpp_income     -= shift_amt;
            years[i].spouse[recipient].cy_cpp_income += shift_amt;
        }
    }

    /* ------------------------------------------------------------------
     * Part 2: T1032 pension income splitting (RRSP/RRIF + LIF).
     * ------------------------------------------------------------------ */

    /* Accumulate eligible splittable pension income for each spouse
     * (RRSP/RRIF withdrawals + LIF withdrawals). */
    double ralph_eligible = years[i].assets[aidx.ralph_rrsp].withdrawal_amount
                          + years[i].assets[aidx.ralph_dcpp].withdrawal_amount;
    double sarah_eligible  = years[i].assets[aidx.sarah_rrsp].withdrawal_amount
                          + years[i].assets[aidx.sarah_dcpp].withdrawal_amount;

    /* Determine which spouse is the higher earner and attempt a split.
     * Each branch is independent so both can run in the same year if needed,
     * though in practice only one will have a positive income gap. */
    for (int s = 0; s < 2; s++) {
        int    donor     = s;
        int    recipient = 1 - s;
        double eligible  = (s == RALPH_IDX) ? ralph_eligible : sarah_eligible;

        if (years[i].spouse[donor].age < PENSION_SPLIT_AGE) continue;
        if (eligible <= 0)                                   continue;
        /* A deceased spouse cannot be a recipient in a pension split. */
        if (g_survivor_mode && years[i].current_year > g_survivor_year &&
            recipient == g_survivor_spouse)                  continue;
        if (years[i].spouse[donor].current_year_income <=
            years[i].spouse[recipient].current_year_income)  continue;

        double income_gap    = (years[i].spouse[donor].current_year_income
                               - years[i].spouse[recipient].current_year_income) / 2.0;
        double half_eligible = eligible / 2.0;
        double shift_amt     = (half_eligible > income_gap) ? income_gap : half_eligible;

        years[i].spouse[donor].current_year_income     -= shift_amt;
        years[i].spouse[recipient].current_year_income += shift_amt;

        /* Adjust cy_pension_income proportionally so the Pension Income
         * Amount credit is based on post-split pension income. */
        if (years[i].spouse[donor].cy_pension_income > 0) {
            double pension_ratio = shift_amt / eligible;
            double pension_shift = years[i].spouse[donor].cy_pension_income * pension_ratio;
            if (pension_shift > years[i].spouse[donor].cy_pension_income) {
                pension_shift = years[i].spouse[donor].cy_pension_income;
            }
            years[i].spouse[donor].cy_pension_income    -= pension_shift;
            years[i].spouse[recipient].cy_pension_income += pension_shift;
        }
    }
}

/*
 * Pension Income Amount credit floor.
 *
 * The federal + Ontario Pension Income Amount credit is worth up to $401/year
 * per spouse on the first $2,000 of eligible pension income (RRSP/RRIF and
 * LIF withdrawals at age 65+).  If a spouse has less than $2,000 of eligible
 * pension income after mandatory withdrawals and pension splitting, the unused
 * credit capacity is wasted.
 *
 * Strategy: do a small targeted RRSP top-up to bring each eligible spouse
 * up to the $2,000 ceiling.  At typical low-to-moderate retirement income
 * levels the marginal tax on the extra $2,000 is at or below the credit rate
 * (20.05%), so the net tax cost is negligible while the full $401 credit is
 * captured.  The top-up is skipped in the Moonbrook sale year, where RRSP income
 * is already suppressed to avoid stacking onto capital-gains income.
 *
 * Must be called after split_pension_income() so post-split cy_pension_income
 * figures are used.
 */
static void top_up_pension_income_credit(int i)
{
    if (years[i].current_year == g_moonbrook_sale_year) return;

    int rrsp_idx[2] = { aidx.ralph_rrsp, aidx.sarah_rrsp };

    for (int s = 0; s < 2; s++) {
        if (years[i].spouse[s].age < PENSION_INCOME_CREDIT_AGE) continue;

        double shortfall = PENSION_INCOME_CREDIT_CEILING
                         - years[i].spouse[s].cy_pension_income;
        if (shortfall <= 0.0) continue;

        int ri = rrsp_idx[s];
        if (years[i].assets[ri].value <= 0.0) continue;

        double topup = shortfall;
        if (topup > years[i].assets[ri].value)
            topup = years[i].assets[ri].value;

        years[i].assets[ri].owner->current_year_income += topup;
        years[i].assets[ri].owner->cy_pension_income   += topup;
        years[i].assets[ri].value                      -= topup;
        years[i].assets[ri].withdrawal_amount          += topup;
    }
}

/*
 * Fill each spouse's first combined tax bracket with discretionary RRSP/RRIF
 * withdrawals, routing the proceeds to TFSA via the year-end invest_extra_cash().
 *
 * The first bracket (~19.05% combined, up to ~$53,891 in 2026 indexed to
 * inflation) is the cheapest rate available for registered withdrawals.
 * Leaving income below this ceiling means the RRSP/RRIF continues to
 * compound inside the registered envelope and will eventually be drawn at
 * higher marginal rates once RRIF minimums, CPP, and OAS stack up.
 *
 * Taking that income now at 19.05% and sheltering the proceeds in TFSA lets
 * future growth compound tax-free — a net benefit whenever the deferred
 * marginal rate exceeds the current one, which is the typical outcome for
 * large RRSP balances in retirement.
 *
 * During working years, salary alone exceeds the bracket ceiling so
 * calc_first_bracket_room() returns 0 and this function does nothing.
 *
 * Called after top_up_pension_income_credit() and before the final
 * calculate_taxes() pass so the new income is captured in the tax summary.
 * Coexists with the RRIF-floor in make_mandatory_debits(): the floor fills
 * the bracket for spouses with active RRIF draws, and this pass catches the
 * residual (e.g. pre-RRIF spouses whose mandatory debit undershot the
 * bracket, or years where pension top-up room was consumed by LIF-only
 * income).  Skipped in the Moonbrook sale year to avoid stacking on large
 * capital gains.
 */
static void fill_first_bracket(int i)
{
    if (years[i].current_year == g_moonbrook_sale_year) return;

    int rrsp_idx[2] = { aidx.ralph_rrsp, aidx.sarah_rrsp };

    for (int s = 0; s < 2; s++) {
        double room = calc_first_bracket_room(
            years[i].spouse[s].current_year_income);

        if (room <= 0.0) continue;

        int ri = rrsp_idx[s];
        if (years[i].assets[ri].value <= 0.0) continue;

        double topup = room;
        if (topup > years[i].assets[ri].value)
            topup = years[i].assets[ri].value;

        years[i].assets[ri].owner->current_year_income += topup;
        years[i].assets[ri].owner->cy_pension_income   += topup;
        years[i].assets[ri].value                      -= topup;
        years[i].assets[ri].withdrawal_amount          += topup;
    }
}

/*
 * Non-registered capital-gain harvesting.
 *
 * When a spouse has unrealized gains in their non-registered account and
 * their taxable income is below the second combined bracket ceiling
 * (~$58,523 in 2026), proactively realize enough gains to fill the income
 * up to that ceiling.  The gains are "harvested" via a same-day notional
 * sell-and-rebuy:
 *   - current_year_capital_gains increases (taxable now at ~9-11% effective)
 *   - account value is unchanged (immediate repurchase)
 *   - cost basis (ACB) resets upward to the sell price
 *
 * The higher ACB reduces future capital-gains tax when the account is
 * actually drawn down in later years — often at higher marginal rates as
 * RRIF minimums, CPP, and OAS stack up.
 *
 * Two caps are applied:
 *   1. Harvest ceiling: the inflation-adjusted second bracket top returned
 *      by get_harvest_income_ceiling() (avoids crossing the large rate jump).
 *   2. OAS clawback guard: never let estimated taxable income exceed the
 *      indexed OAS clawback threshold, protecting any OAS-receiving spouse
 *      from triggering the 15% recovery tax.
 *
 * Must be called after top_up_pension_income_credit() so all income for the
 * year is accounted for before estimating bracket room.
 */
static void harvest_non_reg_gains(int i)
{
    int nonreg_idx[2] = { aidx.ralph_nonreg, aidx.sarah_nonreg };

    double harvest_ceiling  = get_harvest_income_ceiling();
    double clawback_ceiling = get_oas_clawback_threshold(years[i].current_year);

    for (int s = 0; s < 2; s++) {
        int ni = nonreg_idx[s];

        double value = years[i].assets[ni].value;
        double cost  = years[i].assets[ni].cost;

        if (value <= cost) continue;   /* No unrealized gain — nothing to harvest. */

        /* Estimate the spouse's current taxable income. */
        double est_taxable = years[i].spouse[s].current_year_income
                           + calc_capital_gain_inclusion(
                               years[i].spouse[s].current_year_capital_gains);

        /* Choose the tighter of the two ceilings for this spouse. */
        double effective_ceiling = harvest_ceiling;
        if (years[i].spouse[s].cy_oas_income > 0.0 &&
            clawback_ceiling < effective_ceiling) {
            effective_ceiling = clawback_ceiling;
        }

        double room = effective_ceiling - est_taxable;
        if (room <= 0.0) continue;   /* Already at or above the target ceiling. */

        /* Convert from included-income room to raw capital-gain room,
         * accounting for any threshold boundary already crossed this year. */
        double max_gain = calc_capital_gain_max_from_room(
                              room, years[i].spouse[s].current_year_capital_gains);

        double unrealized_gain = value - cost;
        double gain_to_harvest = (max_gain < unrealized_gain)
                               ? max_gain : unrealized_gain;
        if (gain_to_harvest <= 0.0) continue;

        /* Determine how much market value to notionally sell/rebuy in order
         * to realize gain_to_harvest.
         *   gain = sell_value * (1 - cost/value)
         *   sell_value = gain_to_harvest * value / (value - cost)          */
        double sell_value = gain_to_harvest * value / unrealized_gain;
        double sold_acb   = cost * (sell_value / value);

        /* Realize the gain (increases taxable income via inclusion rate). */
        years[i].spouse[s].current_year_capital_gains  += gain_to_harvest;
        /* Track notional-only gains separately so the cash pool can be
         * corrected after the tax calculation — no real money changed hands. */
        years[i].spouse[s].cy_harvest_capital_gains    += gain_to_harvest;

        /* Record the notional sell value — tells the action plan how much to
         * sell and immediately repurchase in order to reset the ACB. */
        years[i].spouse[s].cy_harvest_sell_value += sell_value;

        /* Reset ACB upward: remove old ACB of the sold slice, add back the
         * full sell price as new basis.  Account value is unchanged. */
        years[i].assets[ni].cost = cost - sold_acb + sell_value;
    }
}

/*
 * Baseline RRSP/RRIF withdrawal target for the optimizer.
 *
 * Anchors the random walk at the first-bracket ceiling so the optimizer
 * starts from the cheapest available rate band rather than an arbitrary
 * fixed threshold.  calc_first_bracket_room(0.0) returns the inflation-
 * adjusted bracket ceiling for the current simulation year.
 */
static double default_rrsp_target_for_year(void)
{
    return calc_first_bracket_room(0.0);
}

/* =========================================================================
 * SIMULATION RUNNER
 * ====================================================================== */

static int out_of_cash_count = 0;

/*
 * Per-year failure histogram.  out_of_cash_hist[k] counts how many simulation
 * trials ran out of cash in calendar year (CURRENT_YEAR + k).  Reset and
 * accumulated alongside out_of_cash_count for each (CCA, moonbrook-year) run.
 * Printed when all trials fail to help diagnose parameter problems.
 */
static int out_of_cash_hist[SIMULATED_YEARS];

/*
 * Load per-year RRSP withdrawal targets from a seed CSV file into the two
 * global arrays (g_ralph_tgt, g_sarah_tgt).  The expected file format is:
 *
 *   Year,  2026, 2027, ..., 2069
 *   Ralph,    0, <wd>, ...,    0
 *   Sarah,     0, <wd>, ...,    0
 *
 * Returns the number of years loaded, or 0 on error.
 */
static int load_rrsp_targets(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Error: cannot open RRSP seed file '%s'\n", path);
        return 0;
    }

    /* Zero the target arrays before filling them. */
    for (int k = 0; k < SIMULATED_YEARS; k++) {
        g_ralph_tgt[k] = 0.0;
        g_sarah_tgt[k]  = 0.0;
    }

    char line[4096];
    int  year_indices[SIMULATED_YEARS];
    int  n_years = 0;

    while (fgets(line, (int)sizeof(line), f)) {
        /* Strip trailing newline / carriage-return. */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Tokenise by comma (modifies line in-place). */
        char *tok[SIMULATED_YEARS + 2];
        int   n_tok = 0;
        char *p = line;
        while (p && n_tok < SIMULATED_YEARS + 2) {
            tok[n_tok++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        if (n_tok < 2) continue;

        if (strcmp(tok[0], "Year") == 0) {
            n_years = n_tok - 1;
            for (int k = 0; k < n_years && k < SIMULATED_YEARS; k++) {
                int yr = atoi(tok[k + 1]);
                year_indices[k] = yr - CURRENT_YEAR;  /* offset into arrays */
            }
        } else if (strcmp(tok[0], "Ralph") == 0) {
            for (int k = 0; k < n_years && k < SIMULATED_YEARS; k++) {
                int idx = year_indices[k];
                if (idx >= 0 && idx < SIMULATED_YEARS)
                    g_ralph_tgt[idx] = atof(tok[k + 1]);
            }
        } else if (strcmp(tok[0], "Sarah") == 0) {
            for (int k = 0; k < n_years && k < SIMULATED_YEARS; k++) {
                int idx = year_indices[k];
                if (idx >= 0 && idx < SIMULATED_YEARS)
                    g_sarah_tgt[idx] = atof(tok[k + 1]);
            }
        }
    }

    fclose(f);
    return n_years;
}

/*
 * Run a single simulation trial.
 *
 * Normal mode  (g_seed_mode == false):
 *   ralph_adj / sarah_adj are year-0 offsets applied to the RRSP target
 *   withdrawal amounts.  Subsequent years walk randomly by ±15 000.
 *   strategy_seed is used for the per-year withdrawal-target random walk and
 *   market_seed is used for the stochastic asset-return path.
 *
 * Seeded mode  (g_seed_mode == true):
 *   g_ralph_tgt[] / g_sarah_tgt[] supply the per-year base targets loaded
 *   from the seed file.  Each year the target is set to that base value
 *   plus a thread-local random perturbation (±15 000).  ralph_adj,
 *   sarah_adj, strategy_seed, and market_seed must be provided by the caller.
 *
 * Returns 0 on success, or the calendar year cash ran out on failure.
 * Tax brackets are reset at entry so every trial starts from the same
 * CURRENT_YEAR base.
 */
static int run_simulation(double ralph_adj, double sarah_adj,
                          unsigned int *strategy_seed,
                          unsigned int *market_seed)
{
    reset_tax_brackets();
    init_years();

    years[0].assets[0].target_withdrawal = default_rrsp_target_for_year();
    years[0].assets[1].target_withdrawal = default_rrsp_target_for_year();

    if (g_seed_mode) {
        /* Seeded mode: year-0 target comes from the loaded file (if non-zero),
         * otherwise the age-aware default baseline is kept. */
        if (g_ralph_tgt[0] > 0.0)
            years[0].assets[0].target_withdrawal = g_ralph_tgt[0];
        if (g_sarah_tgt[0] > 0.0)
            years[0].assets[1].target_withdrawal = g_sarah_tgt[0];
    } else {
        years[0].assets[0].target_withdrawal += ralph_adj;  /* Ralph RRSP */
        years[0].assets[1].target_withdrawal += sarah_adj;   /* Sarah  RRSP */
    }

    for (int i = 1; i < SIMULATED_YEARS; i++) {

        copy_previous_year(i);

        if (g_oas_target_enabled) {
            /* OAS-target mode: rebase each year's RRSP target to the
             * inflation-indexed OAS clawback threshold for every year —
             * both before and after OAS begins.
             *
             * Rationale (RRSP meltdown): the pre-OAS window (retirement to
             * age 70) is the cheapest tax window available.  Drawing each
             * spouse to the clawback threshold throughout the pre-OAS
             * window reduces forced RRIF minimums in the high-rate years.
             *
             * The optimiser's ralph_adj/sarah_adj can still push the anchor
             * below threshold if a lower draw path consistently wins, and
             * the ±RRSP_TARGET_WALK_RANGE per-year walk adds local
             * exploration in real-dollar-constant width. */
            double threshold = get_oas_clawback_threshold(years[i].current_year);
            double anchor_c  = threshold;
            double anchor_l  = threshold;
            int range = (int)(RRSP_TARGET_WALK_RANGE *
                              get_inflation_factor(years[i].current_year));
            years[i].assets[0].target_withdrawal =
                anchor_c + ralph_adj
                         + (double)((int)(local_rand(strategy_seed)
                                          % (2 * range + 1))
                                    - range);
            years[i].assets[1].target_withdrawal =
                anchor_l + sarah_adj
                         + (double)((int)(local_rand(strategy_seed)
                                          % (2 * range + 1))
                                    - range);
        } else if (g_seed_mode) {
            /* Seeded mode: anchor each year to the loaded target, then add a
             * thread-local random perturbation so the MC loop can explore
             * improvements around the known-good schedule.
             * Walk range is expressed in CURRENT_YEAR dollars and scaled by
             * get_inflation_factor() so the real-dollar width stays constant
             * over the horizon (seeded mode uses half the normal range). */
            double c_base = g_ralph_tgt[i] > 0.0
                          ? g_ralph_tgt[i]
                          : (years[i].current_year == years[i].assets[0].first_year
                              ? default_rrsp_target_for_year()
                              : years[i].assets[0].target_withdrawal);
            double l_base = g_sarah_tgt[i] > 0.0
                          ? g_sarah_tgt[i]
                          : (years[i].current_year == years[i].assets[1].first_year
                              ? default_rrsp_target_for_year()
                              : years[i].assets[1].target_withdrawal);
            int range = (int)(RRSP_TARGET_WALK_RANGE / 2.0 *
                              get_inflation_factor(years[i].current_year));
            years[i].assets[0].target_withdrawal =
                c_base + (double)((int)(local_rand(strategy_seed) % (2 * range + 1)) - range);
            years[i].assets[1].target_withdrawal =
                l_base + (double)((int)(local_rand(strategy_seed) % (2 * range + 1)) - range);
        } else {
            /* Normal mode: random walk from the previous year's target, but
             * re-anchor the first withdrawal year to the age-aware baseline so
             * the search does not inherit a pre-withdrawal fixed-threshold
             * target into retirement.
             * Walk range is expressed in CURRENT_YEAR dollars and scaled by
             * get_inflation_factor() to keep real-dollar width constant as the
             * anchor drifts up with inflation over the horizon. */
            double c_base = (years[i].current_year == years[i].assets[0].first_year)
                          ? default_rrsp_target_for_year()
                          : years[i].assets[0].target_withdrawal;
            double l_base = (years[i].current_year == years[i].assets[1].first_year)
                          ? default_rrsp_target_for_year()
                          : years[i].assets[1].target_withdrawal;
            int range = (int)(RRSP_TARGET_WALK_RANGE *
                              get_inflation_factor(years[i].current_year));
            years[i].assets[0].target_withdrawal =
                c_base + (double)((int)(local_rand(strategy_seed) % (2 * range + 1)) - range);
            years[i].assets[1].target_withdrawal =
                l_base + (double)((int)(local_rand(strategy_seed) % (2 * range + 1)) - range);
        }

        /* Surviving-spouse death event: one-shot asset transfer and expense
         * reduction.  Runs before any income/expense processing for this year. */
        if (g_survivor_mode && years[i].current_year == g_survivor_year) {
            apply_survivor_death(i);
        }

        subtract_expenses(i);
        subtract_mortgage_payments(i);
        make_mandatory_debits(i);
        make_rrsp_contributions(i);
        make_dcpp_employer_contributions(i);

        calculate_taxes(i, RALPH_IDX);
        calculate_taxes(i, SARAH_IDX);

        if (isnan(years[i].cash)) return years[i].current_year;

        double after_tax_subtotal =
            years[i].spouse[RALPH_IDX].cy_take_home +
            years[i].spouse[SARAH_IDX].cy_take_home;

        years[i].cash += after_tax_subtotal;

        /*
         * OAS clawback-aware withdrawal sequencing.
         *
         * Non-registered withdrawals generate taxable capital gains, which
         * raise net income and can trigger or worsen the OAS recovery tax
         * (15 cents per dollar above ~$91k, indexed).  TFSA withdrawals are
         * completely tax-free and have no effect on net income.
         *
         * Strategy: if either OAS-receiving spouse's current estimated taxable
         * income is within OAS_CLAWBACK_BUFFER dollars of the indexed
         * threshold, draw from TFSA first, then fall back to non-reg only if
         * TFSA is depleted.  Outside that window use the normal order
         * (non-reg first, TFSA as fallback) to preserve TFSA room for future
         * clawback-sensitive years.
         */
        bool clawback_risk = false;
        {
            double threshold = get_oas_clawback_threshold(years[i].current_year);
            for (int s = 0; s < 2; s++) {
                if (years[i].spouse[s].cy_oas_income > 0.0) {
                    double est_taxable =
                        years[i].spouse[s].current_year_income +
                        calc_capital_gain_inclusion(
                            years[i].spouse[s].current_year_capital_gains);
                    if (est_taxable > threshold - OAS_CLAWBACK_BUFFER) {
                        clawback_risk = true;
                        break;
                    }
                }
            }
        }

        /* When clawback risk is detected, prefer TFSA before non-reg. */
        if (clawback_risk && years[i].cash < PREFERRED_CASH * get_inflation_factor(years[i].current_year)) {
            make_next_safest_debits(i, PREFERRED_CASH * get_inflation_factor(years[i].current_year) - years[i].cash);
        }

        /* Draw from non-registered accounts if still needed. */
        int  limit    = 0;
        bool exhausted = false;
        while (years[i].cash < PREFERRED_CASH * get_inflation_factor(years[i].current_year)) {
            if (limit++ >= 3 || exhausted) break;

            double needed = PREFERRED_CASH * get_inflation_factor(years[i].current_year) - years[i].cash;
            exhausted = make_non_reg_debits(i, needed * NONREG_EXTRA_DEBIT);

            years[i].cash -= after_tax_subtotal;
            calculate_taxes(i, RALPH_IDX);
            calculate_taxes(i, SARAH_IDX);
            after_tax_subtotal =
                years[i].spouse[RALPH_IDX].cy_take_home +
                years[i].spouse[SARAH_IDX].cy_take_home;
            years[i].cash += after_tax_subtotal;
        }

        /* Draw from TFSA as fallback (skipped if already drawn above). */
        if (!clawback_risk && years[i].cash < PREFERRED_CASH * get_inflation_factor(years[i].current_year)) {
            make_next_safest_debits(i, PREFERRED_CASH * get_inflation_factor(years[i].current_year) - years[i].cash);
        }

        /* Draw from RRSP/RRIF as a last resort (skipped in Moonbrook sale year
         * to avoid stacking RRSP income on top of capital gains). */
        if (years[i].current_year != g_moonbrook_sale_year) {
            limit    = 0;
            exhausted = false;
            while (years[i].cash < PREFERRED_CASH * get_inflation_factor(years[i].current_year)) {
                if (limit++ >= 3 || exhausted) break;

                double needed = PREFERRED_CASH * get_inflation_factor(years[i].current_year) - years[i].cash;
                exhausted = make_last_resort_debits(i, needed * RRSP_EXTRA_DEBIT);

                years[i].cash -= after_tax_subtotal;
                calculate_taxes(i, RALPH_IDX);
                calculate_taxes(i, SARAH_IDX);
                after_tax_subtotal =
                    years[i].spouse[RALPH_IDX].cy_take_home +
                    years[i].spouse[SARAH_IDX].cy_take_home;
                years[i].cash += after_tax_subtotal;
            }
        }

        years[i].cash -= after_tax_subtotal;

        split_pension_income(i);
        top_up_pension_income_credit(i);
        fill_first_bracket(i);
        harvest_non_reg_gains(i);

        calculate_taxes(i, RALPH_IDX);
        calculate_taxes(i, SARAH_IDX);

        after_tax_subtotal =
            years[i].spouse[RALPH_IDX].cy_take_home +
            years[i].spouse[SARAH_IDX].cy_take_home;

        years[i].cash += after_tax_subtotal;

        /* Harvest is a notional sell-and-rebuy: no net cash is received, but the
         * gain was included in cy_take_home (and thus in after_tax_subtotal above).
         * Remove the gross gain to leave only the real cash effect — the tax on
         * those gains — already correctly deducted inside cy_taxes / cy_take_home. */
        for (int s = 0; s < 2; s++) {
            years[i].cash -= years[i].spouse[s].cy_harvest_capital_gains;
        }

        if (years[i].cash < -50000) {
            return years[i].current_year;   /* caller records failure year */
        }

        invest_extra_cash(i);
        grow_assets(i, market_seed);
        grow_cash(i);
        increment_tax_brackets();
    }

    return 0;
}

/* =========================================================================
 * FAILURE DIAGNOSTICS
 * ====================================================================== */

/*
 * Print a breakdown of which years cash was exhausted across all failed trials.
 *
 * Displays up to MAX_ROWS years sorted by failure count (descending), with the
 * calendar year, approximate spouse ages, run count, and percentage of trials.
 * Called after a parallel block when best_networth == 0 (all trials failed).
 */
#define CASH_EXHAUSTION_MAX_ROWS 12

static void report_cash_exhaustion(const int *hist, int n_trials)
{
    /* Collect non-zero buckets. */
    typedef struct { int year; int count; } bucket_t;
    bucket_t buckets[SIMULATED_YEARS];
    int nb = 0;
    int total = 0;
    for (int k = 0; k < SIMULATED_YEARS; k++) {
        if (hist[k] > 0) {
            buckets[nb].year  = CURRENT_YEAR + k;
            buckets[nb].count = hist[k];
            nb++;
            total += hist[k];
        }
    }
    if (nb == 0) return;

    /* Insertion-sort by count descending (nb ≤ SIMULATED_YEARS ≈ 45). */
    for (int a = 1; a < nb; a++) {
        bucket_t tmp = buckets[a];
        int b = a - 1;
        while (b >= 0 && buckets[b].count < tmp.count) {
            buckets[b + 1] = buckets[b];
            b--;
        }
        buckets[b + 1] = tmp;
    }

    int show = (nb < CASH_EXHAUSTION_MAX_ROWS) ? nb : CASH_EXHAUSTION_MAX_ROWS;

    printf("  Cash-exhaustion breakdown (%d total failures):\n", total);
    printf("  %-6s  %-8s  %-8s  %9s  %6s\n",
           "Year", "C-Age", "L-Age", "Runs", "Pct");
    printf("  %-6s  %-8s  %-8s  %9s  %6s\n",
           "------", "--------", "--------", "---------", "------");

    for (int a = 0; a < show; a++) {
        int yr = buckets[a].year;
        printf("  %-6d  %-8d  %-8d  %9d  %5.1f%%\n",
               yr,
               yr - ralph_template.yob,
               yr - sarah_template.yob,
               buckets[a].count,
               100.0 * buckets[a].count / n_trials);
    }
    if (nb > CASH_EXHAUSTION_MAX_ROWS) {
        printf("  ... and %d more years\n", nb - CASH_EXHAUSTION_MAX_ROWS);
    }
}

/* =========================================================================
 * PERCENTILE BAND PASS
 *
 * After the winning strategy is found and the representative CSV path has
 * been written, run N_PERCENTILE_PATHS independent Monte Carlo paths of that
 * strategy, collect after-tax net worth at every simulated year, sort each
 * year's column, and extract P10 / P50 / P90.  The results are appended to
 * retirement.csv so retirement_to_xlsx.py can draw a confidence band on the
 * Net Worth chart.
 *
 * Thread safety: each OMP thread runs run_simulation() against its own
 * threadprivate years[] and writes one row of path_nw[] indexed by the loop
 * variable p — no two threads share a row, so no lock is needed.
 * ====================================================================== */

#include <stdlib.h>   /* qsort, malloc, free */

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void run_percentile_pass(double ralph_adj, double sarah_adj,
                                unsigned int strategy_seed_base,
                                double *p10_out, double *p50_out,
                                double *p90_out, int num_years)
{
    /* Heap-allocate the path×year matrix to avoid stack overflow. */
    double (*path_nw)[num_years] = calloc(N_PERCENTILE_PATHS, sizeof(*path_nw));
    if (!path_nw) {
        fprintf(stderr, "Warning: percentile pass skipped (out of memory).\n");
        return;
    }

    printf("Computing P10/P50/P90 bands (%d paths)...\n", N_PERCENTILE_PATHS);

    #pragma omp parallel for schedule(dynamic, 1)
    for (int p = 0; p < N_PERCENTILE_PATHS; p++) {
        /* Fresh market seed for each path; strategy seed frozen to the
         * winner so the RRSP withdrawal schedule is identical across paths.
         * The xorshift keeps seeds diverse without a global counter. */
        unsigned int strat_seed  = strategy_seed_base;
        unsigned int market_seed = (unsigned int)(p * 1234567u ^ 0xDEADBEEFu);

        int fail_year = run_simulation(ralph_adj, sarah_adj,
                                       &strat_seed, &market_seed);

        for (int i = 0; i < num_years; i++) {
            path_nw[p][i] = (fail_year == 0 ||
                              years[i].current_year < fail_year)
                           ? sum_networth(years, i)
                           : 0.0;
        }
    }

    /* Sort each year's column and extract percentiles. */
    double *col = malloc(N_PERCENTILE_PATHS * sizeof(double));
    if (col) {
        for (int i = 0; i < num_years; i++) {
            for (int p = 0; p < N_PERCENTILE_PATHS; p++)
                col[p] = path_nw[p][i];
            qsort(col, N_PERCENTILE_PATHS, sizeof(double), cmp_double);
            /* Use the nearest-rank method: index = floor(pct × (N-1)). */
            p10_out[i] = col[(int)(0.10 * (N_PERCENTILE_PATHS - 1))];
            p50_out[i] = col[(int)(0.50 * (N_PERCENTILE_PATHS - 1))];
            p90_out[i] = col[(int)(0.90 * (N_PERCENTILE_PATHS - 1))];
        }
        free(col);
    }
    free(path_nw);
}

/* =========================================================================
 * ENTRY POINT
 * ====================================================================== */

int main(int argc, char *argv[])
{
    /* Parse flags and optional arguments.
     *
     *   -v              — verbose per-year tax output
     *   --rrsp <file>   — load per-year RRSP targets from a seed CSV and run
     *                     in seeded refinement mode instead of blind MC search
     *   --cca           — also run the CCA-enabled pass and compare strategies
     *                     (by default only the CCA-disabled pass runs)
     *   --sor           — sequence-of-returns stress test: apply fixed negative
     *                     returns (-10 %, then -5 %) to financial investment
     *                     assets for the two years after the first spouse retires
     *   --bucket        — RRIF cash-bucket strategy: earmark 2.5 years of
     *                     minimum RRIF withdrawals in conservative assets
     *                     inside the RRIF; drain-first / refill-on-up-year
     *                     rebalancing to insulate forced sales from drawdowns.
     *                     See RRIF_BUCKET_* constants in params.h
     *   --oas-target    — rebase each year's RRSP withdrawal target to the
     *                     inflation-indexed OAS clawback threshold so draws
     *                     aim to land net income just below clawback territory,
     *                     replacing the default first-bracket anchor.
     *                     The optimiser's adj offset still shifts the anchor.
     *   --survivor <name> <age>
     *                   — surviving-spouse scenario: the named spouse (ralph or
     *                     sarah) dies at the given age; all assets roll over to
     *                     the survivor and household expenses drop to 70 %
     *   --retire <name> <age>
     *                   — override the retirement age for the named spouse
     *                     (ralph or sarah); updates the RRSP start year and
     *                     remaining-year salary automatically
     */
    const char *rrsp_file = NULL;
    bool run_cca_sweep    = false;
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "-v") == 0) {
            prints_on = true;
        } else if (strcmp(argv[a], "--rrsp") == 0 && a + 1 < argc) {
            rrsp_file = argv[++a];
        } else if (strcmp(argv[a], "--cca") == 0) {
            run_cca_sweep = true;
        } else if (strcmp(argv[a], "--sor") == 0) {
            g_sor_mode = true;
            /* g_sor_year_1/2 are computed after arg parsing so that --retire
             * overrides are already in effect when the SOR window is set. */
        } else if (strcmp(argv[a], "--bucket") == 0) {
            g_rrif_bucket_enabled = true;
        } else if (strcmp(argv[a], "--oas-target") == 0) {
            g_oas_target_enabled = true;
        } else if (strcmp(argv[a], "--survivor") == 0 && a + 2 < argc) {
            const char *name     = argv[++a];
            int         die_age  = atoi(argv[++a]);
            if (strcmp(name, "ralph") == 0 || strcmp(name, "Ralph") == 0) {
                g_survivor_spouse = RALPH_IDX;
                g_survivor_year   = ralph_template.yob + die_age;
            } else if (strcmp(name, "sarah") == 0 || strcmp(name, "Sarah") == 0) {
                g_survivor_spouse = SARAH_IDX;
                g_survivor_year   = sarah_template.yob + die_age;
            } else {
                printf("Error: --survivor expects 'ralph' or 'sarah', got '%s'\n", name);
                return 1;
            }
            g_survivor_mode = true;
        } else if (strcmp(argv[a], "--retire") == 0 && a + 2 < argc) {
            const char *name    = argv[++a];
            int         ret_age = atoi(argv[++a]);
            if (strcmp(name, "ralph") == 0 || strcmp(name, "Ralph") == 0) {
                g_retire_year[RALPH_IDX] = ralph_template.yob + ret_age;
            } else if (strcmp(name, "sarah") == 0 || strcmp(name, "Sarah") == 0) {
                g_retire_year[SARAH_IDX] = sarah_template.yob + ret_age;
            } else {
                printf("Error: --retire expects 'ralph' or 'sarah', got '%s'\n", name);
                return 1;
            }
        }
    }

    /* Apply --retire overrides to templates (must happen before
     * compute_asset_indices() scans the template layout).
     *
     * rys (remaining-year salary) depends on whether the spouse retires in the
     * current calendar year or a future one:
     *   same year  →  months from CURRENT_MONTH to RETIREMENT_MONTH
     *   future year →  full months from January to RETIREMENT_MONTH
     *
     * RRSP withdrawals begin the year after retirement, so first_year tracks
     * the patched yor rather than the compile-time constant.
     */
    if (g_retire_year[RALPH_IDX] != RALPH_RETIREMENT_YEAR) {
        int new_yor = g_retire_year[RALPH_IDX];
        int months  = (new_yor == CURRENT_YEAR)
                    ? (RALPH_RETIREMENT_MONTH - CURRENT_MONTH)
                    : (RALPH_RETIREMENT_MONTH - 1);
        ralph_template.yor             = new_yor;
        ralph_template.rys             = (double)RALPH_SALARY * months / 12.0;
        ralph_rrsp_template.first_year = new_yor + 1;
        printf("Retire override: Ralph retires at age %d (%d), rys = %.0f\n",
               new_yor - ralph_template.yob, new_yor, ralph_template.rys);
    }
    if (g_retire_year[SARAH_IDX] != SARAH_RETIREMENT_YEAR) {
        int new_yor = g_retire_year[SARAH_IDX];
        int months  = (new_yor == CURRENT_YEAR)
                    ? (SARAH_RETIREMENT_MONTH - CURRENT_MONTH)
                    : (SARAH_RETIREMENT_MONTH - 1);
        sarah_template.yor             = new_yor;
        sarah_template.rys             = (double)SARAH_SALARY * months / 12.0;
        sarah_rrsp_template.first_year = new_yor + 1;
        printf("Retire override: Sarah retires at age %d (%d), rys = %.0f\n",
               new_yor - sarah_template.yob, new_yor, sarah_template.rys);
    }

    /* --sor year window — computed here (after --retire patches) so it anchors
     * to the effective first retirement year, not the compile-time constant. */
    if (g_sor_mode) {
        int first_retire = (g_retire_year[RALPH_IDX] < g_retire_year[SARAH_IDX])
                         ?  g_retire_year[RALPH_IDX] : g_retire_year[SARAH_IDX];
        g_sor_year_1 = first_retire + 1;
        g_sor_year_2 = first_retire + 2;
    }

    /* When only the CCA-disabled pass runs we do twice as many trials to use
     * the full budget that the CCA-enabled pass would otherwise consume. */
    int n_trials = run_cca_sweep ? N_TRIALS : N_TRIALS * 2;
    printf("Trials per moonbrook year: %d%s\n", n_trials,
           run_cca_sweep ? "" : "  (2x -- CCA sweep disabled)");

    /* Limit threads to reduce CPU heat; 0 means "use all cores". */
    if (MAX_OMP_THREADS > 0) {
        omp_set_num_threads(MAX_OMP_THREADS);
    }

    /* Disable stdout buffering so progress lines appear immediately even when
     * output is captured by make or a terminal emulator on Windows. */
    setbuf(stdout, NULL);

    /* Remove any stale output file before starting so a failed run cannot be
     * mistaken for a fresh result. */
    remove("retirement.csv");

    init_tax_tables();
    compute_asset_indices();   /* calls init_years() once to scan the layout */

    /* Load seed file if provided and switch to seeded refinement mode. */
    if (rrsp_file != NULL) {
        int n = load_rrsp_targets(rrsp_file);
        if (n == 0) {
            printf("Error: failed to load RRSP targets from '%s'. Aborting.\n", rrsp_file);
            return 1;
        }
        g_seed_mode = true;
        printf("Seeded mode: loaded %d years of RRSP targets from '%s'\n", n, rrsp_file);
    }

    if (g_sor_mode) {
        printf("SOR stress mode: investment assets receive %.0f%% return in %d, "
               "%.0f%% return in %d "
               "(return-neutral: +%.2f%% boost in all other investment years)\n",
               SOR_YEAR_1_RETURN * 100.0, g_sor_year_1,
               SOR_YEAR_2_RETURN * 100.0, g_sor_year_2,
               SOR_ANNUAL_COMPENSATION * 100.0);
    }

    if (g_rrif_bucket_enabled) {
        printf("RRIF bucket mode: earmarking %.1f years of min withdrawals "
               "at %.2f%% real return (sd=%.2f%%); refilled only in positive-shock years\n",
               RRIF_BUCKET_YEARS_TARGET,
               RRIF_BUCKET_RETURN * 100.0,
               RRIF_BUCKET_VOLATILITY * 100.0);
    }

    if (g_oas_target_enabled) {
        printf("OAS-target mode: RRSP withdrawal target rebased to indexed "
               "OAS clawback threshold ($%.0f in %d, inflated %.1f%%/yr)\n",
               OAS_CLAWBACK_THRESHOLD, CURRENT_YEAR, INFLATION * 100.0);
    }

    if (g_survivor_mode) {
        const char *dname = (g_survivor_spouse == RALPH_IDX) ? "Ralph" : "Sarah";
        int         dyob  = (g_survivor_spouse == RALPH_IDX)
                          ? ralph_template.yob : sarah_template.yob;
        printf("Survivor mode: %s dies at age %d (%d); "
               "expenses → %.0f%% of joint, assets roll to spouse\n",
               dname, g_survivor_year - dyob, g_survivor_year,
               SURVIVOR_EXPENSE_FACTOR * 100.0);
    }

    /*
     * CCA strategy × moonbrook sale year sweep.
     *
     * Outer loop: two CCA strategies (disabled / enabled).
     * Inner loop: every candidate moonbrook sale year in [MOONBROOK_SALE_YEAR_MIN,
     *             MOONBROOK_SALE_YEAR_MAX].
     *
     * For each (CCA strategy, moonbrook year) combination the full parallel
     * Monte Carlo RRSP-target optimisation is run.  A single global best is
     * tracked across both strategies; retirement.csv always reflects the one
    * scenario — CCA on or off — with the highest success count on the
    * evaluation paths, breaking ties by mean terminal net worth.
     *
     * g_moonbrook_sale_year and g_cca_enabled are set before each parallel block
     * and are read-only within it, so no synchronisation is needed for them.
     */
#define MOONBROOK_YEAR_RANGE (MOONBROOK_SALE_YEAR_MAX - MOONBROOK_SALE_YEAR_MIN + 1)

    double cca_best_networth[2]   = {0.0, 0.0};
    int    cca_best_moonbrook_year[2] = {MOONBROOK_SALE_YEAR_MIN, MOONBROOK_SALE_YEAR_MIN};
    int    cca_best_successes[2]  = {0, 0};

    /* Single global best shared across both CCA passes.
     * Ranking rule: maximize successful evaluation paths first, then mean
     * terminal net worth as the tie-breaker. */
    double best_overall_networth   = 0.0;
    int    best_overall_moonbrook_year = MOONBROOK_SALE_YEAR_MIN;
    int    best_overall_cca        = 0;
    int    best_overall_successes  = 0;
    double best_overall_rep_path_net = 0.0;
    double best_overall_ralph_adj      = 0.0;
    double best_overall_sarah_adj       = 0.0;
    unsigned int best_overall_strategy_seed_base = 0;
    unsigned int best_overall_rep_market_seed    = 0;
    /* Fresh-seed diagnostic carried from the winning (cca, fsy).  Parallel
     * eval panel that re-tests the winning (ralph_adj, sarah_adj) against the
     * SAME market seeds as the replay panel, but with a FRESH strategy_seed
     * per path.  Measures how much of the success rate is driven by the
     * specific ±-walk schedule baked into best_strategy_seed_base vs. the
     * broader class of strategies (same adj + a typical random-walk
     * realisation).  If fresh >> replay, the winner is cherry-picked. */
    int    best_overall_fresh_successes = 0;
    int    best_overall_total_paths     = EVALUATION_PATHS_PER_STRATEGY;

    int cca_max = run_cca_sweep ? 1 : 0;

    for (int cca = 0; cca <= cca_max; cca++) {
        g_cca_enabled = (bool)cca;

        printf("\n\n====================================================\n");
        if (cca)
            printf("=== CCA ENABLED  -- defer rental tax (recapture later) ===\n");
        else
            printf("=== CCA DISABLED -- pay rental tax now (lower recapture) ===\n");
        printf("====================================================\n");

        double moonbrook_year_best[MOONBROOK_YEAR_RANGE];
        for (int k = 0; k < MOONBROOK_YEAR_RANGE; k++) moonbrook_year_best[k] = 0.0;

        /* Per-CCA local best (for the per-strategy summary line). */
        double cca_local_best   = 0.0;
        int    cca_local_fy     = MOONBROOK_SALE_YEAR_MIN;

        for (int fsy = MOONBROOK_SALE_YEAR_MIN; fsy <= MOONBROOK_SALE_YEAR_MAX; fsy++) {
            g_moonbrook_sale_year = fsy;
            out_of_cash_count = 0;
            memset(out_of_cash_hist, 0, sizeof(out_of_cash_hist));

            unsigned int market_path_seed[RETURN_PATHS_PER_STRATEGY];
            unsigned int market_seed_gen = 0x9e3779b9u
                                         ^ (unsigned int)fsy
                                         ^ ((unsigned int)cca * 0x85ebca6bu);
            for (int path = 0; path < RETURN_PATHS_PER_STRATEGY; path++) {
                market_path_seed[path] = (unsigned int)local_rand(&market_seed_gen)
                                       ^ ((unsigned int)(path + 1) * 3266489917u);
            }

            printf("\n=== Moonbrook sale year: %d (CCA %s) ===\n",
                   fsy, cca ? "ON" : "OFF");

            double best_networth  = 0.0;
            double best_ralph_adj = 0.0;
            double best_sarah_adj  = 0.0;
            int    best_successes = 0;
            unsigned int best_strategy_seed_base = 0u;

            /*
             * years[] and brackets[] are threadprivate so every trial runs in
             * fully isolated state.  The critical section uses a double-checked
             * pattern: a cheap unsynchronised read filters out most misses; only
             * potential new-bests enter the lock where a second check avoids a
             * stale race.
             */
            time_t last_progress_print = time(NULL);

            #pragma omp parallel default(none) \
                  shared(best_networth, best_ralph_adj, best_sarah_adj, best_successes, \
                      best_strategy_seed_base, \
                       out_of_cash_count, out_of_cash_hist, g_seed_mode, g_sor_mode, \
                       g_sor_year_1, g_sor_year_2, \
                       g_survivor_mode, g_survivor_spouse, g_survivor_year, \
                       best_overall_networth, best_overall_moonbrook_year, \
                       best_overall_cca, best_overall_successes, best_overall_rep_path_net, \
                       cca_local_best, cca_local_fy, cca_best_successes, \
                       fsy, cca, last_progress_print, n_trials, market_path_seed)
            {
                unsigned int seed = (unsigned int)time(NULL)
                                  ^ ((unsigned int)omp_get_thread_num() * 2654435761u);

                #pragma omp for schedule(dynamic, 500)
                for (int trial = 0; trial < n_trials; trial++) {
                    int x = 0, y = 0;
                    if (!g_seed_mode) {
                        x = (local_rand(&seed) % 101) * 1000 - 20000;
                        y = (local_rand(&seed) % 101) * 1000 - 20000;
                    }

                    unsigned int strategy_seed_base = (unsigned int)local_rand(&seed)
                                                    ^ ((unsigned int)(trial + 1) * 2246822519u);
                    int          success_count      = 0;
                    double       total_net          = 0.0;

                    for (int path = 0; path < RETURN_PATHS_PER_STRATEGY; path++) {
                        unsigned int strategy_seed_path = strategy_seed_base;
                        unsigned int market_seed_path  = market_path_seed[path];

                        int fail_year = run_simulation(x, y,
                                                      &strategy_seed_path,
                                                      &market_seed_path);
                        if (fail_year) {
                            #pragma omp atomic
                            out_of_cash_count++;
                            #pragma omp atomic
                            out_of_cash_hist[fail_year - CURRENT_YEAR]++;
                            continue;
                        }

                        double path_net = sum_networth(years, SIMULATED_YEARS - 1);
                        success_count++;
                        total_net += path_net;
                    }

                    if (success_count == 0) {
                        continue;
                    }

                    double curr_net = total_net / (double)success_count;

                    if (success_count > best_successes ||
                        (success_count == best_successes && curr_net > best_networth)) {
                        #pragma omp critical(best_update)
                        {
                            if (success_count > best_successes ||
                                (success_count == best_successes && curr_net > best_networth)) {
                                if (g_seed_mode) {
                                    printf("%7d: New best optimization mean networth = %16.0f  [%d/%d paths, seeded, moonbrook %d]\n",
                                           trial, curr_net, success_count,
                                           RETURN_PATHS_PER_STRATEGY, fsy);
                                } else {
                                    printf("%7d %7d %7d: New best optimization mean networth = %16.0f  [%d/%d paths, moonbrook %d]\n",
                                           trial, x, y, curr_net, success_count,
                                           RETURN_PATHS_PER_STRATEGY, fsy);
                                }

                                best_networth  = curr_net;
                                best_ralph_adj = x;
                                best_sarah_adj  = y;
                                best_successes = success_count;
                                best_strategy_seed_base = strategy_seed_base;
                            }
                        }
                    }

                    /* Rate-limit "still running" to at most once every 10 seconds. */
                    if (trial % (n_trials / 1000) == 0) {
                        #pragma omp critical(progress)
                        {
                            time_t now = time(NULL);
                            if (now - last_progress_print >= 10) {
                                printf("%7d still running... out of cash %d\n",
                                       trial, out_of_cash_count);
                                last_progress_print = now;
                            }
                        }
                    }
                }
            }

            /* ----------------------------------------------------------------
             * Evaluation pass — re-test the optimisation winner against
             * EVALUATION_PATHS_PER_STRATEGY fresh market seeds to get an
             * honest mean terminal net worth and success rate.  Because only
             * one candidate is evaluated per fsy (vs n_trials candidates
             * during the search), this pass's cost is negligible — the total
             * evaluation work is CCA_passes × fsy_range × EVAL_PATHS, dwarfed
             * by the optimisation loop.  Its numbers drive the cross-fsy
             * ranking, the per-CCA summary, and the final CSV replay.
             *
             * Strategy seed is frozen at the winner's seed (so every eval
             * path runs the SAME strategy including its year-to-year ±15K
             * target-withdrawal random walk).  Only the market seeds vary —
             * which is exactly the question "how does this strategy perform
             * under different market scenarios?".
             * -------------------------------------------------------------- */
            double       eval_networth          = 0.0;
            int          eval_successes         = 0;
            double       eval_rep_path_net      = 0.0;
            unsigned int eval_rep_market_seed   = 0;

            /* Fresh-seed diagnostic: same EVAL market seeds, fresh strategy
             * seed per path.  Measures the overfitting contribution of the
             * winning random-walk realisation (see banner text at end of
             * main() for interpretation). */
            double       eval_fresh_networth    = 0.0;
            int          eval_fresh_successes   = 0;

            if (best_successes > 0) {
                /* Deterministic but distinct-from-optimisation seed stream
                 * (the 0xcafef00d tag distinguishes eval seeds from the
                 * optimisation market_path_seed[] stream above). */
                unsigned int eval_seed_gen = 0x9e3779b9u
                                           ^ ((unsigned int)fsy * 0xa24baed4u)
                                           ^ ((unsigned int)cca * 0x3f8b0b1du)
                                           ^ 0xcafef00du;

                /* Pre-compute market seeds so the replay panel and the
                 * fresh-seed diagnostic panel see the SAME market scenarios.
                 * That's what makes the comparison apples-to-apples: the only
                 * thing that differs between panels is the strategy_seed. */
                unsigned int eval_market_seed[EVALUATION_PATHS_PER_STRATEGY];
                for (int p = 0; p < EVALUATION_PATHS_PER_STRATEGY; p++) {
                    eval_market_seed[p] = (unsigned int)local_rand(&eval_seed_gen)
                                        ^ ((unsigned int)(p + 1) * 3266489917u);
                }

                /* -------- Panel A: replay (strategy_seed frozen) -------- */
                double       eval_path_net[EVALUATION_PATHS_PER_STRATEGY];
                unsigned int eval_used_seeds[EVALUATION_PATHS_PER_STRATEGY];
                double       eval_total = 0.0;

                for (int p = 0; p < EVALUATION_PATHS_PER_STRATEGY; p++) {
                    unsigned int strat_seed = best_strategy_seed_base;
                    unsigned int mkt_seed   = eval_market_seed[p];

                    if (!run_simulation(best_ralph_adj, best_sarah_adj,
                                        &strat_seed, &mkt_seed)) {
                        double path_net = sum_networth(years, SIMULATED_YEARS - 1);
                        eval_path_net[eval_successes]   = path_net;
                        /* Store the INITIAL seed (not the mutated one) so the
                         * final CSV replay regenerates the same market path. */
                        eval_used_seeds[eval_successes] = eval_market_seed[p];
                        eval_successes++;
                        eval_total += path_net;
                    }
                }

                if (eval_successes > 0) {
                    eval_networth = eval_total / (double)eval_successes;
                    /* Representative path: the one whose terminal NW is closest
                     * to the eval-pass mean — shown in retirement.csv so the
                     * reader can see a "typical" path rather than a cherry-pick. */
                    double best_delta    = fabs(eval_path_net[0] - eval_networth);
                    eval_rep_market_seed = eval_used_seeds[0];
                    eval_rep_path_net    = eval_path_net[0];
                    for (int s = 1; s < eval_successes; s++) {
                        double d = fabs(eval_path_net[s] - eval_networth);
                        if (d < best_delta) {
                            best_delta           = d;
                            eval_rep_market_seed = eval_used_seeds[s];
                            eval_rep_path_net    = eval_path_net[s];
                        }
                    }
                }

                /* -------- Panel B: fresh (new strategy_seed per path) --------
                 * Tests the same (ralph_adj, sarah_adj) rule against the same
                 * markets but with a different random-walk realisation each
                 * time.  If success rate is comparable to Panel A, the
                 * winning strategy_seed is not meaningfully cherry-picked;
                 * if much higher, it is.  We don't update the ranking with
                 * these numbers — we only print them as a diagnostic. */
                unsigned int fresh_seed_gen = eval_seed_gen ^ 0xdeadbeefu;
                double       eval_fresh_total = 0.0;
                for (int p = 0; p < EVALUATION_PATHS_PER_STRATEGY; p++) {
                    unsigned int strat_seed = (unsigned int)local_rand(&fresh_seed_gen);
                    unsigned int mkt_seed   = eval_market_seed[p];
                    if (!run_simulation(best_ralph_adj, best_sarah_adj,
                                        &strat_seed, &mkt_seed)) {
                        double path_net = sum_networth(years, SIMULATED_YEARS - 1);
                        eval_fresh_successes++;
                        eval_fresh_total += path_net;
                    }
                }
                if (eval_fresh_successes > 0) {
                    eval_fresh_networth = eval_fresh_total / (double)eval_fresh_successes;
                }
            }

            /* Cross-fsy comparison uses the honest eval-pass mean, not the
             * noisier optimisation mean. */
            moonbrook_year_best[fsy - MOONBROOK_SALE_YEAR_MIN] = eval_networth;

            /* Per-CCA best tracking (drives the "Best moonbrook year" summary). */
            if (eval_successes > 0 &&
                (eval_successes > cca_best_successes[cca] ||
                 (eval_successes == cca_best_successes[cca] && eval_networth > cca_local_best))) {
                cca_local_best          = eval_networth;
                cca_local_fy            = fsy;
                cca_best_successes[cca] = eval_successes;
            }

            /* Global best across both CCA passes — also eval-driven. */
            if (eval_successes > 0 &&
                (eval_successes > best_overall_successes ||
                 (eval_successes == best_overall_successes && eval_networth > best_overall_networth))) {
                best_overall_networth             = eval_networth;
                best_overall_moonbrook_year           = fsy;
                best_overall_cca                  = cca;
                best_overall_successes            = eval_successes;
                best_overall_rep_path_net         = eval_rep_path_net;
                best_overall_ralph_adj            = best_ralph_adj;
                best_overall_sarah_adj             = best_sarah_adj;
                best_overall_strategy_seed_base   = best_strategy_seed_base;
                best_overall_rep_market_seed      = eval_rep_market_seed;
                /* Carry the fresh-seed diagnostic of the winning fsy/cca
                 * forward for the final banner.  (We don't need the fresh
                 * networth — the banner reports success rate only.) */
                best_overall_fresh_successes      = eval_fresh_successes;
                (void)eval_fresh_networth;
            }

            if (best_networth == 0.0) {
                printf("Moonbrook year %d: no successful outcomes.\n", fsy);
                report_cash_exhaustion(out_of_cash_hist,
                                       n_trials * RETURN_PATHS_PER_STRATEGY);
            } else {
                /* Show replay + fresh side-by-side so the overfitting gap is
                 * visible per-fsy.  fresh = same markets, new strategy_seed;
                 * a big gap means the winner's random-walk schedule is
                 * cherry-picked. */
                printf("Moonbrook year %d best: $%.0f avg terminal NW  (eval %d/%d paths; fresh %d/%d paths; opt $%.0f / %d/%d paths; Ralph adj: $%.0f  Sarah adj: $%.0f)%s\n",
                       fsy,
                       eval_networth, eval_successes, EVALUATION_PATHS_PER_STRATEGY,
                       eval_fresh_successes, EVALUATION_PATHS_PER_STRATEGY,
                       best_networth, best_successes, RETURN_PATHS_PER_STRATEGY,
                       best_ralph_adj, best_sarah_adj,
                       fsy == best_overall_moonbrook_year && cca == best_overall_cca
                           ? "  [GLOBAL BEST so far]" : "");
            }
        }

        /* ---- Per-CCA summary table ---- */
        printf("\n=== Moonbrook sale year sweep (CCA %s) -- summary ===\n",
               cca ? "ENABLED" : "DISABLED");
        for (int fsy = MOONBROOK_SALE_YEAR_MIN; fsy <= MOONBROOK_SALE_YEAR_MAX; fsy++) {
            double nw = moonbrook_year_best[fsy - MOONBROOK_SALE_YEAR_MIN];
            if (nw > 0.0)
                printf("  %d:  $%.0f avg%s\n", fsy, nw,
                       (fsy == best_overall_moonbrook_year && cca == best_overall_cca)
                           ? "  <-- GLOBAL BEST" : "");
        }

        if (cca_local_best == 0.0) {
            printf("\n*** No successful simulation outcomes found. ***\n");
             printf("    All %d strategy evaluations (%d paths each) exhausted cash before end of simulation.\n",
                 n_trials, RETURN_PATHS_PER_STRATEGY);
        } else {
             /* Per-CCA "Best moonbrook year" summary uses the eval-pass numbers
              * (cca_best_successes[cca] out of EVALUATION_PATHS_PER_STRATEGY)
              * so it reflects the honest re-test, not the 4-path optimisation
              * mean. */
             printf("\nBest moonbrook year (CCA %s): %d  (%d/%d eval paths, avg net worth: $%.0f)\n",
                 cca ? "ON" : "OFF", cca_local_fy,
                 cca_best_successes[cca], EVALUATION_PATHS_PER_STRATEGY,
                 cca_local_best);
        }

        cca_best_networth[cca]   = cca_local_best;
        cca_best_moonbrook_year[cca] = cca_local_fy;
    }

    /* ---- Final CCA strategy comparison (only when both passes ran) ---- */
    if (run_cca_sweep) {
        printf("\n\n====================================================\n");
        printf("=== CCA Strategy Comparison                     ===\n");
        printf("====================================================\n");
        printf("%-32s  %14s  %s\n", "Strategy", "Best NW ($)", "Moonbrook Year");
        printf("%-32s  %14s  %s\n", "--------", "-----------", "----------");
        printf("%-32s  %14.0f  %d\n", "CCA DISABLED (pay tax now)",
               cca_best_networth[0], cca_best_moonbrook_year[0]);
        printf("%-32s  %14.0f  %d\n", "CCA ENABLED  (defer/recapture)",
               cca_best_networth[1], cca_best_moonbrook_year[1]);
        if (cca_best_networth[0] > 0.0 && cca_best_networth[1] > 0.0) {
            double diff = cca_best_networth[1] - cca_best_networth[0];
            printf("\nDifference (CCA ON - CCA OFF): %+.0f  (%s is better)\n",
                   diff, diff > 0.0 ? "CCA ENABLED" : "CCA DISABLED");
        }
    }

    /* ----------------------------------------------------------------------
     * Final CSV replay.
     *
     * We replaced the old in-loop write_simulation_to_file() with a single
     * end-of-run write: replay the global-best strategy (best_overall_*)
     * against its eval-pass representative market seed, then emit the CSV.
     *
     * Guarantees:
     *   - The numbers in the CSV summary (successes / mean NW / representative
     *     path NW) are all eval-pass numbers, not the noisier optimisation
     *     pass.
     *   - g_cca_enabled / g_moonbrook_sale_year are restored to the winning
     *     scenario before the replay so the simulation behaves identically
     *     to the run that discovered the winner.
     *   - run_simulation() calls reset_tax_brackets() + init_years() itself,
     *     so no extra setup is needed beyond the globals above.
     * ------------------------------------------------------------------- */
    if (best_overall_networth > 0.0) {
        g_cca_enabled     = (bool)best_overall_cca;
        g_moonbrook_sale_year = best_overall_moonbrook_year;

        unsigned int replay_strategy_seed = best_overall_strategy_seed_base;
        unsigned int replay_market_seed   = best_overall_rep_market_seed;

        if (!run_simulation(best_overall_ralph_adj,
                            best_overall_sarah_adj,
                            &replay_strategy_seed,
                            &replay_market_seed)) {
            /* Compute P10/P50/P90 net-worth bands before writing the CSV so
             * everything is flushed in a single file open. */
            double *p10 = calloc(SIMULATED_YEARS, sizeof(double));
            double *p50 = calloc(SIMULATED_YEARS, sizeof(double));
            double *p90 = calloc(SIMULATED_YEARS, sizeof(double));
            if (p10 && p50 && p90) {
                run_percentile_pass(best_overall_ralph_adj,
                                    best_overall_sarah_adj,
                                    best_overall_strategy_seed_base,
                                    p10, p50, p90, SIMULATED_YEARS);
            }

            write_simulation_to_file(years, SIMULATED_YEARS,
                                     "retirement.csv",
                                     best_overall_successes,
                                     best_overall_total_paths,
                                     best_overall_networth,
                                     best_overall_rep_path_net,
                                     g_survivor_mode,
                                     g_survivor_spouse,
                                     g_survivor_year,
                                     p10, p50, p90);
            free(p10); free(p50); free(p90);
        } else {
            printf("\n*** Warning: final replay of best strategy failed "
                   "(out of cash).  No CSV written. ***\n");
        }

        /* Classify the eval-pass success ratio against the same thresholds
         * used by the xlsx/docx outputs so the console summary tells the
         * same story.  95% is the Bengen/Trinity planning benchmark; below
         * 70% the strategy needs revisiting. */
        double success_rate = (best_overall_total_paths > 0)
            ? (double)best_overall_successes / (double)best_overall_total_paths
            : 0.0;
        const char *success_label =
            (success_rate >= 0.95) ? "STRONG"   :
            (success_rate >= 0.85) ? "GOOD"     :
            (success_rate >= 0.70) ? "MARGINAL" :
            (success_rate >= 0.50) ? "WEAK"     : "AT RISK";

        /* Fresh-seed diagnostic: if the gap between replay and fresh is
         * large, the winning strategy_seed's random-walk realisation is
         * cherry-picked rather than broadly good.  Useful for judging how
         * much of the score reflects genuine plan risk vs. overfitting. */
        double fresh_rate = (best_overall_total_paths > 0)
            ? (double)best_overall_fresh_successes / (double)best_overall_total_paths
            : 0.0;
        double gap_pp = (fresh_rate - success_rate) * 100.0;
        const char *gap_verdict =
            (fabs(gap_pp) <  5.0) ? "minimal (rule, not seed, drives score)"      :
            (gap_pp      < -5.0)  ? "negative (winner beats the rule's average)"  :
            (gap_pp      < 10.0)  ? "modest (some seed luck)"                     :
            (gap_pp      < 20.0)  ? "notable (meaningful overfitting)"            :
                                    "large (winner is heavily cherry-picked)";

        printf("\n======================================================\n");
        printf("  Strategy Robustness: %d / %d market paths  (%.0f%%)  [%s]\n",
               best_overall_successes, best_overall_total_paths,
               success_rate * 100.0, success_label);
        printf("  Fresh-seed diagnostic: %d / %d paths  (%.0f%%)  "
               "|  gap %+.0fpp -- %s\n",
               best_overall_fresh_successes, best_overall_total_paths,
               fresh_rate * 100.0, gap_pp, gap_verdict);
        printf("======================================================\n");
        printf("Best overall: CCA %s, moonbrook year %d  "
               "(avg net worth: $%.0f, representative path: $%.0f)\n",
               best_overall_cca ? "ENABLED" : "DISABLED",
               best_overall_moonbrook_year,
               best_overall_networth, best_overall_rep_path_net);
        printf("Output: retirement.csv\n");
    } else {
        printf("\n*** No successful simulation outcomes found. ***\n");
        printf("    Check spending assumptions, income sources, and starting balances.\n\n");
    }

    return 0;
}
