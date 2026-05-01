/*
 * types.h - Core data structures for the retirement simulation.
 */

#pragma once

#include <stdbool.h>
#include "params.h"

/* =========================================================================
 * SPOUSE
 * ====================================================================== */

typedef struct {
    char*  name;
    int    yob;               /* Year of birth */
    int    yor;               /* Year of retirement */
    int    age;               /* Current simulation age */
    int    id;                /* RALPH_IDX or SARAH_IDX */
    double salary;            /* Current (inflation-adjusted) salary */
    double rys;               /* Salary for remaining months of the retirement year */
    double ytd_income;        /* Salary already earned Jan–(CURRENT_MONTH-1); added to
                               * current_year_income in year 0 for tax bracket accuracy
                               * but NOT to cash (already reflected in starting balance). */
    double expenses;          /* Annual living expenses */
    double current_year_income;
    double current_year_capital_gains;
    double cy_pension_income;   /* Eligible pension income for the $2000 credit (RRSP/RRIF + LIF) */
    double cy_oas_income;       /* OAS received this year — used for clawback calculation   */
    double cy_cpp_income;       /* CPP received this year — used for pension assignment      */
    double cy_take_home;
    double cy_taxes;
    double cy_harvest_sell_value;     /* Market value of non-reg sold & repurchased for ACB reset */
    double cy_harvest_capital_gains;  /* Capital gains realised by the harvest (notional — no cash
                                       * received; used to claw back phantom cash from cy_take_home) */
} spouse_t;

/* =========================================================================
 * ASSET
 * ====================================================================== */

typedef enum {
    ASSET_OTHER_INCOME = 1,
    ASSET_PROPERTY,
    ASSET_LOCKED_IN,
    ASSET_RRSP,
    ASSET_TFSA,
    ASSET_NONREG,
} asset_type_e;

typedef struct {
    char*         name;
    int           spouse_idx;
    spouse_t*     owner;
    asset_type_e  type;
    bool          tax_free;
    bool          capital_gains;
    bool          accept_deposits;
    bool          is_oas;             /* True for OAS income assets (used for clawback tracking)  */
    bool          is_cpp;             /* True for CPP income assets (used for pension assignment)  */
    bool          is_moonbrook_rent;      /* True for Moonbrook rent income assets (profit grows as mortgage
                                       * interest declines each year with paydown)                 */
    bool          use_spouse_rrif_age;/* Use the younger spouse's age for RRIF minimum withdrawal  */
    int           min_age;
    double        cost;
    double        value;
    double        withdrawal_amount;
    double        after_tax_amount;
    double        tax_amount;
    double        room;
    double        flat_room_growth;
    int           first_year;
    int           last_year;
    int           sale_year;
    int           no_year;             /* Year to skip income (e.g. property sale year) */
    double        percent_growth;
    double        target_withdrawal;
    double        min_percent_withdrawal;
    double        max_percent_withdrawal;
    /* Mortgage fields — only used for ASSET_PROPERTY with an outstanding loan. */
    double        mortgage_balance;           /* Remaining principal                          */
    double        mortgage_monthly_payment;   /* Fixed monthly payment amount                 */
    double        mortgage_interest_rate;     /* Annual interest rate (e.g. 0.055)            */
    /* Populated by subtract_mortgage_payments() each simulation year. */
    double        cy_mortgage_interest;      /* Interest portion paid this year              */
    double        cy_mortgage_principal;     /* Principal portion repaid this year           */
    /* Populated by grow_assets() each simulation year.  Equals percent_growth
     * plus any Monte-Carlo shock (financial / property / rent) or SOR override
     * applied that year.  Written to the CSV so the xlsx shows the realized
     * per-asset return rather than the static expected rate. */
    double        cy_realized_growth;        /* Shock-adjusted growth rate this year         */
    /* CCA recapture — only relevant for depreciable rental property. */
    double        ucc;                        /* Undepreciated Capital Cost (per-spouse share) */
} asset_t;

/* =========================================================================
 * YEAR STATE
 * ====================================================================== */

typedef struct {
    int      current_year;
    spouse_t spouse[2];
    double   cash;
    double   cy_senior_living;   /* Annual senior-living / rent cost (post-Robertson sale) */
    /*
     * Conservative-allocation portion of each spouse's RRSP/RRIF, a subset of
     * the matching ASSET_RRSP.value (not a separate account — a RRIF can hold
     * cash/GIC and equity under one tax wrapper).  Populated only when the
     * --bucket strategy is enabled; otherwise remains 0 and all growth &
     * withdrawal logic falls back to the pooled-financial path.  Indexed by
     * RALPH_IDX / SARAH_IDX.
     */
    double   rrif_bucket[2];
    asset_t  assets[MAX_ASSETS];
} year_t;
