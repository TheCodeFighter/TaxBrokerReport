import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  answerRatio,
  card,
  cardStatus,
  excludedInvestments,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { useSession } from "./support/session";

const YEARS = ["2022", "2023", "2024", "2025"];

describe("year 2025 of the manual files", () => {
  const session = useSession();

  it("E8 answers the questions of 2025 and leaves out the missing-history share", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(YEARS.map((year) => manualFile(`trade_republic_${year}.csv`)));

    await startRun(page, { files, year: 2025 });

    // Class of the unknown share: capital gains.
    const classification = question(page, "Synthetic Unknown Class Share (XX0000000118)", "Doh-KDVP");
    await pickWord(classification, "capital_gains");
    await sendAnswer(classification);

    // Split ratio 2 : 1 with the date of the row, confirmed.
    const ratio = question(page, "Synthetic Split Share (XX0000000107)");
    await answerRatio(ratio, "2", "1");

    // Quantity of the 2024 sale: the units left.
    await sendAnswer(question(page, "Synthetic Quantity Share (XX0000000109)"));

    // Payer of British American Tobacco: the first row.
    const payer = question(page, "British American Tobacco plc (GB0002875804)");
    await pickWord(payer, "0");
    await sendAnswer(payer);

    // Bond type at maturity: ordinary.
    const bond = question(page, "Synthetic Bond A 2025 (XS0000000201)");
    await pickWord(bond, "ordinary");
    await sendAnswer(bond);

    // Exclusion of the missing-history share from Doh-KDVP.
    const kdvpFailure = question(page, "Doh-KDVP: nadaljevanje brez teh naložb?");
    await pickWord(kdvpFailure, "exclude_failed_instruments");
    await sendAnswer(kdvpFailure);

    // README, 2025: Doh-KDVP without the missing-history share; Doh-Div 7 rows; Doh-DHO 1 row
    // and no reminder (EUR 1,140 is above the EUR 1,000 limit); Doh-Obr 3 rows.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await excludedInvestments(page, "Doh-KDVP").getByText("Synthetic Missing History Share (XX0000000111)").count()).toBe(1);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 7,/).count()).toBe(1);
    expect(await cardStatus(page, "Doh-DHO")).toBe("pripravljeno");
    expect(await card(page, "Doh-DHO").getByText(/Vrstice: 1,/).count()).toBe(1);
    expect(await card(page, "Doh-DHO").getByText(/Obresti drugih bank niso vključene/).count()).toBe(0);
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 3,/).count()).toBe(1);
  });
});
