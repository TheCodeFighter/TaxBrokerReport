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
