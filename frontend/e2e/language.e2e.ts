import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import { card, openReportPage, question, startRun } from "./support/report_page";
import { useSession } from "./support/session";

describe("language", () => {
  const session = useSession();

  it("E14 switches the page to English and keeps the choice after a reload", async () => {
    const page = await openReportPage(session.context, session.server);

    await startRun(page, { files: [await manualFile("trade_republic_2022.csv")], year: 2022 });

    // Slovenian first.
    expect(await page.getByRole("button", { name: "SL" }).getAttribute("aria-pressed")).toBe("true");

    // English: the result status, the form statuses and a question screen change their language.
    await page.getByRole("button", { name: "EN" }).click();
    await page.getByText("We need an answer from you.").waitFor();

    expect(await card(page, "Doh-KDVP").locator(".badge").first().innerText()).toBe("waiting for an answer");
    expect(await card(page, "Doh-Div").locator(".badge").first().innerText()).toBe("ready");
    expect(await card(page, "Doh-DHO").locator(".badge").first().innerText()).toBe("no data");

    const lossShare = question(page, "Synthetic Loss Share (XX0000000103)");

    expect(await lossShare.getByRole("button", { name: "Send answer" }).count()).toBe(1);
    expect(await lossShare.getByRole("button", { name: "Cancel the question" }).count()).toBe(1);

    // After a reload the choice is kept (it is in the browser storage).
    await page.reload();
    await page.getByRole("link", { name: "Report", exact: true }).waitFor();

    expect(await page.getByRole("button", { name: "EN" }).getAttribute("aria-pressed")).toBe("true");
    expect(await page.getByRole("button", { name: "Create the reports" }).count()).toBe(1);
  });
});
