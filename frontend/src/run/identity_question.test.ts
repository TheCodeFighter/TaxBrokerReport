import { describe, expect, it } from "vitest";

import {
  allOneAction,
  eachOwnAction,
  groupingAnswer,
  groupingProblem,
  rowTypeText,
  rowsByAction,
  sameDayStarts,
} from "./identity_question";
import type { IdentityCheck } from "./questions";

// Trade Republic 2025-05-12, Interactive Brokers 2025-05-14, Trade Republic 2025-05-20.
const threeRows: IdentityCheck = {
  name: "Example Split Corp",
  isin: "XX0000000132",
  rows: [
    { broker: "trade_republic", date: "2025-05-12", type: "unresolved_split", transactionId: "t-1", sourceValue: "10" },
    {
      broker: "interactive_brokers",
      date: "2025-05-14",
      type: "split",
      transactionId: "i-1",
      sourceValue: "5",
      brokerActionId: "A1",
    },
    { broker: "trade_republic", date: "2025-05-20", type: "unresolved_split", transactionId: "t-2", sourceValue: "20" },
  ],
};

function rowsOn(dates: string[]): IdentityCheck {
  return {
    name: "Example Split Corp",
    isin: "XX0000000132",
    rows: dates.map((date, index) => ({ broker: "trade_republic", date, type: "split", transactionId: `t-${index}` })),
  };
}

describe("the shortcuts", () => {
  it("gives every row the number 1, or its own number in the order of the rows", () => {
    expect(allOneAction(3)).toEqual([1, 1, 1]);
    expect(eachOwnAction(3)).toEqual([1, 2, 3]);
  });
});

describe("groupingProblem", () => {
  it("wants a number for every row", () => {
    expect(groupingProblem(threeRows, [1, 1, undefined])).toBe("incomplete");
    expect(groupingProblem(threeRows, [1, 1])).toBe("incomplete");
  });

  it("wants the numbers to run from 1 without a gap", () => {
    expect(groupingProblem(threeRows, [1, 1, 3])).toBe("gap");
    expect(groupingProblem(threeRows, [2, 2, 2])).toBe("gap");
  });

  it("takes two actions and one action", () => {
    expect(groupingProblem(threeRows, [1, 1, 2])).toBeUndefined();
    // 2025-05-12 to 2025-05-20 is 8 days, at most 10.
    expect(groupingProblem(threeRows, [1, 1, 1])).toBeUndefined();
  });

  it("refuses rows of one action that are 11 days apart", () => {
    const check = rowsOn(["2025-05-10", "2025-05-14", "2025-05-21"]);

    expect(groupingProblem(check, [1, 2, 1])).toBe("window");
  });

  it("takes rows of one action exactly 10 days apart", () => {
    expect(groupingProblem(rowsOn(["2025-05-10", "2025-05-20"]), [1, 1])).toBeUndefined();
  });

  it("counts 10 days across the year end", () => {
    expect(groupingProblem(rowsOn(["2025-12-28", "2026-01-07"]), [1, 1])).toBeUndefined();
    expect(groupingProblem(rowsOn(["2025-12-28", "2026-01-08"]), [1, 1])).toBe("window");
  });

  it("does not mind how far apart rows of different actions are", () => {
    expect(groupingProblem(rowsOn(["2025-01-10", "2025-09-10"]), [1, 2])).toBeUndefined();
  });
});

describe("sameDayStarts", () => {
  it("is true when two actions start on one day", () => {
    const check = rowsOn(["2025-06-03", "2025-06-03"]);

    expect(sameDayStarts(check, [1, 2])).toBe(true);
    expect(sameDayStarts(check, [1, 1])).toBe(false);
  });

  it("compares the first row of each action", () => {
    // Action 1 starts on 2025-05-12 and action 2 on 2025-05-14.
    expect(sameDayStarts(threeRows, [1, 2, 1])).toBe(false);
    // Action 1 is row 1 (2025-05-12); action 2 is rows 2 and 3 and also starts on 2025-05-12.
    expect(sameDayStarts(rowsOn(["2025-05-12", "2025-05-12", "2025-05-20"]), [1, 2, 2])).toBe(true);
  });

  it("is false while a row has no number", () => {
    expect(sameDayStarts(rowsOn(["2025-06-03", "2025-06-03"]), [1, undefined])).toBe(false);
  });
});

describe("groupingAnswer", () => {
  it("is not there while a row has no number", () => {
    expect(groupingAnswer([1, 1, undefined])).toBeUndefined();
  });

  it("sends the type and one number for each row", () => {
    expect(groupingAnswer([1, 1, 2])).toEqual({ type: "grouping", actionOfRow: [1, 1, 2] });
  });
});

describe("rowTypeText", () => {
  it("names the four row types and leaves an unknown one to the caller", () => {
    expect(rowTypeText("split")).toBe("actionSplit");
    expect(rowTypeText("reverse_split")).toBe("actionReverseSplit");
    expect(rowTypeText("unresolved_split")).toBe("actionUnresolvedSplit");
    expect(rowTypeText("merger")).toBe("actionMerger");
    expect(rowTypeText("spin_off")).toBeUndefined();
  });
});

describe("rowsByAction", () => {
  it("lists the rows of each action number, in the order of the numbers", () => {
    // Choices 1, 2, 1: the first and third row are action 1, the second row is action 2.
    const actions = rowsByAction(threeRows, [1, 2, 1]);

    expect(actions.map((rows) => rows.map((row) => row.date))).toEqual([
      ["2025-05-12", "2025-05-20"],
      ["2025-05-14"],
    ]);
  });

  it("gives one action for each row when every row has its own number", () => {
    const actions = rowsByAction(threeRows, [1, 2, 3]);

    expect(actions.map((rows) => rows.length)).toEqual([1, 1, 1]);
  });
});
