import { describe, expect, it } from "vitest";

import { inlineFile, manualFile, tradeRepublicFileOfSize } from "./support/files";
import {
  card,
  cardStatus,
  countRequests,
  download,
  fillRunForm,
  openReportPage,
  resultRegion,
  startRun,
} from "./support/report_page";
import { answerYear2022, MANUAL_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

/** Exactly 100 MiB, the limit of one file (`kMaxInputFileBytes` in the server). */
const MAX_FILE_BYTES = 104_857_600;

describe("upper limits", () => {
  const session = useSession();

  it("F10 accepts 32 files and refuses 33 before sending", async () => {
    const page = await openReportPage(session.context, session.server);
    const requests = countRequests(page, "/api/run");
    const file = await manualFile("trade_republic_2022.csv");

    // One file alone is the baseline: 7 warnings, 3 errors (the three open loss questions).
    await startRun(page, { files: [file], year: 2022 });

    const baseline = await resultRegion(page).innerText();

    expect(baseline).toContain("Napake: 3, opozorila: 7");

    // 32 copies of it: the exact duplicates merge into the same events, so the errors and the
    // state of every form are the same. Each file reports its own parse warning once, so the
    // warning "unsupported row type" is repeated 32 times instead of once: 7 + 31 = 38.
    const second = await openReportPage(session.context, session.server);
    const secondRequests = countRequests(second, "/api/run");

    await startRun(second, { files: Array.from({ length: 32 }, () => file), year: 2022 });

    const many = await resultRegion(second).innerText();

    expect(await second.locator(".file-list").first().innerText()).toContain("trade_republic_2022.csv (datoteka 32)");
    expect(many).toContain("Napake: 3, opozorila: 38");
    expect(many).toContain("32-krat");

    // The 2022 file alone: the loss questions hold Doh-KDVP; Doh-DHO has no data before
    // 2023-12-06 (the interest of 2022 goes to Doh-Obr); the income questions never block.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("čaka na odgovor");
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await cardStatus(page, "Doh-DHO")).toBe("ni podatkov");
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");

    expect(await cardStatus(second, "Doh-KDVP")).toBe("čaka na odgovor");
    expect(await cardStatus(second, "Doh-Div")).toBe("pripravljeno");
    expect(await cardStatus(second, "Doh-DHO")).toBe("ni podatkov");
    expect(await cardStatus(second, "Doh-Obr")).toBe("pripravljeno");

    expect(secondRequests.count()).toBe(1);

    // 33 files: the page refuses before it sends anything.
    const third = await openReportPage(session.context, session.server);
    const thirdRequests = countRequests(third, "/api/run");

    await fillRunForm(third, { files: Array.from({ length: 33 }, () => file), year: 2022 });
    await third.getByRole("button", { name: "Ustvari poročila" }).click();
    await third.getByText("Datotek je preveč. Naenkrat jih je lahko največ 32.").waitFor();

    expect(thirdRequests.count()).toBe(0);

    // The first page (one file) sent its one run and nothing after it.
    expect(requests.count()).toBe(1);
  });

  it("F11 accepts a file of exactly 100 MiB", async () => {
    const page = await openReportPage(session.context, session.server);
    const file = await tradeRepublicFileOfSize("big.csv", MAX_FILE_BYTES);

    try {
      await startRun(page, { files: [file.path], year: 2022 });

      // The rows are deposits: no trade, so no form has data, and nothing was refused.
      const text = await resultRegion(page).innerText();

      expect(text).toContain("Vsa poročila so pripravljena.");
      expect(await cardStatus(page, "Doh-KDVP")).toBe("ni podatkov");
      expect(await cardStatus(page, "Doh-Div")).toBe("ni podatkov");
      expect(await cardStatus(page, "Doh-DHO")).toBe("ni podatkov");
      expect(await cardStatus(page, "Doh-Obr")).toBe("ni podatkov");
    } finally {
      await file.remove();
    }
  });

  it("F12 refuses one byte more in the page, and the server answers 413 when the page is bypassed", async () => {
    const page = await openReportPage(session.context, session.server);
    const requests = countRequests(page, "/api/run");
    const file = await tradeRepublicFileOfSize("big.csv", MAX_FILE_BYTES + 1);

    try {
      await fillRunForm(page, { files: [file.path], year: 2022 });
      await page.getByRole("button", { name: "Ustvari poročila" }).click();
      await page.getByText("Datoteka big.csv je prevelika. Ena datoteka je lahko velika največ 100 MB.").waitFor();

      expect(requests.count()).toBe(0);
    } finally {
      await file.remove();
    }

    // A file one byte over the limit (zeros), sent by the page's own fetch without the page check.
    const answer = await page.evaluate(async (bytes) => {
      const form = new FormData();

      form.append(
        "request",
        JSON.stringify({
          reportingYear: 2022,
          filingType: "original",
          taxpayer: { taxNumber: "12345679", isResident: true },
          files: [{ part: "file0", kind: "broker", broker: "trade_republic" }],
        }),
      );
      form.append("file0", new Blob([new Uint8Array(bytes)]), "big.csv");

      const response = await fetch("/api/run", { method: "POST", headers: { "X-TaxBroker": "1" }, body: form });

      return { status: response.status, body: await response.text() };
    }, MAX_FILE_BYTES + 1);

    expect(answer.status).toBe(413);
    expect(answer.body).toContain("file_too_large");
  });

  it("F13 shows 2,000 warnings in time and the page keeps answering", async () => {
    const header =
      "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,currency," +
      "original_amount,original_currency,fx_rate,description,transaction_id,counterparty_name,counterparty_iban," +
      "payment_reference,mcc_code\n";
    let text = header;

    for (let index = 0; index < 2000; index += 1) {
      text += `2022-06-15T10:00:00Z,2022-06-15,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,-1.25,,,EUR,,,,Synthetic debit interest,neg-${index},,,,\n`;
    }

    const page = await openReportPage(session.context, session.server);
    const started = Date.now();

    await startRun(page, { files: [inlineFile("negative.csv", text)], year: 2022 });

    expect(Date.now() - started).toBeLessThan(5000);

    // The 2,000 equal warnings are one group with a count and 2,000 details.
    const result = resultRegion(page);

    expect(await result.innerText()).toContain("Napake: 0, opozorila: 2000");
    expect(await result.getByText("2000-krat", { exact: false }).count()).toBe(1);
    expect(await result.getByText("Podrobnosti (2000)").count()).toBe(1);

    // The page answers: opening the 2,000 sources (built only when opened) shows the last one
    // within one second.
    const details = result.locator("details", { hasText: "Podrobnosti (2000)" });
    const clicked = Date.now();

    await details.locator("summary").click();
    await details.locator("li").nth(1999).waitFor();

    expect(Date.now() - clicked).toBeLessThan(1000);
    expect(await details.locator("li").count()).toBe(2000);
  });

  it("F14 keeps a name and an address of 200 characters in the XML", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));
    const name = "N".repeat(200);
    const address = "A".repeat(200);

    await startRun(page, { files, year: 2022, taxpayer: { "Ime in priimek": name, Naslov: address } });
    await answerYear2022(page);

    // The FURS schema has no length limit for the taxpayer's name and address (xs:string).
    const saved = await download(page, card(page, "Doh-KDVP").getByRole("button", { name: "Shrani datoteko Doh-KDVP" }));

    expect(saved.text).toContain(`<edp:name>${name}</edp:name>`);
    expect(saved.text).toContain(`<edp:address1>${address}</edp:address1>`);
  });
});
