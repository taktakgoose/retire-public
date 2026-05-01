/*
 * tables.c - LIF and RRIF regulatory lookup tables.
 */

#include "tables.h"

/* =========================================================================
 * LIF MAXIMUM WITHDRAWAL RATES
 * ====================================================================== */

typedef struct {
    int    age;
    double rate;
} lif_rate_t;

/* Maximum annual LIF withdrawal rates by age (Ontario). */
static const lif_rate_t lif_rates[] = {
    { .age = 55, .rate = 0.065070 },
    { .age = 56, .rate = 0.065659 },
    { .age = 57, .rate = 0.066295 },
    { .age = 58, .rate = 0.066983 },
    { .age = 59, .rate = 0.067729 },
    { .age = 60, .rate = 0.068537 },
    { .age = 61, .rate = 0.069415 },
    { .age = 62, .rate = 0.070370 },
    { .age = 63, .rate = 0.071412 },
    { .age = 64, .rate = 0.072551 },
    { .age = 65, .rate = 0.073799 },
    { .age = 66, .rate = 0.075169 },
    { .age = 67, .rate = 0.076678 },
    { .age = 68, .rate = 0.078345 },
    { .age = 69, .rate = 0.080193 },
    { .age = 70, .rate = 0.082250 },
    { .age = 71, .rate = 0.084548 },
    { .age = 72, .rate = 0.087129 },
    { .age = 73, .rate = 0.090042 },
    { .age = 74, .rate = 0.093351 },
    { .age = 75, .rate = 0.097135 },
    { .age = 76, .rate = 0.101495 },
    { .age = 77, .rate = 0.106566 },
    { .age = 78, .rate = 0.112525 },
    { .age = 79, .rate = 0.119616 },
    { .age = 80, .rate = 0.128177 },
    { .age = 81, .rate = 0.138700 },
    { .age = 82, .rate = 0.151921 },
    { .age = 83, .rate = 0.168995 },
    { .age = 84, .rate = 0.191852 },
    { .age = 85, .rate = 0.223959 },
    { .age = 86, .rate = 0.272256 },
    { .age = 87, .rate = 0.352934 },
    { .age = 88, .rate = 0.514563 },
};

static int num_lif_rates = (int)(sizeof(lif_rates) / sizeof(lif_rates[0]));

double get_lif_max_rate(int age)
{
    double rate = 0.0;
    for (int i = 0; i < num_lif_rates; i++) {
        if (lif_rates[i].age <= age) {
            rate = lif_rates[i].rate;
        } else {
            break;
        }
    }
    return rate;
}

/* =========================================================================
 * LIF ANNUAL DOLLAR WITHDRAWAL LIMITS
 * ====================================================================== */

typedef struct {
    int    year;
    double withdrawal_limit;
} lif_limit_t;

/* Pre-computed annual LIF dollar withdrawal limits (inflation-adjusted). */
static const lif_limit_t lif_withdrawal_limits[] = {
    { .year = 2024, .withdrawal_limit = 27400 },
    { .year = 2025, .withdrawal_limit = 28082 },
    { .year = 2026, .withdrawal_limit = 28781 },
    { .year = 2027, .withdrawal_limit = 29497 },
    { .year = 2028, .withdrawal_limit = 30232 },
    { .year = 2029, .withdrawal_limit = 30984 },
    { .year = 2030, .withdrawal_limit = 31755 },
    { .year = 2031, .withdrawal_limit = 32546 },
    { .year = 2032, .withdrawal_limit = 33356 },
    { .year = 2033, .withdrawal_limit = 34186 },
    { .year = 2034, .withdrawal_limit = 35037 },
    { .year = 2035, .withdrawal_limit = 35909 },
    { .year = 2036, .withdrawal_limit = 36803 },
    { .year = 2037, .withdrawal_limit = 37719 },
    { .year = 2038, .withdrawal_limit = 38658 },
    { .year = 2039, .withdrawal_limit = 39620 },
    { .year = 2040, .withdrawal_limit = 40606 },
    { .year = 2041, .withdrawal_limit = 41617 },
    { .year = 2042, .withdrawal_limit = 42653 },
    { .year = 2043, .withdrawal_limit = 43715 },
    { .year = 2044, .withdrawal_limit = 44803 },
    { .year = 2045, .withdrawal_limit = 45918 },
    { .year = 2046, .withdrawal_limit = 47061 },
    { .year = 2047, .withdrawal_limit = 48233 },
    { .year = 2048, .withdrawal_limit = 49433 },
    { .year = 2049, .withdrawal_limit = 50664 },
    { .year = 2050, .withdrawal_limit = 51925 },
    { .year = 2051, .withdrawal_limit = 53217 },
    { .year = 2052, .withdrawal_limit = 54542 },
    { .year = 2053, .withdrawal_limit = 55899 },
    { .year = 2054, .withdrawal_limit = 57291 },
    { .year = 2055, .withdrawal_limit = 58717 },
    { .year = 2056, .withdrawal_limit = 60178 },
    { .year = 2057, .withdrawal_limit = 61676 },
    { .year = 2058, .withdrawal_limit = 63211 },
    { .year = 2059, .withdrawal_limit = 64785 },
    { .year = 2060, .withdrawal_limit = 66397 },
    { .year = 2061, .withdrawal_limit = 68050 },
    { .year = 2062, .withdrawal_limit = 69744 },
    { .year = 2063, .withdrawal_limit = 71480 },
    { .year = 2064, .withdrawal_limit = 73259 },
    { .year = 2065, .withdrawal_limit = 75083 },
    { .year = 2066, .withdrawal_limit = 76952 },
    { .year = 2067, .withdrawal_limit = 78867 },
    { .year = 2068, .withdrawal_limit = 80830 },
    { .year = 2069, .withdrawal_limit = 82842 },
    { .year = 2070, .withdrawal_limit = 84904 },
    { .year = 2071, .withdrawal_limit = 87018 },
    { .year = 2072, .withdrawal_limit = 89184 },
};

