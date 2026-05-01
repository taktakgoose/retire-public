/*
 * output.c - CSV output functions for the retirement simulation.
 */

#include "output.h"
#include "tax.h"
#include "params.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * NET WORTH
 * ====================================================================== */

/*
 * Gross net worth: all account balances at face value plus property net equity
 * (value minus mortgage), with no tax deductions.  Represents the raw balance-
 * sheet total — what you'd see if you added up every account statement today.
 * ASSET_OTHER_INCOME assets (CPP, OAS, rent streams) are excluded because their
 * .value field holds an annual income amount, not a capital balance.
 */
static double sum_gross_networth(const year_t *years, int i)
{
    double sum = years[i].cash;

    for (int j = 0; j < MAX_ASSETS; j++) {
        double v = years[i].assets[j].value;
        if (v == 0.0) continue;
        switch (years[i].assets[j].type) {
        case ASSET_RRSP:
        case ASSET_LOCKED_IN:
        case ASSET_TFSA:
        case ASSET_NONREG:
            sum += v;
            break;
        case ASSET_PROPERTY:
            sum += v - years[i].assets[j].mortgage_balance;
            break;
        case ASSET_OTHER_INCOME:
        default:
            break;
        }
    }

    return sum;
}

double sum_networth(const year_t *years, int i)
{
    double sum = years[i].cash;

    /*
     * Build one terminal tax return per spouse.
     *
     * RRSP, LIF/DCPP, and CCA recapture are ordinary income.  Rental-property
     * and non-registered deemed dispositions contribute capital gains.
     * Aggregate each spouse's terminal ordinary income and raw capital gains,
     * then apply the capital-gain inclusion rule once per spouse before a
     * single progressive-tax calculation.  Taxing each component separately
     * understates terminal tax because it fails to stack income into higher
     * brackets.
     */
    double ordinary_income[2]   = { 0.0, 0.0 };
    double capital_gains[2]     = { 0.0, 0.0 };

    for (int j = 0; j < MAX_ASSETS; j++) {
        int    s = years[i].assets[j].spouse_idx;  /* RALPH_IDX or SARAH_IDX */
        double v = years[i].assets[j].value;
        if (v == 0.0) continue;

        switch (years[i].assets[j].type) {

        case ASSET_PROPERTY: {
            /*
             * Net equity after outstanding mortgage obligation.
             *
             * Two distinct sub-cases:
             *
             * tax_free == true  →  principal residence (Robertson Court).
             *   The principal-residence exemption (PRE) shields the entire
             *   capital gain on deemed disposition.  No tax owed; carry the
             *   full net equity into the estate.
             *
             * tax_free == false →  rental property (Moonbrook Street).
             *   Deemed disposition triggers two taxable events:
             *     1. Capital gain  = FMV − ACB          (at inclusion rate)
             *     2. CCA recapture = min(FMV, cost) − UCC  (100 % ordinary income,
             *                        stacked with RRSP/LIF on the terminal T1)
             */
            double net_equity = v - years[i].assets[j].mortgage_balance;
            if (net_equity <= 0.0) break;
            sum += net_equity;

            if (years[i].assets[j].tax_free) break;   /* PRE — no deemed-disposition tax */

            double cap_gain = v - years[i].assets[j].cost;
            if (cap_gain > 0.0)
                capital_gains[s] += cap_gain;

            double proceeds_capped = (v < years[i].assets[j].cost)
                                   ? v : years[i].assets[j].cost;
            double recapture = proceeds_capped - years[i].assets[j].ucc;
            if (recapture > 0.0)
                ordinary_income[s] += recapture;
            break;
        }

        case ASSET_LOCKED_IN:
            /*
             * LIF / DCPP: full balance is taxable ordinary income on deemed
             * collapse — identical treatment to RRSP.  Fold into the per-spouse
             * registered-income pool so progressive brackets stack correctly.
             */
            sum += v;
            ordinary_income[s] += v;
            break;

        case ASSET_TFSA:
            /* Passes to the beneficiary tax-free; no deemed disposition. */
            sum += v;
            break;

        case ASSET_NONREG: {
            /*
             * Deemed disposition at FMV.
             * Capital gain = FMV − ACB, included at the applicable rate.
             */
            double gain = v - years[i].assets[j].cost;
            sum += v;
            if (gain > 0.0)
                capital_gains[s] += gain;
            break;
        }

        case ASSET_RRSP:
            /*
             * Full balance deemed withdrawn on death → 100 % ordinary income.
             * Fold into the per-spouse registered pool for combined tax calc.
             */
            sum += v;
            ordinary_income[s] += v;
            break;

        default:
            break;
        }
    }

    /* One progressive-tax deduction per spouse covering all stacked ordinary
     * income and included capital gains on the terminal return. */
    for (int s = 0; s < 2; s++) {
        double terminal_taxable_income = ordinary_income[s];
        if (capital_gains[s] > 0.0)
            terminal_taxable_income += calc_capital_gain_inclusion(capital_gains[s]);

        if (terminal_taxable_income > 0.0)
            sum -= taxes_owing_on_income(terminal_taxable_income);
    }

    return sum;
}

