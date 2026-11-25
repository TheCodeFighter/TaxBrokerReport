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
  resultRegion,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { IBKR_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const YEARS = ["2022", "2023", "2024", "2025"];

describe("year 2024 of the manual files", () => {
  const session = useSession();

  it("E7 answers the questions of 2024, leaves out Bond B and warns about the certificate", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(YEARS.map((year) => manualFile(`trade_republic_${year}.csv`)));
    const ibkrFiles = await Promise.all(IBKR_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, areaFiles: [{ title: "Interactive Brokers", files: ibkrFiles }], year: 2024 });

    // Class of the unknown share: capital gains.
    const classification = question(page, "Synthetic Unknown Class Share (XX0000000118)", "Doh-KDVP");
    await pickWord(classification, "capital_gains");
    await sendAnswer(classification);

    // Bond type of Bond B: discount.
    const bond = question(page, "Synthetic Bond B 2030 (XS0000000202)");
    await pickWord(bond, "discount_or_zero_coupon");
    await sendAnswer(bond);

    // Deposit total: not above the limit (no).
    const deposit = question(page, "Obresti od depozitov 2024");
    await pickWord(deposit, "not_above_limit");
    await sendAnswer(deposit);

    // Reverse split 1 : 10 with the date of the row, confirmed.
    const ratio = question(page, "Synthetic Reverse Split Share (XX0000000108)");
    await answerRatio(ratio, "1", "10");

    // Payer of NVIDIA: the first row.
    const payer = question(page, "NVIDIA Corporation (US67066G1040)");
    await pickWord(payer, "0");
    await sendAnswer(payer);

    // One split booked by two brokers on two dates (Trade Republic 2024-06-10, Interactive Brokers
    // 2024-06-12): the rows are one split, 2 : 1.
    const twoBroker = question(page, "Synthetic Two-Broker Split Share (XX0000000116)");
    await pickWord(twoBroker, "yes");
    await answerRatio(twoBroker, "2", "1");

    // 30-day loss, Dollar Share: yes.
    const loss = question(page, "Synthetic Dollar Share (XX0000000112)");
    await pickWord(loss, "other_activity");
    await sendAnswer(loss);

    // Quantity: the units left, which the field already holds.
    await sendAnswer(question(page, "Synthetic Quantity Share (XX0000000109)"));

    // ISIN change of the renamed share (2024 has a sale of each ISIN): the same company, 1 : 1 and
    // the date of the rows as they are.
    const isinChange = question(
      page,
      "Synthetic Renamed Share (IBKR) (XX0000000134) → Synthetic Renamed Share (IBKR) (XX0000000135)",
    );
    await pickWord(isinChange, "yes");
    expect(await isinChange.getByRole("textbox", { name: "Nove delnice", exact: true }).inputValue()).toBe("1");
    expect(await isinChange.getByRole("textbox", { name: "Datum spremembe" }).inputValue()).toBe("2024-03-04");
    await sendAnswer(isinChange);

    // Exclusion from Doh-Obr (Bond B). The certificate is a derivative: the parser leaves it out, so
    // Doh-KDVP asks nothing about it.
    const obrFailure = question(page, "Doh-Obr: nadaljevanje brez teh naložb?");
    await pickWord(obrFailure, "exclude_failed_instruments");
    await sendAnswer(obrFailure);

    // README, 2024: Doh-KDVP generated, without the certificate, with the two-broker share, the cash takeover and the renamed share in two lists; Doh-Div 5 rows; Doh-DHO no data;
    // Doh-Obr 3 rows without Bond B; two warnings about left-out derivative rows: 2 rows of the
    // certificate (Trade Republic) and 1 row of the option (Interactive Brokers).
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await excludedInvestments(page, "Doh-KDVP").count()).toBe(0);
    // The rows of the two-broker share come from two files: one warning for 2024.
    expect(
      await resultRegion(page)
        .getByText("Ta naložba ima v letu 2024 vrstice korporacijskih dejanj iz več datotek.")
        .count(),
    ).toBe(1);
    // The renamed share is listed twice (the sale before the change under the old ISIN, the one
    // after it under the new ISIN); the warning that the ISIN change was applied is in the list.
    expect(
      await resultRegion(page)
        .getByText("ISIN XX0000000134 je 2024-03-04 postal XX0000000135 v razmerju 1 novih za 1 starih enot, kot ste potrdili.")
        .count(),
    ).toBe(1);
    // The takeover for cash of the Cash Takeover share is a sale; its date is the row's date.
    expect(
      await resultRegion(page)
        .getByText("Enote te naložbe so bile prevzete za gotovino. Prodaja je datirana 2024-09-03")
        .count(),
    ).toBe(1);
    expect(await page.getByText("Synthetic Index Certificate").count()).toBe(0);
    expect(await resultRegion(page).getByText("(število vrstic: 2)").count()).toBe(1);
    expect(await resultRegion(page).getByText("(število vrstic: 1)").count()).toBe(1);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 5,/).count()).toBe(1);
    expect(await cardStatus(page, "Doh-DHO")).toBe("ni podatkov");
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 3,/).count()).toBe(1);
    expect(await excludedInvestments(page, "Doh-Obr").getByText("Synthetic Bond B 2030 (XS0000000202)").count()).toBe(1);

    // The quantity correction of the Quantity Share is listed under its heading in the Doh-KDVP card.
    const corrections = card(page, "Doh-KDVP").locator("h4", { hasText: "Popravljene količine prodaj" }).locator("+ ul");

    expect(await corrections.count()).toBe(1);
    expect(await corrections.textContent()).toContain("XX0000000109");
  });
});