static int num_lif_limits = (int)(sizeof(lif_withdrawal_limits) / sizeof(lif_withdrawal_limits[0]));

double get_lif_withdrawal_limit(int year)
{
    for (int i = 0; i < num_lif_limits; i++) {
        if (lif_withdrawal_limits[i].year == year) {
            return lif_withdrawal_limits[i].withdrawal_limit;
        }
    }
    return 0.0;
}

/* =========================================================================
 * RRIF MINIMUM WITHDRAWAL RATES
 * ====================================================================== */

typedef struct {
    int    age;
    double rate;
} rrif_rate_t;

/* Minimum annual RRIF withdrawal rates by age. */
static const rrif_rate_t rrif_rates[] = {
    { .age = 46, .rate = 0.0227 },
    { .age = 47, .rate = 0.0233 },
    { .age = 48, .rate = 0.0238 },
    { .age = 49, .rate = 0.0244 },
    { .age = 50, .rate = 0.0250 },
    { .age = 51, .rate = 0.0256 },
    { .age = 52, .rate = 0.0263 },
    { .age = 53, .rate = 0.0270 },
    { .age = 54, .rate = 0.0278 },
    { .age = 55, .rate = 0.0286 },
    { .age = 56, .rate = 0.0294 },
    { .age = 57, .rate = 0.0303 },
    { .age = 58, .rate = 0.0313 },
    { .age = 59, .rate = 0.0323 },
    { .age = 60, .rate = 0.0333 },
    { .age = 61, .rate = 0.0345 },
    { .age = 62, .rate = 0.0357 },
    { .age = 63, .rate = 0.0370 },
    { .age = 64, .rate = 0.0385 },
    { .age = 65, .rate = 0.0400 },
    { .age = 66, .rate = 0.0417 },
    { .age = 67, .rate = 0.0435 },
    { .age = 68, .rate = 0.0455 },
    { .age = 69, .rate = 0.0476 },
    { .age = 70, .rate = 0.0500 },
    { .age = 71, .rate = 0.0528 },
    { .age = 72, .rate = 0.0540 },
    { .age = 73, .rate = 0.0553 },
    { .age = 74, .rate = 0.0567 },
    { .age = 75, .rate = 0.0582 },
    { .age = 76, .rate = 0.0598 },
    { .age = 77, .rate = 0.0617 },
    { .age = 78, .rate = 0.0636 },
    { .age = 79, .rate = 0.0658 },
    { .age = 80, .rate = 0.0682 },
    { .age = 81, .rate = 0.0708 },
    { .age = 82, .rate = 0.0738 },
    { .age = 83, .rate = 0.0771 },
    { .age = 84, .rate = 0.0808 },
    { .age = 85, .rate = 0.0851 },
    { .age = 86, .rate = 0.0899 },
    { .age = 87, .rate = 0.0955 },
    { .age = 88, .rate = 0.1021 },
    { .age = 89, .rate = 0.1099 },
    { .age = 90, .rate = 0.1192 },
    { .age = 91, .rate = 0.1306 },
    { .age = 92, .rate = 0.1449 },
    { .age = 93, .rate = 0.1634 },
    { .age = 94, .rate = 0.1879 },
    { .age = 95, .rate = 0.2000 },
};

static int num_rrif_rates = (int)(sizeof(rrif_rates) / sizeof(rrif_rates[0]));

double get_rrif_min_rate(int age)
{
    double rate = 0.0;
    for (int i = 0; i < num_rrif_rates; i++) {
        if (rrif_rates[i].age <= age) {
            rate = rrif_rates[i].rate;
        } else {
            break;
        }
    }
    return rate;
}
