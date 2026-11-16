/** `kMinimumReportingYear` in the server. */
export const FIRST_YEAR = 2000;

/** The years with built-in ECB rates: the files `resources/ecb/rates_<year>.csv` (2016 to 2025). */
export const FIRST_BUILT_IN_RATE_YEAR = 2016;
export const LAST_BUILT_IN_RATE_YEAR = 2025;

/** The years to choose from: the current year down to the first one the server accepts. */
export function reportingYears(today: Date): number[] {
  const years: number[] = [];

  for (let year = today.getFullYear(); year >= FIRST_YEAR; year -= 1) {
    years.push(year);
  }

  return years;
}

/** The year people file for: the one before the current year. */
export function defaultYear(today: Date): number {
  return today.getFullYear() - 1;
}

/** True when the year has no built-in rates, so a trade not in EUR needs a rate file. */
export function needsRateHint(year: number): boolean {
  return year < FIRST_BUILT_IN_RATE_YEAR || year > LAST_BUILT_IN_RATE_YEAR;
}
