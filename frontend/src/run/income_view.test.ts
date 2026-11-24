import { describe, expect, it } from "vitest";

import { benefitView, depositReminder, feeView, incomeGroups, incomeSummaryLine } from "./income_view";
import type { IncomeRow, IncomeSummary } from "./submit";

const dividend: IncomeRow = {
  kind: "dividend",
  isin: "US0000000001",
  date: "2023-05-02",
  grossEur: "10.0000",
  foreignTaxEur: "1.5000",
  taxBlankInExport: false,
  payer: {
    name: "Example Corp",
    country: "US",
    catalogueRow: { source: "example", sourceRevision: "r1", file: "payers.csv", row: 7, origin: "built_in" },
  },
  treaty: {
    claim: "reference_text",
    catalogueRow: { source: "example", sourceRevision: "r1", file: "treaties.csv", row: 3, origin: "built_in" },
  },
  evidenceRequired: true,
};

describe("incomeGroups", () => {
  it("shows a found payer with its reference row and the treaty row", () => {
    const summary: IncomeSummary = { grossEur: "10.0000", foreignTaxEur: "1.5000", rowCount: 1, rows: [dividend] };

    expect(incomeGroups(summary, "en")).toEqual([
      {
        title: "Dividends",
        rows: [
          {
            date: "2023-05-02",
            subject: "US0000000001",
            payer: "Example Corp (US)",
            payerSource: "payers.csv, row 7",
            gross: "10.0000",
            tax: "1.5000",
            treaty: "text from the reference file (treaties.csv, row 3)",
            evidence: true,
          },
        ],
      },
    ]);
  });

  it("writes the gross and the tax of a row with the decimal comma in Slovenian", () => {
    const summary: IncomeSummary = { grossEur: "10.0000", foreignTaxEur: "1.5000", rowCount: 1, rows: [dividend] };
    const row = incomeGroups(summary, "sl")[0]?.rows[0];

    expect(row?.gross).toBe("10,0000");
    expect(row?.tax).toBe("1,5000");
  });

  it("says a payer is not found and a blank tax is no tax in the export, not 0", () => {
    const blank: IncomeRow = {
      kind: "dividend",
      isin: "US0000000002",
      date: "2023-06-01",
      grossEur: "5.0000",
      foreignTaxEur: "0.0000",
      taxBlankInExport: true,
      payer: {},
      treaty: { claim: "none" },
      evidenceRequired: false,
    };
    const realZero: IncomeRow = { ...blank, isin: "US0000000003", taxBlankInExport: false };
    const summary: IncomeSummary = { grossEur: "10.0000", foreignTaxEur: "0.0000", rowCount: 2, rows: [blank, realZero] };

    const rows = incomeGroups(summary, "en")[0]?.rows;

    expect(rows?.[0]?.payer).toBe("payer not found, complete it in eDavki");
    expect(rows?.[0]?.payerSource).toBeUndefined();
    expect(rows?.[0]?.tax).toBe("no tax in the export");
    expect(rows?.[0]?.treaty).toBe("no claim");
    expect(rows?.[0]?.evidence).toBe(false);
    expect(rows?.[1]?.tax).toBe("0.0000");
  });

  it("makes one group for each kind, 2 broker cash rows and 1 coupon, in result order", () => {
    const cash: IncomeRow = {
      kind: "broker_cash_interest",
      broker: "trade_republic",
      date: "2023-02-01",
      grossEur: "1.0000",
      foreignTaxEur: "0.0000",
      taxBlankInExport: true,
      payer: {},
      treaty: { claim: "none" },
      evidenceRequired: false,
    };
    const coupon: IncomeRow = {
      kind: "bond_coupon",
      isin: "XS0000000001",
      date: "2023-04-01",
      grossEur: "1.0000",
      foreignTaxEur: "0.0000",
      taxBlankInExport: true,
      payer: {},
      treaty: { claim: "none" },
      evidenceRequired: false,
    };
    const summary: IncomeSummary = {
      grossEur: "3.0000",
      foreignTaxEur: "0.0000",
      rowCount: 3,
      rows: [cash, { ...cash, date: "2023-03-01" }, coupon],
    };

    const groups = incomeGroups(summary, "en");

    expect(groups.map((group) => [group.title, group.rows.length])).toEqual([
      ["Interest on cash at the broker", 2],
      ["Bond coupons", 1],
    ]);
    expect(groups[0]?.rows[0]?.subject).toBe("Trade Republic");
  });

  it("writes the one-line summary in both languages", () => {
    const summary: IncomeSummary = { grossEur: "10.0000", foreignTaxEur: "1.5000", rowCount: 1, rows: [dividend] };

    expect(incomeSummaryLine(summary, "en")).toBe("Rows: 1, gross EUR 10.0000, foreign tax EUR 1.5000");
    expect(incomeSummaryLine(summary, "sl")).toBe("Vrstice: 1, bruto 10,0000 EUR, tuji davek 1,5000 EUR");
  });
});