/* =========================================================================
 * WRITE HELPERS
 * ====================================================================== */

/* Write withdrawal amounts for all assets belonging to one spouse. */
static void write_asset_debits_to_file(FILE *file, const year_t *years,
                                       int i, int spouse_idx)
{
    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[i].assets[j].spouse_idx != spouse_idx) continue;

        if (years[i].assets[j].type == ASSET_OTHER_INCOME) {
            float income_value = 0;

            if ((years[i].assets[j].owner->age >= years[i].assets[j].min_age) &&
                (years[i].current_year <= years[i].assets[j].last_year) &&
                (years[i].current_year != years[i].assets[j].no_year)) {
                income_value = (float)years[i].assets[j].value;
            }

            fprintf(file, "%.0f;", income_value);
        } else if (years[i].assets[j].type == ASSET_PROPERTY &&
                   years[i].assets[j].withdrawal_amount == 0.0) {
            /* No sale this year — show mortgage principal as a negative so the
             * cash outflow is visible in the withdrawals columns.  Guard
             * against -0 when the mortgage is already paid off. */
            double principal = years[i].assets[j].cy_mortgage_principal;
            fprintf(file, "%.0f;", principal > 0.0 ? -principal : 0.0);
        } else {
            fprintf(file, "%.0f;", years[i].assets[j].withdrawal_amount);
        }
    }
}

/* Return the sum of all non-income withdrawal amounts for year i. */
static double sum_asset_debits(const year_t *years, int i)
{
    double sum = 0;

    for (int j = 0; j < MAX_ASSETS; j++) {
        if (years[i].assets[j].type == ASSET_OTHER_INCOME) continue;
        sum += years[i].assets[j].withdrawal_amount;
    }

    return sum;
}

static void write_asset_to_file(FILE *file, const year_t *years, int i, int j)
{
    const asset_t *a = &years[i].assets[j];

    /* Income-flow assets (OAS, CPP, Moonbrook Rent, Polaron) keep their .value
     * growing with inflation every year even after the asset's active window
     * closes.  Zero out value and growth for inactive years so the asset
     * block doesn't show phantom income after last_year or before min_age. */
    double display_value  = a->value;
    double display_growth = a->cy_realized_growth;
    if (a->type == ASSET_OTHER_INCOME) {
        bool active = (a->owner->age >= a->min_age)
                   && (years[i].current_year <= a->last_year)
                   && (a->no_year == 0 || years[i].current_year != a->no_year);
        if (!active) {
            display_value  = 0.0;
            display_growth = 0.0;
        }
    }

    fprintf(file, "%s;%.0f;%.0f;%.0f;%.4f;",
        a->name,
        display_value,
        a->withdrawal_amount,
        a->room,
        display_growth);

    /* Mortgage columns — ASSET_PROPERTY only. */
    if (a->type == ASSET_PROPERTY) {
        fprintf(file, "%.0f;%.0f;%.0f;",
            a->mortgage_balance,
            a->cy_mortgage_interest,
            a->cy_mortgage_principal);
    }
}

