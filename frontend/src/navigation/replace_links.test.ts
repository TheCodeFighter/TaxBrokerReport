import { describe, expect, it } from "vitest";

import { replacePageLink, type LinkClick } from "./replace_links";

function clickOn(href: string | null, overrides: Partial<LinkClick> = {}): LinkClick & { prevented: boolean } {
  const click = {
    defaultPrevented: false,
    button: 0,
    metaKey: false,
    ctrlKey: false,
    shiftKey: false,
    altKey: false,
    target: {
      closest: () => (href === null ? null : { getAttribute: () => href }),
    },
    prevented: false,
    preventDefault() {
      click.prevented = true;
    },
    ...overrides,
  };

  return click;
}

describe("replacePageLink", () => {
  it("replaces the history entry for a page link", () => {
    const replaced: string[] = [];
    const click = clickOn("#/report");

    expect(replacePageLink(click, (hash) => replaced.push(hash))).toBe(true);
    expect(replaced).toEqual(["#/report"]);
    expect(click.prevented).toBe(true);
  });

  it("leaves an external link and a click outside a link alone", () => {
    const replaced: string[] = [];

    expect(replacePageLink(clickOn("https://example.org/"), (hash) => replaced.push(hash))).toBe(false);
    expect(replacePageLink(clickOn(null), (hash) => replaced.push(hash))).toBe(false);
    expect(replaced).toEqual([]);
  });

  it("leaves a click that opens the link elsewhere alone", () => {
    const replaced: string[] = [];

    for (const overrides of [{ button: 1 }, { ctrlKey: true }, { metaKey: true }, { shiftKey: true }, { altKey: true }]) {
      const click = clickOn("#/report", overrides);

      expect(replacePageLink(click, (hash) => replaced.push(hash))).toBe(false);
      expect(click.prevented).toBe(false);
    }

    expect(replaced).toEqual([]);
  });

  it("leaves a click that something else already handled alone", () => {
    const replaced: string[] = [];

    expect(replacePageLink(clickOn("#/report", { defaultPrevented: true }), (hash) => replaced.push(hash))).toBe(false);
    expect(replaced).toEqual([]);
  });
});
