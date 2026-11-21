import { describe, expect, it } from "vitest";

import { inlineFile, manualFile } from "./support/files";
import {
  card,
  cardStatus,
  fillRunForm,
  openReportPage,
  resultRegion,
  startRun,
} from "./support/report_page";
import { useSession } from "./support/session";

describe("run form", () => {
  const session = useSession();

  it("E2 checks the form: no file, a wrong check digit, a short and an empty tax number", async () => {
    const page = await openReportPage(session.context, session.server);
    const runRequests: string[] = [];
    const submit = page.getByRole("button", { name: "Ustvari poročila" });
    const taxNumber = page.getByRole("textbox", { name: "Davčna številka" });

    page.on("request", (request) => {
      if (request.method() === "POST" && new URL(request.url()).pathname === "/api/run") {
        runRequests.push(request.url());
      }
    });

    // No broker file: the button is off, so nothing can be sent.
    expect(await submit.isDisabled()).toBe(true);

    // A tax number of 8 digits with a wrong check digit: a hint, but the run still starts.
    await fillRunForm(page, { files: [await manualFile("trade_republic_2022.csv")], year: 2022 });
    await taxNumber.fill("12345678");

    expect(await page.getByText("Kontrolna številka ni pravilna.").count()).toBe(1);

    await submit.click();
    await resultRegion(page).waitFor();

    expect(runRequests.length).toBe(1);

    // A short tax number: the server refuses it and the page marks the field.
    await taxNumber.fill("1234");
    await submit.click();
    await page.locator("#error-taxNumber").waitFor();

    expect(await page.locator("#error-taxNumber").innerText()).toBe("Vnesite osem števk davčne številke.");
    expect(await taxNumber.getAttribute("aria-invalid")).toBe("true");
    expect(runRequests.length).toBe(2);

    // An empty tax number: the browser's own check stops the form, so no request is sent.
    await taxNumber.fill("");
    await submit.click();

    expect(await taxNumber.evaluate((input: HTMLInputElement) => input.validity.valueMissing)).toBe(true);
    expect(await taxNumber.evaluate((input: HTMLInputElement) => input.form?.checkValidity())).toBe(false);
    expect(runRequests.length).toBe(2);
  });

  it("E3 shows a file that is not a Trade Republic export as a failed run without downloads", async () => {
    const page = await openReportPage(session.context, session.server);

    await startRun(page, {
      files: [inlineFile("not_an_export.csv", "not,a,trade,republic,export\n1,2,3,4,5\n")],
      year: 2022,
    });

    // Every form failed, the message names the problem, and nothing can be saved.
    expect(await resultRegion(page).getByText("Poročil ni bilo mogoče pripraviti.").count()).toBe(1);

    for (const form of ["Doh-KDVP", "Doh-Div", "Doh-DHO", "Doh-Obr"]) {
      expect(await cardStatus(page, form)).toBe("ni uspelo");
      expect(await card(page, form).getByRole("button", { name: /Shrani datoteko/ }).count()).toBe(0);
    }

    expect(await resultRegion(page).getByText(/Datoteka ni izvoz transakcij Trade Republic/).count()).toBe(1);
  });
});
