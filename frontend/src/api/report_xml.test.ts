import { describe, expect, it, vi } from "vitest";

import { loadReportXml } from "./report_xml";

describe("loadReportXml", () => {
  it("asks once for the form of the shown revision and gives its text and the file name of the server", async () => {
    const apiFetch = vi.fn().mockResolvedValue(
      new Response("<Envelope/>", {
        status: 200,
        headers: { "Content-Disposition": 'attachment; filename="Doh-Div_2025.xml"' },
      }),
    );

    const answer = await loadReportXml(apiFetch, "dividends", "Doh-Div", 3);

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(apiFetch).toHaveBeenCalledWith("/api/run/reports/dividends/xml?revision=3");
    expect("file" in answer && answer.file.fileName).toBe("Doh-Div_2025.xml");
    expect("file" in answer && (await answer.file.blob.text())).toBe("<Envelope/>");
  });

  it("names the file after the form when the header is missing", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response("<Envelope/>", { status: 200 }));

    const answer = await loadReportXml(apiFetch, "dividends", "Doh-Div", 3);

    expect("file" in answer && answer.file.fileName).toBe("Doh-Div.xml");
  });

  it("keeps the unsafe marker of the server name", async () => {
    const apiFetch = vi.fn().mockResolvedValue(
      new Response("<Envelope/>", {
        status: 200,
        headers: { "Content-Disposition": 'attachment; filename="Doh-KDVP_2025_UNSAFE.xml"' },
      }),
    );

    const answer = await loadReportXml(apiFetch, "capitalGains", "Doh-KDVP", 1);

    expect("file" in answer && answer.file.fileName).toBe("Doh-KDVP_2025_UNSAFE.xml");
  });

  it("says the result is stale for a 409", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response('{"error":"stale_revision"}', { status: 409 }));

    expect(await loadReportXml(apiFetch, "dividends", "Doh-Div", 3)).toEqual({ problem: "stale" });
  });

  it("says it failed for a 404", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response('{"error":"no_xml"}', { status: 404 }));

    expect(await loadReportXml(apiFetch, "dividends", "Doh-Div", 3)).toEqual({ problem: "failed" });
  });

  it("says it failed for a 500", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response("", { status: 500 }));

    expect(await loadReportXml(apiFetch, "dividends", "Doh-Div", 3)).toEqual({ problem: "failed" });
  });
});
