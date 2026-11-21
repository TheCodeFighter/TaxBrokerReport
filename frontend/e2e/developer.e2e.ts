import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import {
  card,
  cardStatus,
  excludedInvestments,
  download,
  openReportPage,
  pickWord,
  question,
  sendAnswer,
  startRun,
} from "./support/report_page";
import { useSession } from "./support/session";

const FAILURE_TITLE = "Doh-KDVP: nadaljevanje brez teh naložb?";
const CONFIRMATION_TITLE = "NEVARNA datoteka Doh-KDVP: potrdite";

describe("developer export", () => {
  const session = useSession();

  it("E11 leaving out the only investment gives Doh-KDVP no data and no file", async () => {
    const page = await openReportPage(session.context, session.server);

    await startRun(page, { files: [await manualFile("trade_republic_2021_developer_mode.csv")], year: 2021 });

    // Developer mode is off: the failure question has exactly the two normal choices.
    const failure = question(page, FAILURE_TITLE);

    expect(await failure.locator('input[type="radio"]').evaluateAll((radios) =>
      radios.map((radio) => (radio as HTMLInputElement).value))).toEqual(["exclude_failed_instruments", "fail_report"]);

    await pickWord(failure, "exclude_failed_instruments");
    await sendAnswer(failure);

    // Nothing is left for the form (`docs/diagnostics.md`: capital becomes `no_data`), the share
    // is listed as left out, and no card has a file to save.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("ni podatkov");
    expect(await excludedInvestments(page, "Doh-KDVP").getByText("Synthetic Developer Share (XX0000000131)").count()).toBe(1);

    for (const form of ["Doh-KDVP", "Doh-Div", "Doh-DHO", "Doh-Obr"]) {
      expect(await card(page, form).getByRole("button", { name: /Shrani/ }).count()).toBe(0);
    }
  });

  it("E12 developer mode: Cancel returns to the question, Confirm saves the UNSAFE file", async () => {
    const page = await openReportPage(session.context, session.server);

    // Turn the developer mode on before the run.
    await page.getByRole("button", { name: "Razvijalski način" }).click();
    await page.getByRole("checkbox", { name: "Vklopi razvijalski način za ta izračun" }).check();
    await startRun(page, { files: [await manualFile("trade_republic_2021_developer_mode.csv")], year: 2021 });

    // The third, red choice is offered after the two normal ones.
    const failure = question(page, FAILURE_TITLE);

    expect(await failure.locator('input[type="radio"]').evaluateAll((radios) =>
      radios.map((radio) => (radio as HTMLInputElement).value))).toEqual([
      "exclude_failed_instruments",
      "fail_report",
      "developer_export",
    ]);

    // Choosing it opens the red confirmation screen, which lists the investment and writes nothing.
    await pickWord(failure, "developer_export");
    await sendAnswer(failure);

    // It is an alert, not a plain question region.
    const confirmation = page.getByRole("alert", { name: CONFIRMATION_TITLE });

    await confirmation.waitFor();

    expect(await confirmation.getByText("Synthetic Developer Share (XX0000000131)").count()).toBe(1);

    // Cancel: the failure question comes back and there is no file.
    await confirmation.getByRole("button", { name: "Prekliči" }).click();
    await confirmation.waitFor({ state: "detached" });
    await question(page, FAILURE_TITLE).waitFor();

    expect(await card(page, "Doh-KDVP").getByRole("button", { name: /Shrani/ }).count()).toBe(0);
    expect(await cardStatus(page, "Doh-KDVP")).toBe("čaka na odgovor");

    // Again, and Confirm: Doh-KDVP is generated with the unsafe marker and its own file.
    const again = question(page, FAILURE_TITLE);
    await pickWord(again, "developer_export");
    await sendAnswer(again);

    const second = page.getByRole("alert", { name: CONFIRMATION_TITLE });

    await second.waitFor();
    await second.getByRole("button", { name: "Potrdi in napiši nevarno datoteko" }).click();
    await second.waitFor({ state: "detached" });

    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await card(page, "Doh-KDVP").getByText("Nevaren razvijalski izvoz").count()).toBe(1);
    expect(await card(page, "Doh-KDVP").getByText(/Ta Doh-KDVP je NEVARNA/).count()).toBe(1);

    const saved = await download(page, card(page, "Doh-KDVP").getByRole("button", { name: "Shrani NEVARNO datoteko Doh-KDVP" }));

    expect(saved.name).toBe("Doh-KDVP_2021_UNSAFE.xml");
    expect(saved.text.startsWith("<?xml")).toBe(true);
  });
});
