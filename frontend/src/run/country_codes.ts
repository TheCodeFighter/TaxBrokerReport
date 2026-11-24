/**
 * The officially assigned ISO 3166-1 alpha-2 codes, the same list as `isIsoCountryCode` in the
 * backend (`src/utils/country_codes.cpp`), so a typed code is checked before it is sent. The
 * server still decides; keep the two lists equal when one changes. Each row is a first letter
 * followed by the second letters of its codes ("AD", "AE", ... is the row "ADEFGILMOQRSTUWXZ").
 */
const countryCodeRows = [
  "ADEFGILMOQRSTUWXZ",
  "BABDEFGHIJLMNOQRSTVWYZ",
  "CACDFGHIKLMNORUVWXYZ",
  "DEJKMOZ",
  "ECEGHRST",
  "FIJKMOR",
  "GABDEFGHILMNPQRSTUWY",
  "HKMNRTU",
  "IDELMNOQRST",
  "JEMOP",
  "KEGHIMNPRWYZ",
  "LABCIKRSTUVY",
  "MACDEFGHKLMNOPQRSTUVWXYZ",
  "NACEFGILOPRUZ",
  "OM",
  "PAEFGHKLMNRSTWY",
  "QA",
  "REOSUW",
  "SABCDEGHIJKLMNORSTVXYZ",
  "TCDFGHJKLMNORTVWZ",
  "UAGMSYZ",
  "VACEGINU",
  "WFS",
  "YET",
  "ZAMW",
];

const countryCodes = new Set(
  countryCodeRows.flatMap((row) => Array.from(row.slice(1), (second) => row[0] + second)),
);

/** Whether the text is an assigned ISO 3166-1 alpha-2 code in capital letters. */
export function isCountryCode(code: string): boolean {
  return countryCodes.has(code);
}