static void write_spouse_to_file(FILE *file, const year_t *years, int i, int j)
{
    double total_income = years[i].spouse[j].current_year_income
                        + years[i].spouse[j].current_year_capital_gains;
    double tax_rate = (total_income > 0)
        ? years[i].spouse[j].cy_taxes / total_income
        : 0.0;

    fprintf(file, "%d;%.0f;%.0f;%.0f;%.0f;%.0f;%.0f;%.0f;%.3f;%.0f;",
        years[i].spouse[j].age,
        years[i].spouse[j].salary,
        years[i].spouse[j].expenses,
        total_income,
        years[i].spouse[j].current_year_income,
        years[i].spouse[j].current_year_capital_gains,
        years[i].spouse[j].cy_take_home,
        years[i].spouse[j].cy_taxes,
        tax_rate,
        years[i].spouse[j].cy_harvest_sell_value);
}

static void write_year_to_file(FILE *file, const year_t *years, int i)
{
    double networth = sum_networth(years, i);
    double debits   = sum_asset_debits(years, i);

    fprintf(file, "%d;%.0f;%.0f;%d;%.0f;",
        years[i].current_year,
        years[i].cash,
        debits,
        years[i].spouse[SARAH_IDX].age,
        -(years[i].spouse[RALPH_IDX].expenses + years[i].spouse[SARAH_IDX].expenses
          + years[i].cy_senior_living));

    write_asset_debits_to_file(file, years, i, RALPH_IDX);
    write_asset_debits_to_file(file, years, i, SARAH_IDX);

    fprintf(file, "%.0f;%.0f;", sum_gross_networth(years, i), networth);

    write_spouse_to_file(file, years, i, RALPH_IDX);
    write_spouse_to_file(file, years, i, SARAH_IDX);

    for (int j = 0; j < MAX_ASSETS; j++) {
        write_asset_to_file(file, years, i, j);
    }

    fprintf(file, "\n");
}

static void write_header_to_file(FILE *file, const year_t *years)
{
    fprintf(file, "Year;Cash;Total Debits;Sarah Age;Expenses;");
    fprintf(file, "Ralph RRSP;DCPP;NonReg;TFSA;Robertson;Moonbrook;Rent;OAS;CPP;Polaron;");
    fprintf(file, "Sarah RRSP;DCPP;NonReg;TFSA;Robertson;Moonbrook;Rent;OAS;CPP;Polaron;");
    fprintf(file, "Gross;After-Tax;");
    fprintf(file, "Ralph Age;Ralph Salary;Ralph Expenses;Ralph Income;Ralph Reg;Ralph Capital;"
                  "Ralph Take Home;Ralph Taxes;Ralph Tax Rate;Ralph Harvest;");
    fprintf(file, "Sarah Age;Sarah Salary;Sarah Expenses;Sarah Income;Sarah Reg;Sarah Capital;"
                  "Sarah Take Home;Sarah Taxes;Sarah Tax Rate;Sarah Harvest;");

    for (int j = 0; j < MAX_ASSETS; j++) {
        fprintf(file, "Asset;Value;Debit;Room;Growth;");
        if (years[0].assets[j].type == ASSET_PROPERTY) {
            fprintf(file, "Mtg Bal;Mtg Int;Mtg Principal;");
        }
    }

    fprintf(file, "\n");
}

