import { describe, expect, it } from "vitest";

import { questionFiles } from "./question_files";

describe("questionFiles", () => {
  it("names the broker and the file once, however many rows of the file the question uses", () => {
    const sources = [
      { sourceId: "source-0002", broker: "trade_republic", file: "export.csv", row: 5, transactionId: "t-5" },
      { sourceId: "source-0002", broker: "trade_republic", file: "export.csv", row: 9 },
    ];

    expect(questionFiles(sources, ["a.csv", "export.csv"], "en")).toEqual(["Trade Republic: export.csv"]);
  });

  it("lists the files in the order of their numbers, each with its own broker", () => {
    const sources = [
      { sourceId: "source-0003", broker: "interactive_brokers", file: "flex.xml" },
      { sourceId: "source-0001", broker: "trade_republic", file: "tr.csv" },
    ];

    expect(questionFiles(sources, ["tr.csv", "x.csv", "flex.xml"], "sl")).toEqual([
      "Trade Republic: tr.csv",
      "Interactive Brokers: flex.xml",
    ]);
  });

  it("tells two files with the same name apart by the label of the sent files", () => {
    const sources = [
      { sourceId: "source-0001", broker: "trade_republic", file: "export.xml" },
      { sourceId: "source-0002", broker: "interactive_brokers", file: "export.xml" },
    ];

    expect(questionFiles(sources, ["export.xml (file 1)", "export.xml (file 2)"], "en")).toEqual([
      "Trade Republic: export.xml (file 1)",
      "Interactive Brokers: export.xml (file 2)",
    ]);
  });

  it("falls back to the file number when the server sent no name, and has no lines without sources", () => {
    expect(questionFiles([{ sourceId: "source-0004", broker: "trade_republic" }], [], "en")).toEqual([
      "Trade Republic: file 4",
    ]);
    expect(questionFiles([], [], "en")).toEqual([]);
  });
});
