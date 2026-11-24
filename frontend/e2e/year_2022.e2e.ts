import type { Page } from "playwright-core";
import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  card,
  cardStatus,
  download,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
  typeInto,
} from "./support/report_page";
import { IBKR_FILE_NAMES, MANUAL_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

describe("year 2022 of the manual files", () => {
  const session = useSession();

  /** Loads the eight files, picks 2022 and answers the four questions of the README. */
  async function runAndAnswer(page: Page): Promise<void> {
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));
    const ibkrFiles = await Promise.all(IBKR_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, areaFiles: [{ title: "Interactive Brokers", files: ibkrFiles }], year: 2022 });

    // 30-day loss, Partly Blocked Share: yes.
    const partlyBlocked = question(page, "Synthetic Partly Blocked Share (XX0000000104)");
    await pickWord(partlyBlocked, "other_activity");
    await sendAnswer(partlyBlocked);

    // 30-day loss, Loss Share: no.
    const lossShare = question(page, "Synthetic Loss Share (XX0000000103)");
    await pickWord(lossShare, "no_other_activity");
    await sendAnswer(lossShare);

    // Treaty text for BR: other, with the text "Article 10".
    const treaty = question(page, "Davčna pogodba: BR, 2022");
    await pickWord(treaty, "text");
    await typeInto(treaty, "Besedilo pogodbe (do 100 znakov)", "Article 10");
    await sendAnswer(treaty);

    // Payer of NVIDIA: the first row.
    const payer = question(page, "NVIDIA Corporation (US67066G1040)");
    await pickWord(payer, "0");
    await sendAnswer(payer);
  }

  it("E5 answers the four questions of 2022 and saves Doh-KDVP", async () => {
    const page = await openReportPage(session.context, session.server);

    await runAndAnswer(page);

    // README, 2022: Doh-KDVP generated; Doh-Div 8 rows (the IBKR dividend is one); Doh-DHO no data;
    // Doh-Obr 15 rows (the IBKR broker interest is one).
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await cardStatus(page, "Doh-DHO")).toBe("ni podatkov");
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 8,/).count()).toBe(1);
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 15,/).count()).toBe(1);

    const saved = await download(page, card(page, "Doh-KDVP").getByRole("button", { name: "Shrani datoteko Doh-KDVP" }));

    expect(saved.name).toBe("Doh-KDVP_2022.xml");
    expect(saved.text.startsWith("<?xml")).toBe(true);
  });

  it("E13 deletes a recorded answer and its question comes back", async () => {
    const page = await openReportPage(session.context, session.server);

    await runAndAnswer(page);

    const dividends = card(page, "Doh-Div");
    const brazilRow = dividends.locator("tr", { hasText: "Banco Bradesco" });

    // The four answers are listed, and the Doh-Div row of Brazil claims the treaty with "Vaše besedilo".
    expect(await page.getByRole("region", { name: "Vaši odgovori (4)" }).count()).toBe(1);
    expect(await brazilRow.textContent()).toContain("Vaše besedilo");

    // Delete the treaty answer: the question returns and the claim leaves the row. The claim is
    // optional, so Doh-Div stays generated.
    const treatyAnswer = page.locator(".recorded-answer", { hasText: "Besedilo pogodbe" });

    await treatyAnswer.getByRole("button", { name: "Izbriši" }).click();
    await question(page, "Davčna pogodba: BR, 2022").waitFor();

    expect(await page.getByRole("region", { name: "Vaši odgovori (3)" }).count()).toBe(1);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await brazilRow.textContent()).toContain("brez oprostitve");

    // Answer again: the claim is back.
    const treaty = question(page, "Davčna pogodba: BR, 2022");

    await pickWord(treaty, "text");
    await typeInto(treaty, "Besedilo pogodbe (do 100 znakov)", "Article 10");
    await sendAnswer(treaty);

    expect(await page.getByRole("region", { name: "Vaši odgovori (4)" }).count()).toBe(1);
    expect(await brazilRow.textContent()).toContain("Vaše besedilo");
  });
});