static void write_summary_to_file(FILE *file, const year_t *years, int num_years,
                                  int representative_successes,
                                  int representative_total_paths,
                                  double representative_mean_networth,
                                  double representative_path_networth)
{
    double ralph_income    = 0, ralph_cg       = 0;
    double ralph_take_home = 0, ralph_taxes     = 0, ralph_avg_tax = 0;
    double sarah_income     = 0, sarah_cg         = 0;
    double sarah_take_home  = 0, sarah_taxes      = 0, sarah_avg_tax  = 0;
    double networth     = 0;
    int    final_year_i = 1;

    for (int i = 1; i < num_years; i++) {
        double c_total = years[i].spouse[RALPH_IDX].current_year_income
                       + years[i].spouse[RALPH_IDX].current_year_capital_gains;
        double l_total = years[i].spouse[SARAH_IDX].current_year_income
                       + years[i].spouse[SARAH_IDX].current_year_capital_gains;

        ralph_income    += years[i].spouse[RALPH_IDX].current_year_income;
        ralph_cg        += years[i].spouse[RALPH_IDX].current_year_capital_gains;
        ralph_take_home += years[i].spouse[RALPH_IDX].cy_take_home;
        ralph_taxes     += years[i].spouse[RALPH_IDX].cy_taxes;
        ralph_avg_tax   += (c_total > 0) ? years[i].spouse[RALPH_IDX].cy_taxes / c_total : 0.0;

        sarah_income    += years[i].spouse[SARAH_IDX].current_year_income;
        sarah_cg        += years[i].spouse[SARAH_IDX].current_year_capital_gains;
        sarah_take_home += years[i].spouse[SARAH_IDX].cy_take_home;
        sarah_taxes     += years[i].spouse[SARAH_IDX].cy_taxes;
        sarah_avg_tax   += (l_total > 0) ? years[i].spouse[SARAH_IDX].cy_taxes / l_total : 0.0;

        networth     = sum_networth(years, i);
        final_year_i = i;
    }

    /* Deflate the final nominal net worth back to CURRENT_YEAR purchasing
     * power by reversing compounded inflation over the elapsed years. */
    int    elapsed_years    = years[final_year_i].current_year - CURRENT_YEAR;
    double deflation_factor = pow(1.0 + INFLATION, (double)elapsed_years);
    double real_networth    = networth / deflation_factor;

    int sim_years = num_years - 1;
    ralph_avg_tax  /= (double)sim_years;
    sarah_avg_tax   /= (double)sim_years;
    double both_avg = (ralph_avg_tax + sarah_avg_tax) / 2.0;

    fprintf(file, "\n");
    fprintf(file, ";Ralph;Sarah;Total;\n");
    fprintf(file, "RRSP Target;%.0f;%.0f;;\n",
        years[0].assets[0].target_withdrawal,
        years[0].assets[1].target_withdrawal);
    fprintf(file, "Reg. Income;%.0f;%.0f;%.0f;\n",
        ralph_income, sarah_income, ralph_income + sarah_income);
    fprintf(file, "Capital Gains;%.0f;%.0f;%.0f;\n",
        ralph_cg, sarah_cg, ralph_cg + sarah_cg);
    fprintf(file, "Take Home;%.0f;%.0f;%.0f;\n",
        ralph_take_home, sarah_take_home, ralph_take_home + sarah_take_home);
    fprintf(file, "Taxes;%.0f;%.0f;%.0f;\n",
        ralph_taxes, sarah_taxes, ralph_taxes + sarah_taxes);
    fprintf(file, "Average Tax Rate;%.4f;%.4f;%.4f;\n",
        ralph_avg_tax, sarah_avg_tax, both_avg);
    fprintf(file, "Networth (Nominal);;;%.0f;\n", networth);
    fprintf(file, "Networth (Real %d$);;;%.0f;\n", CURRENT_YEAR, real_networth);
    fprintf(file, "CSV Selection Rule;;;Highest success count first; mean terminal net worth breaks ties; representative path is the successful evaluation path closest to the winning mean;\n");
    fprintf(file, "Return Paths Per Strategy;;;%d;\n", RETURN_PATHS_PER_STRATEGY);
    fprintf(file, "Winning Strategy Successes;;;%d/%d;\n",
        representative_successes, representative_total_paths);
    fprintf(file, "Winning Strategy Mean Networth;;;%.0f;\n",
        representative_mean_networth);
    fprintf(file, "Representative Path Networth;;;%.0f;\n",
        representative_path_networth);

    /* -------------------------------------------------------------------
     * Realized-return averages per asset class.
     *
     * For each year i>=1 (year 0 is the starting-balance snapshot and its
     * cy_realized_growth is seeded from percent_growth, not realized), walk
     * every asset and accumulate the shock-adjusted rate applied that year.
     * Group by class so the summary shows a single number per market driver.
     *
     * We also accumulate the expected (base) rate over the same asset-years
     * so consumers can display "realized vs expected" side-by-side without
     * re-deriving a weighted expected value from params.h.
     * ------------------------------------------------------------------- */
    double fin_r = 0.0, fin_e = 0.0; int fin_n = 0;
    double pro_r = 0.0, pro_e = 0.0; int pro_n = 0;
    double ren_r = 0.0, ren_e = 0.0; int ren_n = 0;

    /* Per-year min/max for the financial asset class.  We compute the average
     * realized return across all financial assets in each year, then track the
     * best and worst of those per-year averages across the simulation. */
    double fin_min_r =  1.0e9;   /* initialised to ±infinity sentinels */
    double fin_max_r = -1.0e9;

    for (int i = 1; i < num_years; i++) {
        double yr_fin_r = 0.0; int yr_fin_n = 0;
        for (int j = 0; j < MAX_ASSETS; j++) {
            asset_type_e t   = years[i].assets[j].type;
            bool         fin = (t == ASSET_RRSP || t == ASSET_LOCKED_IN ||
                                t == ASSET_TFSA || t == ASSET_NONREG);
            if (fin) {
                fin_r += years[i].assets[j].cy_realized_growth;
                fin_e += years[i].assets[j].percent_growth;
                fin_n++;
                yr_fin_r += years[i].assets[j].cy_realized_growth;
                yr_fin_n++;
            } else if (t == ASSET_PROPERTY) {
                pro_r += years[i].assets[j].cy_realized_growth;
                pro_e += years[i].assets[j].percent_growth;
                pro_n++;
            } else if (years[i].assets[j].is_moonbrook_rent) {
                ren_r += years[i].assets[j].cy_realized_growth;
                ren_e += years[i].assets[j].percent_growth;
                ren_n++;
            }
        }
        if (yr_fin_n > 0) {
            double yr_avg = yr_fin_r / (double)yr_fin_n;
            if (yr_avg < fin_min_r) fin_min_r = yr_avg;
            if (yr_avg > fin_max_r) fin_max_r = yr_avg;
        }
    }

    double avg_fin_r  = fin_n ? fin_r / (double)fin_n : 0.0;
    double avg_fin_e  = fin_n ? fin_e / (double)fin_n : 0.0;
    double avg_pro_r  = pro_n ? pro_r / (double)pro_n : 0.0;
    double avg_pro_e  = pro_n ? pro_e / (double)pro_n : 0.0;
    double avg_ren_r  = ren_n ? ren_r / (double)ren_n : 0.0;
    double avg_ren_e  = ren_n ? ren_e / (double)ren_n : 0.0;

    /* Sentinel guard: if no financial years were seen, reset to zero. */
    if (fin_min_r >  0.5) fin_min_r = 0.0;
    if (fin_max_r < -0.5) fin_max_r = 0.0;

    fprintf(file, "Expected Financial Return;;;%.4f;\n", avg_fin_e);
    fprintf(file, "Realized Financial Return;;;%.4f;\n", avg_fin_r);
    fprintf(file, "Min Financial Return;;;%.4f;\n",      fin_min_r);
    fprintf(file, "Max Financial Return;;;%.4f;\n",      fin_max_r);
    fprintf(file, "Expected Property Return;;;%.4f;\n",  avg_pro_e);
    fprintf(file, "Realized Property Return;;;%.4f;\n",  avg_pro_r);
    fprintf(file, "Expected Rent Return;;;%.4f;\n",      avg_ren_e);
    fprintf(file, "Realized Rent Return;;;%.4f;\n",      avg_ren_r);
}

