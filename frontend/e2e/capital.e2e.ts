import { describe, expect, it } from "vitest";

import { edited, manualFile } from "./support/files";
import {
  answerRatio,
  card,
  cardStatus,
  countRequests,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
  typeInto,
} from "./support/report_page";
import { MANUAL_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const SPLIT_TITLE = "Synthetic Split Share (XX0000000107)";

describe("capital screens of the manual files", () => {
  const session = useSession();

  it("F3 lists the broker record of the split on the ratio screen", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = [await manualFile("trade_republic_2022.csv"), await manualFile("trade_republic_2023.csv")];

    await startRun(page, { files, year: 2023 });

    // Row 4 of trade_republic_2023.csv: the SPLIT row of 2023-01-17 with the ID manual-2023-002.
    // The page prints the date as it is in the result (ISO), with the units of the row.
    const ratio = question(page, SPLIT_TITLE);

    expect(await ratio.getByText("Trade Republic, 2023-01-17, ID posla manual-2023-002").count()).toBe(1);
    expect(await ratio.getByText("Količina v zapisu: 5.").count()).toBe(1);
  });

  it("F4 refuses an invalid ratio before sending, and a changed source reopens the question", async () => {
    const page = await openReportPage(session.context, session.server);
    const requests = countRequests(page, "/api/run/answers");
    const file2022 = await manualFile("trade_republic_2022.csv");
    const file2023 = await manualFile("trade_republic_2023.csv");

    await startRun(page, { files: [file2022, file2023], year: 2023 });

    const ratio = question(page, SPLIT_TITLE);

    // New shares 0: the field message shows, nothing is confirmable, nothing is sent.
    await typeInto(ratio, "Nove delnice", "0");
    await typeInto(ratio, "Stare delnice", "1");

    expect(await ratio.getByText(/Vnos ne da delitve\./).count()).toBe(1);
    expect(await ratio.getByRole("checkbox").count()).toBe(0);
    expect(await ratio.getByRole("button", { name: "Pošlji odgovor" }).isDisabled()).toBe(true);

    // New shares as text: the same.
    await typeInto(ratio, "Nove delnice", "abc");
    await typeInto(ratio, "Stare delnice", "1");

    expect(await ratio.getByText(/Vnos ne da delitve\./).count()).toBe(1);
    expect(await ratio.getByRole("checkbox").count()).toBe(0);
    expect(await ratio.getByRole("button", { name: "Pošlji odgovor" }).isDisabled()).toBe(true);

    expect(requests.count()).toBe(0);

    // 2 : 1 with the date of the row, confirmed: the answer is recorded.
    await answerRatio(ratio, "2", "1");

    expect(requests.count()).toBe(1);
    expect(await page.getByRole("region", { name: "Vaši odgovori (1)" }).count()).toBe(1);

    // Control: a new run of the same session with the same files keeps the answer.
    const same = await openReportPage(session.context, session.server);

    await startRun(same, { files: [file2022, file2023], year: 2023 });

    expect(await question(same, SPLIT_TITLE).count()).toBe(0);

    // The split row changed from 5 to 6 units: the recorded ratio belongs to the old record
    // (docs/fifo.md, "New or changed source records invalidate it"), so the question is open again.
    const changedPage = await openReportPage(session.context, session.server);
    const changed = edited(file2023, "XX0000000107,5,", "XX0000000107,6,");

    await startRun(changedPage, { files: [file2022, changed], year: 2023 });

    expect(await question(changedPage, SPLIT_TITLE).count()).toBe(1);
    expect(await cardStatus(changedPage, "Doh-KDVP")).toBe("čaka na odgovor");
  });

  it("F5 refuses a quantity above the units left, then corrects the sale", async () => {
    const page = await openReportPage(session.context, session.server);
    const requests = countRequests(page, "/api/run/answers");
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));

    await startRun(page, { files, year: 2024 });

    // The other questions that Doh-KDVP of 2024 needs (README, 2024).
    const classification = question(page, "Synthetic Unknown Class Share (XX0000000118)", "Doh-KDVP");
    await pickWord(classification, "capital_gains");
    await sendAnswer(classification);

    await answerRatio(question(page, "Synthetic Reverse Split Share (XX0000000108)"), "1", "10");

    const loss = question(page, "Synthetic Dollar Share (XX0000000112)");
    await pickWord(loss, "other_activity");
    await sendAnswer(loss);

    const failure = question(page, "Doh-KDVP: nadaljevanje brez teh naložb?");
    await pickWord(failure, "exclude_failed_instruments");
    await sendAnswer(failure);

    const sentBefore = requests.count();

    // The quantity question: 1 unit sold, 0.99999999 bought. 1 is above the units left.
    const quantity = question(page, "Synthetic Quantity Share (XX0000000109)");

    await typeInto(quantity, "Količina prodaje (enote)", "1");

    expect(await quantity.getByText(/Največ 0,99999999 enot/).count()).toBe(1);
    expect(await quantity.getByRole("button", { name: "Pošlji odgovor" }).isDisabled()).toBe(true);
    expect(requests.count()).toBe(sentBefore);

    // The units left are accepted; the sale is recalculated.
    await typeInto(quantity, "Količina prodaje (enote)", "0.99999999");
    await sendAnswer(quantity);

    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");

    const corrections = card(page, "Doh-KDVP").locator("h4", { hasText: "Popravljene količine prodaj" }).locator("+ ul");

    expect(await corrections.textContent()).toContain("XX0000000109, prodaja 2024-09-10: 1,00000000 → 0,99999999 enot");
  });

  it("F6 shows the January hint of a December loss, and no question once the January file is added", async () => {
    const page = await openReportPage(session.context, session.server);
    const file2022 = await manualFile("trade_republic_2022.csv");

    await startRun(page, { files: [file2022], year: 2022 });

    // README: the 2022-12-20 loss of the December Share has its window in January 2023; the
    // 2022 file alone does not show a purchase there, so the app asks and hints.
    const december = question(page, "Synthetic December Share (XX0000000106)");

    expect(await december.getByText("Nakup v januarju naslednjega leta lahko to izgubo blokira; dodajte ta izvoz, če ga imate.").count()).toBe(1);

    // With the 2023 file the purchase of 2023-01-05 blocks the loss: no question.
    const second = await openReportPage(session.context, session.server);

    await startRun(second, { files: [file2022, await manualFile("trade_republic_2023.csv")], year: 2022 });

    expect(await question(second, "Synthetic December Share (XX0000000106)").count()).toBe(0);
  });
});