describe("depositReminder", () => {
  it("reminds to add the other banks only when the form holds just the imported interest", () => {
    expect(depositReminder({ importedInterestEur: "900.0000", limitEur: "1000.0000", addOtherBanks: true }, "en")).toBe(
      "The form holds only the interest from your files (EUR 900.0000). Interest from other banks is not in it: add it in eDavki (the limit of EUR 1000.0000 is for all banks together).",
    );
    expect(depositReminder({ importedInterestEur: "1200.0000", limitEur: "1000.0000", addOtherBanks: false }, "en")).toBeUndefined();
    expect(depositReminder(undefined, "en")).toBeUndefined();
  });
});

describe("feeView", () => {
  it("shows no sum when a fee has no rate", () => {
    const view = feeView(
      {
        brokers: [
          {
            broker: "trade_republic",
            byCurrency: [
              { currency: "EUR", count: 3, total: "3.0000" },
              { currency: "USD", count: 1, total: "1.0000" },
            ],
            totalEur: null,
            brokerRateCount: 0,
          },
        ],
        totalEur: null,
      },
      "en",
    );

    expect(view).toEqual({
      rows: [
        {
          title: "Trade Republic",
          lines: ["EUR 3.00 (count: 3)", "USD 1.00 (count: 1)"],
          total: "Total in EUR: not available (an amount has no exchange rate)",
        },
      ],
      total: "Total in EUR: not available (an amount has no exchange rate)",
    });
    // No partial sum of the known part (3.0000 + 1.0000) appears anywhere.
    expect(JSON.stringify(view)).not.toContain("4.0000");
  });

  it("shows the total and how many fees used the broker's own rate", () => {
    const view = feeView(
      {
        brokers: [
          {
            broker: "trade_republic",
            byCurrency: [
              { currency: "EUR", count: 3, total: "3.0000" },
              { currency: "USD", count: 1, total: "1.0000" },
            ],
            totalEur: "4.9000",
            brokerRateCount: 1,
          },
        ],
        totalEur: "4.9000",
      },
      "en",
    );

    // 4.9000 is shown with two decimals.
    expect(view?.rows[0]?.total).toBe("Total in EUR: 4.90");
    expect(view?.rows[0]?.converted).toBe("Converted at the broker's own rate: 1");
    expect(view?.total).toBe("Total in EUR: 4.90");
  });

  it("is undefined without any fee", () => {
    expect(feeView(undefined, "en")).toBeUndefined();
    expect(feeView({ brokers: [], totalEur: null }, "en")).toBeUndefined();
  });

  it("shows a currency whose total is not known without a number", () => {
    const view = feeView(
      {
        brokers: [
          { broker: "trade_republic", byCurrency: [{ currency: "USD", count: 2, total: null }], totalEur: null, brokerRateCount: 0 },
        ],
        totalEur: null,
      },
      "sl",
    );

    expect(view?.rows[0]?.lines).toEqual(["USD: znesek ni na voljo (število: 2)"]);
  });
});

describe("benefitView", () => {
  it("shows one row for a saveback with its total", () => {
    const view = benefitView(
      {
        brokers: [
          {
            broker: "trade_republic",
            type: "saveback",
            byCurrency: [{ currency: "EUR", count: 2, total: "30.0000" }],
            totalEur: "30.0000",
          },
        ],
        totalEur: "30.0000",
      },
      "en",
    );

    expect(view).toEqual({
      rows: [{ title: "Trade Republic: saveback", lines: ["EUR 30.00 (count: 2)"], total: "Total in EUR: 30.00" }],
      total: "Total in EUR: 30.00",
    });
  });

  it("shows a type it does not know as it is", () => {
    const view = benefitView(
      { brokers: [{ broker: "trade_republic", type: "new_kind", byCurrency: [], totalEur: "1.0000" }], totalEur: "1.0000" },
      "en",
    );

    expect(view?.rows[0]?.title).toBe("Trade Republic: new_kind");
  });
});