/* =========================================================================
 * PUBLIC INTERFACE
 * ====================================================================== */

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
                              const double *p90)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        printf("\nError opening file: %s\n", strerror(errno));
        return;
    }

    write_header_to_file(file, years);

    for (int i = 0; i < num_years; i++) {
        write_year_to_file(file, years, i);
    }

    write_summary_to_file(file, years, num_years,
                          representative_successes,
                          representative_total_paths,
                          representative_mean_networth,
                          representative_path_networth);

    /* Survivor scenario metadata — read back by retirement_to_xlsx.py and
     * retirement_to_docx.py to populate the Assumptions tab and milestones. */
    if (survivor_mode) {
        const char *name = (survivor_spouse == RALPH_IDX) ? "ralph" : "sarah";
        fprintf(file, "Survivor;%s;%d;;\n", name, survivor_year);
    } else {
        fprintf(file, "Survivor;none;;;\n");
    }

    /* Percentile confidence bands — written in the same file open so the
     * CSV is written exactly once.  p10/p50/p90 are NULL when the
     * percentile pass was skipped (e.g. out-of-memory). */
    if (p10 && p50 && p90) {
        const double *bands[3] = { p10, p50, p90 };
        const char  *tags[3]   = { "NW_P10", "NW_P50", "NW_P90" };
        for (int b = 0; b < 3; b++) {
            fprintf(file, "%s", tags[b]);
            for (int i = 0; i < num_years; i++)
                fprintf(file, ";%.0f", bands[b][i]);
            fprintf(file, ";\n");
        }
    }

    fclose(file);
}

