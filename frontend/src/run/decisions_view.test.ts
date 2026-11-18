import { describe, expect, it } from "vitest";

import { correctionLines, decisionRows } from "./decisions_view";
import type { Correction, Decision, Reconciliation } from "./questions";

const lossCheck = {
  name: "Example Share",
  isin: "US0000000001",
  sales: [],
};

describe("decisionRows", () => {
  it("describes a loss answer with the investment and no notes", () => {
    const decision: Decision = {
      id: "decision-0001",
      type: "loss_eligibility_confirmation_required",
      fromEarlierRun: false,
      used: true,
      lossCheck,
      answer: "other_activity",
    };

    expect(decisionRows([decision], "en")).toEqual([
      {
        id: "decision-0001",
        title: "30-day rule",
        investment: "Example Share (US0000000001)",
        answer: "Yes, I did",
        notes: [],
      },
    ]);
  });

  it("describes the no answer and a cancel in Slovenian", () => {
    const no: Decision = {
      id: "decision-0001",
      type: "loss_eligibility_confirmation_required",
      fromEarlierRun: false,
      used: true,
      lossCheck,
      answer: "no_other_activity",
    };
    const cancelled: Decision = { ...no, id: "decision-0002", answer: "cancel" };

    const rows = decisionRows([no, cancelled], "sl");

    expect(rows[0]?.title).toBe("Pravilo 30 dni");
    expect(rows[0]?.answer).toBe("Ne, drugje je nisem kupoval ali prodajal");
    expect(rows[1]?.answer).toBe("Prekinjeno (brez odgovora)");
  });

  it("describes a split ratio answer with its date and confirmed units", () => {
    const decision: Decision = {
      id: "decision-0002",
      type: "corporate_action_ratio_required",
      fromEarlierRun: false,
      used: true,
      ratioCheck: {
        name: "Example Split Corp",
        isin: "US0000000002",
        effectiveDate: "2023-01-15",
        sources: [],
        dateRanges: [],
        askSameEvent: false,
      },
      answer: { newShares: 2, oldShares: 1, effectiveDate: "2023-01-15", confirmedUnits: "2.00000000" },
    };

    const row = decisionRows([decision], "en")[0];

    expect(row?.title).toBe("Split ratio");
    expect(row?.investment).toBe("Example Split Corp (US0000000002)");
    expect(row?.answer).toBe("2 : 1 on 2023-01-15, 2.00000000 units after the split");
  });

  it("describes the separate-events answer", () => {
    const decision: Decision = {
      id: "decision-0003",
      type: "corporate_action_ratio_required",
      fromEarlierRun: false,
      used: true,
      answer: "separate_events",
    };

    expect(decisionRows([decision], "en")[0]?.answer).toBe("Two separate splits");
  });

  it("describes a corrected quantity", () => {
    const decision: Decision = {
      id: "decision-0004",
      type: "quantity_correction_required",
      fromEarlierRun: false,
      used: true,
      quantityCheck: {
        name: "Example Share",
        isin: "US0000000001",
        saleDate: "2024-05-02",
        originalUnits: "1.00000000",
        proposedUnits: "0.99999998",
        unitValueEur: { numerator: "10", denominator: "1" },
        budgetUsedEur: { numerator: "0", denominator: "1" },
      },
      answer: { correctedUnits: "0.99999998" },
    };

    const row = decisionRows([decision], "en")[0];

    expect(row?.title).toBe("Sale quantity");
    expect(row?.answer).toBe("Corrected to 0.99999998 units");
  });

  it("notes an answer from an earlier run and an answer that this result did not use", () => {
    const decision: Decision = {
      id: "decision-0005",
      type: "loss_eligibility_confirmation_required",
      fromEarlierRun: true,
      used: false,
      lossCheck,
      answer: "no_other_activity",
    };

    expect(decisionRows([decision], "en")[0]?.notes).toEqual(["from an earlier run", "not used in this result"]);
  });

  it("gives an answer of a kind without a screen a general text and no investment", () => {
    const decision: Decision = {
      id: "decision-0006",
      type: "tax_classification_required",
      fromEarlierRun: false,
      used: true,
      answer: "capital_gains",
    };

    expect(decisionRows([decision], "en")).toEqual([
      { id: "decision-0006", title: "Other answer", answer: "Answer recorded", notes: [] },
    ]);
  });
});

