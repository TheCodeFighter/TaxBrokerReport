import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  cancelAnswer,
  card,
  cardStatus,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { useSession } from "./support/session";

describe("answers", () => {
  const session = useSession();

  it("E4 refuses an answer from an old page and refreshes its questions", async () => {
    const pageA = await openReportPage(session.context, session.server);
    const pageB = await openReportPage(session.context, session.server);
    const file = await manualFile("trade_republic_2022.csv");

    // Page A shows the first result. Page B then sends the same file: a new run of the same
    // session, so the revision that page A still shows is out of date.
    await startRun(pageA, { files: [file], year: 2022 });
    await startRun(pageB, { files: [file], year: 2022 });

    // Page A answers the Loss Share question with the old revision. It was in the background, and
    // a background tab does not draw, so the page is brought to the front first.
    await pageA.bringToFront();

    const lossShare = question(pageA, "Synthetic Loss Share (XX0000000103)");
    await pickWord(lossShare, "no_other_activity");
    await lossShare.getByRole("button", { name: "Pošlji odgovor" }).click();

    // The server refuses it: page A says the questions were refreshed and records no answer.
    await pageA.getByRole("status").filter({ hasText: "Rezultat se je spremenil; vprašanja so osvežena." }).waitFor();

    expect(await pageA.getByRole("region", { name: /Vaši odgovori/ }).count()).toBe(0);
    expect(await question(pageA, "Synthetic Loss Share (XX0000000103)").count()).toBe(1);
    expect(await cardStatus(pageA, "Doh-KDVP")).toBe("čaka na odgovor");

    // The refreshed question works: the same answer is now accepted.
    const refreshed = question(pageA, "Synthetic Loss Share (XX0000000103)");
    await pickWord(refreshed, "no_other_activity");
    await sendAnswer(refreshed);

    expect(await pageA.getByRole("region", { name: "Vaši odgovori (1)" }).count()).toBe(1);
  });

  it("E10 cancel on a loss question blocks Doh-KDVP only, and a cancelled failure question fails it", async () => {
    const page = await openReportPage(session.context, session.server);

    await startRun(page, { files: [await manualFile("trade_republic_2022.csv")], year: 2022 });

    // Cancel the Loss Share question: its sale cannot be reported, and Doh-KDVP now asks the
    // failure question that lists it.
    await cancelAnswer(question(page, "Synthetic Loss Share (XX0000000103)"));

    const failure = question(page, "Doh-KDVP: nadaljevanje brez teh naložb?");

    expect(await failure.getByText("Synthetic Loss Share (XX0000000103)").count()).toBe(1);
    expect(await cardStatus(page, "Doh-KDVP")).toBe("čaka na odgovor");

    // The income forms keep their own status.
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
    expect(await cardStatus(page, "Doh-Obr")).toBe("pripravljeno");

    // Answer the other two loss questions with "no", then cancel the failure question too.
    const partlyBlocked = question(page, "Synthetic Partly Blocked Share (XX0000000104)");
    await pickWord(partlyBlocked, "no_other_activity");
    await sendAnswer(partlyBlocked);

    const december = question(page, "Synthetic December Share (XX0000000106)");
    await pickWord(december, "no_other_activity");
    await sendAnswer(december);

    await cancelAnswer(question(page, "Doh-KDVP: nadaljevanje brez teh naložb?"));

    // README: `cancel` blocks. Doh-KDVP failed, with no file.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("ni uspelo");
    expect(await card(page, "Doh-KDVP").getByRole("button", { name: /Shrani datoteko/ }).count()).toBe(0);
    expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
  });
});
