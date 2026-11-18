import { describe, expect, it } from "vitest";

import { uiText, uiTexts, uiTextWith, type UiTextKey } from "./ui_texts";

describe("uiTexts", () => {
  it("has a non-empty Slovenian and English text for every key", () => {
    const keys = Object.keys(uiTexts) as UiTextKey[];

    expect(keys.length).toBeGreaterThan(0);

    for (const key of keys) {
      expect(uiTexts[key].sl.trim(), `${key} sl`).not.toBe("");
      expect(uiTexts[key].en.trim(), `${key} en`).not.toBe("");
    }
  });

  it("returns the text of the language", () => {
    expect(uiText("sl", "unknownMessage")).toBe("Neznano sporočilo");
    expect(uiText("en", "unknownMessage")).toBe("Unknown message");
  });

  it("has the support report texts", () => {
    expect(uiText("sl", "navSupport")).toBe("Podpora");
    expect(uiText("en", "navSupport")).toBe("Support");
    expect(uiText("sl", "supportReportSave")).toBe("Shrani poročilo za podporo");
    expect(uiText("en", "supportReportSave")).toBe("Save support report");
    expect(uiText("en", "supportReportHint")).toBe(
      "For a problem report. It holds no amounts, names, tax numbers or file names, but it does hold the ISINs and dates of the problems. Read it before you send it.",
    );
    expect(uiText("sl", "supportReportHint")).toBe(
      "Za prijavo težave. Ne vsebuje zneskov, imen, davčnih številk ali imen datotek, vsebuje pa ISIN-e in datume težav. Preberite ga, preden ga pošljete.",
    );
  });

  it("has the report save texts", () => {
    expect(uiTextWith("sl", "reportSave", { form: "Doh-Div" })).toBe("Shrani datoteko Doh-Div");
    expect(uiTextWith("en", "reportSave", { form: "Doh-Div" })).toBe("Save the Doh-Div file");
    expect(uiText("en", "reportSaveHint")).toBe(
      "This file is not filed yet: import it in eDavki, check the values and file the return there.",
    );
    expect(uiText("sl", "reportSaveStale")).toBe("Rezultat se je medtem spremenil. Ponovno ustvarite poročila.");
    expect(uiText("en", "reportSaveStale")).toBe("The result changed in the meantime. Create the reports again.");
  });

  it("has the navigation texts", () => {
    expect(uiText("sl", "navManual")).toBe("Navodila");
    expect(uiText("en", "navAbout")).toBe("About");
  });

  it("fills the places of a text", () => {
    expect(uiTextWith("en", "resultCounts", { errors: 1, warnings: 2 })).toBe("Errors: 1, warnings: 2");
    expect(uiTextWith("sl", "fileSuffix", { n: 2 })).toBe(" (datoteka 2)");
    expect(uiTextWith("en", "versionLabel", { version: "2.0.0" })).toBe("Version 2.0.0");
    expect(uiTextWith("en", "rateFilesHint", { first: 2016, last: 2025 })).toBe(
      "For trades not in EUR. The rates of the years 2016 to 2025 are already in the application.",
    );
  });

  it("fills the places of the result texts", () => {
    expect(uiTextWith("en", "questionsWaiting", { count: 2 })).toBe("Questions waiting for an answer: 2");
    expect(uiTextWith("sl", "questionsWaiting", { count: 2 })).toBe("Vprašanja, ki čakajo na odgovor: 2");
    expect(uiTextWith("en", "evidenceRows", { count: 3 })).toBe(
      "Rows that need the proof of the foreign tax attached in eDavki: 3",
    );
    expect(uiTextWith("en", "groupCount", { count: 12 })).toBe("12 times");
    expect(uiTextWith("sl", "groupCount", { count: 12 })).toBe("12-krat");
    expect(uiTextWith("en", "detailsSummary", { count: 7 })).toBe("Details (7)");
    expect(uiTextWith("sl", "detailsSummary", { count: 7 })).toBe("Podrobnosti (7)");
    expect(uiTextWith("en", "fieldLabel", { field: "datetime" })).toBe("field datetime");
    expect(uiTextWith("sl", "sourceFile", { n: 3 })).toBe("datoteka 3");
  });
});