describe("decisionRows for the income and classification answers", () => {
  const base = { fromEarlierRun: false, used: true };

  it("describes a trade class answer", () => {
    const decision: Decision = {
      ...base,
      id: "decision-0001",
      type: "tax_classification_required",
      classificationCheck: { name: "Example Share", isin: "US0000000001", classLabels: ["Stock"] },
      answer: "capital_gains",
    };

    expect(decisionRows([decision], "en")).toEqual([
      {
        id: "decision-0001",
        title: "Kind of investment",
        investment: "Example Share (US0000000001)",
        answer: "Capital gains (Doh-KDVP)",
        notes: [],
      },
    ]);
  });

  it("describes an income type answer with its kind of other interest", () => {
    const check = {
      eventKind: "interest",
      receiptDate: "2023-02-01",
      name: "Example Cash Interest",
      classLabels: [],
      grossAmount: "1.0000",
      currency: "EUR",
    };
    const plain: Decision = {
      ...base,
      id: "decision-0001",
      type: "tax_classification_required",
      incomeCheck: check,
      answer: "dividends",
    };
    const other: Decision = {
      ...base,
      id: "decision-0002",
      type: "tax_classification_required",
      incomeCheck: check,
      answer: { choice: "other_interest", subtype: "broker_cash" },
    };

    const rows = decisionRows([plain, other], "en");

    expect(rows[0]?.title).toBe("Kind of income");
    expect(rows[0]?.investment).toBe("Example Cash Interest");
    expect(rows[0]?.answer).toBe("Dividends (Doh-Div)");
    expect(rows[1]?.answer).toBe("Other interest (Doh-Obr): Interest on cash at the broker");
  });

  it("describes the picked payer row and a skip", () => {
    const check = {
      name: "Example Corp",
      isin: "US0000000001",
      options: [
        { name: "Example Corp", address: "1 Main Street", country: "US", source: { source: "a", sourceRevision: "r1", file: "payers.csv", row: 2, origin: "built_in" } },
        { name: "Example Corp Ltd", address: "2 Side Street", country: "IE", source: { source: "a", sourceRevision: "r1", file: "payers.csv", row: 9, origin: "built_in" } },
      ],
    };
    const picked: Decision = {
      ...base,
      id: "decision-0001",
      type: "income_details_required",
      payerCheck: check,
      answer: { option: 1 },
    };
    const skipped: Decision = { ...picked, id: "decision-0002", answer: "skip" };

    const rows = decisionRows([picked, skipped], "en");

    expect(rows[0]?.title).toBe("Payer");
    expect(rows[0]?.answer).toBe("Example Corp Ltd (IE), payers.csv, row 9");
    expect(rows[1]?.answer).toBe("Skipped (payer facts empty)");
  });

  it("describes a bond type, a treaty text, no claim and a deposit total", () => {
    const bond: Decision = {
      ...base,
      id: "decision-0001",
      type: "income_details_required",
      bondCheck: { isin: "XS0000000001", name: "Example Bond", soldUnits: "1.00000000", maturedUnits: "0.00000000" },
      answer: "ordinary",
    };
    const treaty: Decision = {
      ...base,
      id: "decision-0002",
      type: "income_details_required",
      treatyCheck: { year: 2022, country: "BR", isins: ["BR0000000001"] },
      answer: { statement: "Article 10" },
    };
    const noClaim: Decision = { ...treaty, id: "decision-0003", answer: "no_claim" };
    const deposit: Decision = {
      ...base,
      id: "decision-0004",
      type: "deposit_interest_total_required",
      depositCheck: { year: 2023, importedInterestEur: "12.5000", limitEur: "1000.0000" },
      answer: "above_limit",
    };

    const rows = decisionRows([bond, treaty, noClaim, deposit], "en");

    expect(rows[0]?.investment).toBe("Example Bond (XS0000000001)");
    expect(rows[0]?.answer).toBe("Ordinary bond (regular coupons)");
    expect(rows[1]?.title).toBe("Treaty text");
    expect(rows[1]?.investment).toBe("BR, 2022");
    expect(rows[1]?.answer).toBe("Text: Article 10");
    expect(rows[2]?.answer).toBe("No relief claimed");
    expect(rows[3]?.investment).toBe("2023");
    expect(rows[3]?.answer).toBe("Yes, the interest of all banks together is above the limit");
  });

  it("describes a cancelled answer of the new kinds", () => {
    const decision: Decision = {
      ...base,
      id: "decision-0001",
      type: "income_details_required",
      bondCheck: { isin: "XS0000000001", name: "Example Bond", soldUnits: "1.00000000", maturedUnits: "0.00000000" },
      answer: "cancel",
    };

    expect(decisionRows([decision], "sl")[0]?.answer).toBe("Prekinjeno (brez odgovora)");
  });
});

describe("correctionLines", () => {
  const rounding: Correction = {
    isin: "US0000000001",
    saleDate: "2024-05-02",
    source: { sourceId: "source-0001" },
    originalUnits: "1.00000000",
    correctedUnits: "0.99999998",
    kind: "rounding_evidence",
    impactEur: { numerator: "1", denominator: "200000" }, // EUR 0.000005
  };
  const confirmed: Correction = { ...rounding, kind: "user_confirmed", impactEur: { numerator: "1", denominator: "3" } };
  const impact: Reconciliation = {
    isin: "US0000000001",
    impactEur: { numerator: "1", denominator: "200" }, // EUR 0.005
    limitEur: "0.0100",
  };

  it("writes the original and corrected units, the kind and the impact with 4 decimals", () => {
    const lines = correctionLines([rounding, confirmed], [impact], "en");

    expect(lines.corrections).toEqual([
      "US0000000001, sale on 2024-05-02: 1.00000000 → 0.99999998 units, rounding in the files, impact EUR 0.0000",
      "US0000000001, sale on 2024-05-02: 1.00000000 → 0.99999998 units, your answer, impact EUR 0.3333",
    ]);
    expect(lines.reconciliation).toEqual(["US0000000001: total impact EUR 0.0050 of EUR 0.01"]);
  });

  it("writes the Slovenian lines", () => {
    const lines = correctionLines([confirmed], [], "sl");

    expect(lines.corrections).toEqual([
      "US0000000001, prodaja 2024-05-02: 1.00000000 → 0.99999998 enot, Vaš odgovor, vpliv 0.3333 EUR",
    ]);
    expect(lines.reconciliation).toEqual([]);
  });
});
