import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  answerRatio,
  card,
  cardStatus,
  chooseActions,
  excludedInvestments,
  openReportPage,
  pickWord,
  question,
  resultRegion,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { IBKR_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const YEARS = ["2022", "2023", "2024", "2025"];

describe("year 2025 of the manual files", () => {
  const session = useSession();

  it("E8 answers the questions of 2025 and leaves out the missing-history share", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(YEARS.map((year) => manualFile(`trade_republic_${year}.csv`)));
    const ibkrFiles = await Promise.all(IBKR_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, areaFiles: [{ title: "Interactive Brokers", files: ibkrFiles }], year: 2025 });

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

    // The three split rows of the grouped share: one action of two rows, and one of one row.
    // The ratio questions have the same title, so each is found by its text.
    const identity = question(page, "Synthetic Grouped Split Share (XX0000000132)").filter({
      hasText: "Katere vrstice so eno korporacijsko dejanje?",
    });
    await identity.getByRole("button", { name: "Vsaka vrstica je svoje dejanje" }).click();
    await chooseActions(identity, ["1", "1", "3"]);
    expect(await identity.getByText("Uporabite številke od 1 naprej brez preskoka.").count()).toBe(1);
    expect(await identity.getByRole("button", { name: "Pošlji odgovor" }).isDisabled()).toBe(true);
    await chooseActions(identity, ["1", "1", "2"]);
    expect(await identity.getByText("Uporabite številke od 1 naprej brez preskoka.").count()).toBe(0);
    await sendAnswer(identity);

    // Action 1 (2025-05-12 and 2025-05-14): 2 : 1, 15 units become 30. Action 2 (2025-05-20): 2 : 1.
    await answerRatio(
      question(page, "Synthetic Grouped Split Share (XX0000000132)").filter({ hasText: "2025-05-12" }),
      "2",
      "1",
    );
    await answerRatio(
      question(page, "Synthetic Grouped Split Share (XX0000000132)").filter({ hasText: "2025-05-20" }),
      "2",
      "1",
    );

    // Bond type at maturity: ordinary.
    const bond = question(page, "Synthetic Bond A 2025 (XS0000000201)");
    await pickWord(bond, "ordinary");
    await sendAnswer(bond);

    // Split ratio 2 : 1 of the Interactive Brokers share, with the date of the row, confirmed.
    const ibkrRatio = question(page, "Synthetic US Share (XX0000000115)");
    await answerRatio(ibkrRatio, "2", "1");

    // Bond type at maturity of the Interactive Brokers bond: ordinary.
    const ibkrBond = question(page, "Synthetic IB Bond 2025 (XS0000000203)");
    await pickWord(ibkrBond, "ordinary");
    await sendAnswer(ibkrBond);

    // Exclusion of the missing-history share from Doh-KDVP.
    const kdvpFailure = question(page, "Doh-KDVP: nadaljevanje brez teh naložb?");
    await pickWord(kdvpFailure, "exclude_failed_instruments");
    await sendAnswer(kdvpFailure);

    // README, 2025: the grouped share is in Doh-KDVP (40 units after both splits) with the warning
    // for rows from two files; Doh-KDVP without the missing-history share; Doh-Div 8 rows; Doh-DHO 1 row
    // and no reminder (EUR 1,140 is above the EUR 1,000 limit); Doh-Obr 3 rows.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await excludedInvestments(page, "Doh-KDVP").getByText("Synthetic Missing History Share (XX0000000111)").count()).toBe(1);
    expect(
      await resultRegion(page)
        .getByText("Ta naložba ima v letu 2025 vrstice korporacijskih dejanj iz več datotek.")
        .count(),
    ).toBe(1);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 8,/).count()).toBe(1);
    expect(await cardStatus(page, "Doh-DHO")).toBe("pripravljeno");
    expect(await card(page, "Doh-DHO").getByText(/Vrstice: 1,/).count()).toBe(1);
    expect(await card(page, "Doh-DHO").getByText(/Obresti drugih bank niso vključene/).count()).toBe(0);
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 3,/).count()).toBe(1);
  });
});
