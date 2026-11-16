import { describe, expect, it } from "vitest";

import { uiText, uiTexts, type UiTextKey } from "./ui_texts";

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
});
