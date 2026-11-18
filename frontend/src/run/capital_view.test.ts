import { describe, expect, it } from "vitest";

import { capitalView } from "./capital_view";
import type { CapitalInvestment, CapitalSummary } from "./submit";

const source = { sourceId: "source-0001", file: "history.csv", row: 5 };

const listed: CapitalInvestment = {
  isin: "XX0000000001",
  name: "Example Share",
  state: "listed",
  reasonCodes: [],
  listedSales: 2,
  listedUnits: "3.00000000",
  blockedLossUnits: "1.50000000",
  sales: [
    { date: "2024-03-01", source },
    { date: "2024-03-01", source: { ...source, row: 6 } },
    { date: "2024-05-02", source: { ...source, row: 9 } },
  ],
};

describe("capitalView", () => {
  it("has nothing to show when there is no summary or no sale", () => {
    expect(capitalView(undefined, "en")).toBeUndefined();
    expect(capitalView({ investments: [], longHeld: [] }, "en")).toBeUndefined();
  });

  it("shows a listed investment with its units, blocked units and the distinct sale dates", () => {
    const view = capitalView({ investments: [listed], longHeld: [] }, "en");

    expect(view?.summaryLine).toBe("Investments with a sale in the year: 1, in the form: 1");
    expect(view?.rows).toEqual([
      {
        investment: "Example Share (XX0000000001)",
        state: "in the form",
        sales: 2,
        units: "3.00000000",
        blockedUnits: "1.50000000",
        dates: "2024-03-01, 2024-05-02",
      },
    ]);
    expect(view?.longHeld).toEqual([]);
  });

  it("shows a dash when no loss is blocked and the decimal comma in Slovenian", () => {
    const view = capitalView({ investments: [{ ...listed, blockedLossUnits: "0.00000000" }], longHeld: [] }, "sl");

    expect(view?.rows[0]?.blockedUnits).toBe("–");
    expect(view?.rows[0]?.units).toBe("3,00000000");
    expect(view?.rows[0]?.state).toBe("v obrazcu");
    expect(view?.summaryLine).toBe("Naložbe s prodajo v letu: 1, v obrazcu: 1");
  });

  it("names every state and counts only the listed investments as in the form", () => {
    const summary: CapitalSummary = {
      investments: [
        listed,
        { ...listed, isin: "XX0000000002", state: "blocked", reasonCodes: ["incomplete_history"], listedSales: 0, listedUnits: "0.00000000", blockedLossUnits: "0.00000000" },
        { ...listed, isin: "XX0000000003", state: "excluded", listedSales: 0, listedUnits: "0.00000000", blockedLossUnits: "0.00000000" },
        { ...listed, isin: "XX0000000004", state: "long_held_only", listedSales: 0, listedUnits: "0.00000000", blockedLossUnits: "0.00000000" },
      ],
      longHeld: [],
    };
    const view = capitalView(summary, "en");

    expect(view?.summaryLine).toBe("Investments with a sale in the year: 4, in the form: 1");
    expect(view?.rows.map((row) => row.state)).toEqual([
      "in the form",
      "blocked by an error",
      "left out (your choice)",
      "only units past the holding period",
    ]);
  });

  it("lists the units past the holding period and shows an unknown state as it is", () => {
    const view = capitalView(
      {
        investments: [{ ...listed, state: "other_state" }],
        longHeld: [{ isin: "XX0000000001", name: "Example Share", saleDate: "2024-03-01", units: "0.50000000" }],
      },
      "en",
    );

    expect(view?.rows[0]?.state).toBe("other_state");
    expect(view?.longHeld).toEqual(["Example Share (XX0000000001), sale 2024-03-01: 0.50000000 units"]);
  });
});
