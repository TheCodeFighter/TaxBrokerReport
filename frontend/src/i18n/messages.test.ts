import { describe, expect, it, vi } from "vitest";

import { createMessageLoader, renderDiagnostic, type MessageCatalogue } from "./messages";

const catalogue: MessageCatalogue = {
  messages: {
    "x.y": {
      en: "No rate for {currency} on {date}.",
      sl: "Ni tečaja za {currency} na dan {date}.",
      params: [
        { name: "currency", kind: "text" },
        { name: "date", kind: "text" },
      ],
    },
    "x.term": {
      en: "Problem in {rowKind}.",
      sl: "Težava v vrstici: {rowKind}.",
      params: [{ name: "rowKind", kind: "term" }],
    },
    "x.count": {
      en: "{count} rows were skipped.",
      sl: "Preskočenih vrstic: {count}.",
      params: [{ name: "count", kind: "number" }],
    },
    "x.year": {
      en: "The ECB rate file for {year} is not loaded.",
      sl: "Datoteka s tečaji ECB za leto {year} ni naložena.",
      params: [{ name: "year", kind: "number" }],
    },
  },
  terms: { "row_kind.trade": { en: "trade", sl: "posel" } },
};

describe("renderDiagnostic", () => {
  it("fills the parameters into the English template", () => {
    const text = renderDiagnostic(catalogue, "x.y", { currency: "USD", date: "2024-03-01" }, "en");

    expect(text).toBe("No rate for USD on 2024-03-01.");
  });

  it("fills the parameters into the Slovenian template", () => {
    const text = renderDiagnostic(catalogue, "x.y", { currency: "USD", date: "2024-03-01" }, "sl");

    expect(text).toBe("Ni tečaja za USD na dan 2024-03-01.");
  });

  it("translates a term parameter", () => {
    const text = renderDiagnostic(catalogue, "x.term", { rowKind: "row_kind.trade" }, "sl");

    expect(text).toBe("Težava v vrstici: posel.");
  });

  it("writes a number parameter as plain digits", () => {
    expect(renderDiagnostic(catalogue, "x.count", { count: 3 }, "en")).toBe("3 rows were skipped.");
    expect(renderDiagnostic(catalogue, "x.count", { count: 12345 }, "sl")).toBe("Preskočenih vrstic: 12345.");
  });

  it("writes a decimal text parameter with the decimal comma in Slovenian only", () => {
    expect(renderDiagnostic(catalogue, "x.y", { currency: "12.50", date: "2024-03-01" }, "sl")).toBe(
      "Ni tečaja za 12,50 na dan 2024-03-01.",
    );
    expect(renderDiagnostic(catalogue, "x.y", { currency: "12.50", date: "2024-03-01" }, "en")).toBe(
      "No rate for 12.50 on 2024-03-01.",
    );
  });

  it("writes a year without digit grouping in both languages", () => {
    // Grouping would give "2,024" (en) and "2.024" (sl).
    expect(renderDiagnostic(catalogue, "x.year", { year: 2024 }, "en")).toBe(
      "The ECB rate file for 2024 is not loaded.",
    );
    expect(renderDiagnostic(catalogue, "x.year", { year: 2024 }, "sl")).toBe(
      "Datoteka s tečaji ECB za leto 2024 ni naložena.",
    );
  });

  it("shows the unknown-message text with the key for an unknown key", () => {
    expect(renderDiagnostic(catalogue, "a.b", {}, "en")).toBe("Unknown message: a.b");
    expect(renderDiagnostic(catalogue, "a.b", {}, "sl")).toBe("Neznano sporočilo: a.b");
  });
});

describe("createMessageLoader", () => {
  it("loads the catalogue once", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response(JSON.stringify(catalogue), { status: 200 }));
    const load = createMessageLoader(apiFetch);

    const first = await load();
    const second = await load();

    expect(first.terms["row_kind.trade"]?.sl).toBe("posel");
    expect(second).toBe(first);
    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(apiFetch).toHaveBeenCalledWith("/api/messages");
  });

  it("tries again after a failed load", async () => {
    const apiFetch = vi
      .fn()
      .mockResolvedValueOnce(new Response(null, { status: 500 }))
      .mockResolvedValueOnce(new Response(JSON.stringify(catalogue), { status: 200 }));
    const load = createMessageLoader(apiFetch);

    await expect(load()).rejects.toThrow("messages_unavailable");
    const loaded = await load();

    expect(loaded.messages["x.y"]?.en).toBe("No rate for {currency} on {date}.");
    expect(apiFetch).toHaveBeenCalledTimes(2);
  });
});
