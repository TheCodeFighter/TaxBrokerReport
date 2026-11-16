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
});
