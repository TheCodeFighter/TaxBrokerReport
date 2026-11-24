import { describe, expect, it } from "vitest";

import { brokerKinds } from "./input_kinds";

describe("brokerKinds", () => {
  it("lists Trade Republic first and Interactive Brokers second, with unique request names", () => {
    expect(brokerKinds.map((kind) => kind.broker)).toEqual(["trade_republic", "interactive_brokers"]);
  });

  it("filters the file dialog by the export type of each broker", () => {
    expect(brokerKinds.map((kind) => kind.accept)).toEqual([".csv,text/csv", ".xml,application/xml,text/xml"]);
  });

  it("names every broker and its export in both languages", () => {
    expect(brokerKinds.map((kind) => [kind.title.sl, kind.title.en])).toEqual([
      ["Trade Republic", "Trade Republic"],
      ["Interactive Brokers", "Interactive Brokers"],
    ]);
    expect(brokerKinds.map((kind) => [kind.format.sl, kind.format.en])).toEqual([
      ["Izvoz transakcij (CSV)", "Transaction export (CSV)"],
      ["Flex Query (XML)", "Flex Query (XML)"],
    ]);
  });
});
