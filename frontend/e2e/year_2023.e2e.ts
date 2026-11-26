import type { Page } from "playwright-core";
import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  answerRatio,
  card,
  cardStatus,
  excludedInvestments,
  cancelAnswer,
  download,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { IBKR_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const YEARS = ["2022", "2023", "2024", "2025"];

describe("year 2023 of the manual files", () => {
  const session = useSession();

  /** Runs 2023 and answers everything except the exclusion question of Doh-Div. */
  async function runAndAnswerUpToDohDiv(page: Page): Promise<void> {
    const files = await Promise.all(YEARS.map((year) => manualFile(`trade_republic_${year}.csv`)));
    const ibkrFiles = await Promise.all(IBKR_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, areaFiles: [{ title: "Interactive Brokers", files: ibkrFiles }], year: 2023 });

    // Class of the unknown share: capital gains.
    const classification = question(page, "Synthetic Unknown Class Share (XX0000000118)", "Doh-KDVP");
    await pickWord(classification, "capital_gains");
    await sendAnswer(classification);

    // Split ratio 2 : 1 with the date of the row, confirmed.
    const ratio = question(page, "Synthetic Split Share (XX0000000107)");
    await answerRatio(ratio, "2", "1");

    // Payer of British American Tobacco: the first row.
    const payer = question(page, "British American Tobacco plc (GB0002875804)");
    await pickWord(payer, "0");
    await sendAnswer(payer);

    // Type of the unknown dividend: dividends.
    const income = question(page, "Synthetic Unknown Class Share (XX0000000118)", "Doh-Div");
    await pickWord(income, "dividends");
    await sendAnswer(income);

    // Bond type: ordinary.
    const bond = question(page, "Synthetic Bond A 2025 (XS0000000201)");
    await pickWord(bond, "ordinary");
    await sendAnswer(bond);

    // Deposit total: above the limit (yes).
    const deposit = question(page, "Obresti od depozitov 2023");
    await pickWord(deposit, "above_limit");
    await sendAnswer(deposit);

    // Exclusion of the ruble share from Doh-KDVP.
    const kdvpFailure = question(page, "Doh-KDVP: nadaljevanje brez teh naložb?");
    await pickWord(kdvpFailure, "exclude_failed_instruments");
    await sendAnswer(kdvpFailure);
  }

  it("E6 answers the questions of 2023 and leaves the ruble share out", async () => {
    const page = await openReportPage(session.context, session.server);

    await runAndAnswerUpToDohDiv(page);

    // Exclusion of the ruble share from Doh-Div.
    const divFailure = question(page, "Doh-Div: nadaljevanje brez teh naložb?");
    await pickWord(divFailure, "exclude_failed_instruments");
    await sendAnswer(divFailure);

    // README, 2023: Doh-KDVP without the ruble share; Doh-Div 7 rows (the IBKR payment
    // in lieu is one); Doh-DHO 1 row; Doh-Obr 15 rows (the IBKR coupon is one).
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await excludedInvestments(page, "Doh-KDVP").getByText("Synthetic Ruble Share (XX0000000121)").count()).toBe(1);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 7,/).count()).toBe(1);
    expect(await cardStatus(page, "Doh-DHO")).toBe("pripravljeno");
    expect(await card(page, "Doh-DHO").getByText(/Vrstice: 1,/).count()).toBe(1);
    expect(await card(page, "Doh-DHO").getByText(/Obresti drugih bank niso vključene/).count()).toBe(1);
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");

    // README, approximate tax 2023: the amount of each form and the total.
    const estimate = await page.getByRole("region", { name: "Približni davek za plačilo" }).textContent();

    expect(estimate).toContain("Doh-KDVP: 28,23 EUR");
    expect(estimate).toContain("Doh-Div: 15,36 EUR");
    expect(estimate).toContain("Doh-DHO: 0,00 EUR");
    expect(estimate).toContain("Doh-Obr: 31,40 EUR");
    expect(estimate).toContain("Skupaj: 74,99 EUR");
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 15,/).count()).toBe(1);
  });

  it("E9 keeps Doh-KDVP when the Doh-Div question is cancelled", async () => {
    const page = await openReportPage(session.context, session.server);

    await runAndAnswerUpToDohDiv(page);
    await cancelAnswer(question(page, "Doh-Div: nadaljevanje brez teh naložb?"));

    // Doh-Div has no file; the unrelated Doh-KDVP keeps its own file.
    expect(await cardStatus(page, "Doh-Div")).toBe("ni uspelo");
    expect(await card(page, "Doh-Div").getByRole("button", { name: /Shrani datoteko/ }).count()).toBe(0);
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");

    const saved = await download(page, card(page, "Doh-KDVP").getByRole("button", { name: "Shrani datoteko Doh-KDVP" }));

    expect(saved.name).toBe("Doh-KDVP_2023.xml");
    expect(saved.text.startsWith("<?xml")).toBe(true);
  });
});
