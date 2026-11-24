import { describe, expect, it } from "vitest";

import { inlineFile, manualFile } from "./support/files";
import {
  card,
  cardStatus,
  openAllDetails,
  openReportPage,
  pickWord,
  question,
  resultRegion,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { answerYear2022, MANUAL_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const NVIDIA_TITLE = "NVIDIA Corporation (US67066G1040)";

describe("income screens of the manual files", () => {
  const session = useSession();

  it("F7 shows the income rows, the excluded items, the fees and the benefits of 2022", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, year: 2022 });
    await answerYear2022(page);
    await openAllDetails(page);

    // Doh-Div: 7 rows, each with the treaty text of its country or "no relief".
    const dividends = card(page, "Doh-Div").getByRole("table", { name: "Dividende" });

    expect(await dividends.locator("tbody tr").count()).toBe(7);
    expect(await dividends.locator("tr", { hasText: "Apple Inc" }).textContent()).toContain("besedilo iz referenčne datoteke");
    expect(await dividends.locator("tr", { hasText: "Banco Bradesco" }).textContent()).toContain("Vaše besedilo");
    expect(await dividends.locator("tr", { hasText: "Johnson & Johnson" }).textContent()).toContain("brez oprostitve");

    // Doh-Obr: interest on cash (12 rows) and bond coupons (2 rows) in two tables.
    const obr = card(page, "Doh-Obr");

    expect(await obr.getByRole("table", { name: "Obresti na gotovini pri posredniku" }).locator("tbody tr").count()).toBe(12);
    expect(await obr.getByRole("table", { name: "Kuponi obveznic" }).locator("tbody tr").count()).toBe(2);

    // Krka pays the Slovenian tax itself: an excluded item, not a row of Doh-Div.
    const text = (await resultRegion(page).textContent()) ?? "";

    expect(text).toContain("Krka d.d. (SI0031102120) — davek je odtegnil slovenski izplačevalec");
    expect(await dividends.locator("tr", { hasText: "Krka" }).count()).toBe(0);

    // Fees: EUR 3 in three rows and USD 2 in one row (the fee column of the 2022 file).
    const fees = page.getByRole("region", { name: "Stroški in ugodnosti" });

    expect(await fees.textContent()).toContain("EUR 3,00 (število: 3)");
    expect(await fees.textContent()).toContain("USD 2,00 (število: 1)");

    // Benefits: saveback 2.50, stock perk 7.50, bonus 3.00 and referral 20.00 are 33.00 in all.
    expect(await fees.textContent()).toContain("Skupaj v EUR: 33,00");
  });

  it("F8 skips the payer and says that other banks' interest is missing", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, year: 2024 });

    // Payer of NVIDIA: skip. The row is made without the payer and with a warning.
    const payer = question(page, NVIDIA_TITLE);

    // Every question names the broker and the file it is about.
    expect(await payer.textContent()).toContain("Posrednik in datoteka:");
    expect(await payer.textContent()).toContain("Trade Republic: trade_republic_");

    await pickWord(payer, "skip");
    await sendAnswer(payer);

    // Deposit total: above the limit (yes). README: Doh-DHO is made from EUR 427.80.
    const deposit = question(page, "Obresti od depozitov 2024");
    await pickWord(deposit, "above_limit");
    await sendAnswer(deposit);

    await openAllDetails(page);

    expect(await cardStatus(page, "Doh-DHO")).toBe("pripravljeno");
    expect(await card(page, "Doh-DHO").getByText(/bruto 427,8000 EUR/).count()).toBe(1);
    expect(await card(page, "Doh-DHO").getByText(/Obresti drugih bank niso vključene/).count()).toBe(1);

    const dividends = card(page, "Doh-Div");

    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");

    // Two rows without a payer (the row shows the ISIN): the skipped NVIDIA dividend of 2024-03-14
    // and the Yen Share dividend of 2024-07-04, whose payer is not in the reference files (README).
    const noPayer = dividends.locator("tr", { hasText: "izplačevalec ni najden" });

    expect(await noPayer.count()).toBe(2);
    expect(await noPayer.filter({ hasText: "US67066G1040" }).count()).toBe(1);
    expect(await noPayer.filter({ hasText: "XX0000000126" }).count()).toBe(1);
    expect(await dividends.textContent()).toContain("Izplačevalca te dividende ni v referenčnih datotekah ali pa niste izbrali ene od več vrstic.");
  });

  it("F9 asks the payer again when a reference file changes the catalogue", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, year: 2022 });
    await answerYear2022(page);

    expect(await page.getByRole("region", { name: "Vaši odgovori (4)" }).count()).toBe(1);

    // A new run of the same session with a made-up reference file that lists two other NVIDIA
    // rows (a supplied file replaces the built-in rows of its ISINs): the options of the payer
    // question changed, so the old answer is not used and the question comes back.
    const payers = inlineFile(
      "payers.csv",
      "isin,name,tax_number,address,country,instrument_kind,verified_on,isin_source,source,source_revision\n" +
        'US67066G1040,NVIDIA Test Corporation,111111111,"1 Test Street, Test City",US,,,issuer,manual,\n' +
        'US67066G1040,NVIDIA Test Holdings,222222222,"2 Test Street, Test City",US,,,issuer,manual,\n',
    );
    const second = await openReportPage(session.context, session.server);

    await startRun(second, { files, referenceFiles: [payers], year: 2022 });

    const payerQuestion = question(second, NVIDIA_TITLE);

    expect(await payerQuestion.count()).toBe(1);
    expect(await payerQuestion.getByText("NVIDIA Test Corporation", { exact: false }).count()).toBe(1);
    expect(await payerQuestion.getByText("NVIDIA Test Holdings", { exact: false }).count()).toBe(1);

    // A payer question never blocks the form: the row is made without the payer facts meanwhile.
    expect(await cardStatus(second, "Doh-Div")).toBe("pripravljeno");

    // The old answer stays in the list, marked as not used in this result.
    const oldAnswer = second.locator(".recorded-answer", { hasText: "Izplačevalec: NVIDIA" });

    expect(await oldAnswer.textContent()).toContain("ni uporabljen v tem rezultatu");
  });
});
