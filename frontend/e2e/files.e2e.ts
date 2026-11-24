import { describe, expect, it } from "vitest";

import { chooseFilesInArea, manualFile, renamed } from "./support/files";
import {
  card,
  cardStatus,
  download,
  loginContext,
  openAllDetails,
  openReportPage,
  resultRegion,
  startRun,
} from "./support/report_page";
import { answerYear2022, MANUAL_FILE_NAMES } from "./support/scenarios";
import { startServer } from "./support/server";
import { useSession } from "./support/session";

describe("files with the same name and in another order", () => {
  const session = useSession();

  it("F1 numbers two files of the same name and names the right one in a warning", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = [
      renamed(await manualFile("trade_republic_2022.csv"), "export.csv"),
      renamed(await manualFile("trade_republic_2023.csv"), "export.csv"),
    ];

    await startRun(page, { files, year: 2022 });

    // docs/diagnostics.md, "Files with the same name": the list shows the file number.
    const list = await page.locator(".file-list").first().innerText();

    expect(list).toContain("export.csv (datoteka 1)");
    expect(list).toContain("export.csv (datoteka 2)");

    await answerYear2022(page);

    // Only the two Trade Republic files are loaded: Doh-Div 7 rows, Doh-DHO no data, Doh-Obr 14 rows.
    expect(await cardStatus(page, "Doh-KDVP")).toBe("pripravljeno");
    expect(await cardStatus(page, "Doh-DHO")).toBe("ni podatkov");
    expect(await card(page, "Doh-Div").getByText(/Vrstice: 7,/).count()).toBe(1);
    expect(await card(page, "Doh-Obr").getByText(/Vrstice: 14,/).count()).toBe(1);

    // The debit interest of -1.25 is line 33 of the 2022 file (header is line 1), which is file 1.
    await openAllDetails(page);

    // The sources are drawn after the box opens: wait for the line before reading the text.
    await resultRegion(page).getByText("export.csv (datoteka 1), vrstica 33", { exact: false }).waitFor();

    const text = (await resultRegion(page).textContent()) ?? "";

    expect(text).toContain("export.csv (datoteka 1), vrstica 33, ID transakcije manual-2022-054");
    expect(text).not.toContain("export.csv (datoteka 2), vrstica 33");
  });

  it("F2 gives the same Doh-KDVP file for the files in the reverse order", async () => {
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));
    const saveKdvp = async (server: typeof session.server, context: typeof session.context, ordered: typeof files) => {
      const page = await openReportPage(context, server);

      await startRun(page, { files: ordered, year: 2022 });
      await answerYear2022(page);

      expect(await cardStatus(page, "Doh-Div")).toBe("pripravljeno");
      expect(await card(page, "Doh-Obr").getByText(/Vrstice: 14,/).count()).toBe(1);

      return {
        page,
        saved: await download(page, card(page, "Doh-KDVP").getByRole("button", { name: "Shrani datoteko Doh-KDVP" })),
      };
    };

    const normal = await saveKdvp(session.server, session.context, files);

    // The second run has its own server and profile, so no answer of the first one is kept.
    const otherServer = await startServer();

    try {
      const otherContext = await loginContext(session.browser, otherServer);

      try {
        const reversed = await saveKdvp(otherServer, otherContext, [...files].reverse());

        expect(reversed.saved.name).toBe("Doh-KDVP_2022.xml");
        expect(reversed.saved.text).toBe(normal.saved.text);

        // The warning of line 33 still names the 2022 file, now the fourth in the list.
        await openAllDetails(reversed.page);

        // The sources are drawn after the box opens: wait for the line before reading the text.
        await resultRegion(reversed.page).getByText("trade_republic_2022.csv, vrstica 33", { exact: false }).waitFor();

        const text = (await resultRegion(reversed.page).textContent()) ?? "";

        // The names differ, so no number is added (docs/diagnostics.md: only when names collide).
        expect(text).toContain("trade_republic_2022.csv, vrstica 33, ID transakcije manual-2022-054");
        expect(text).not.toContain("(datoteka");
      } finally {
        await otherContext.close();
      }
    } finally {
      await otherServer.stop();
    }
  });

  it("F3 lists an Interactive Brokers file in its own area, which only takes XML files", async () => {
    const page = await openReportPage(session.context, session.server);
    const trArea = page.locator(".file-area", { has: page.getByRole("heading", { name: "Trade Republic" }) });
    const ibkrArea = page.locator(".file-area", { has: page.getByRole("heading", { name: "Interactive Brokers" }) });

    await chooseFilesInArea(page, "Interactive Brokers", [await manualFile("ibkr_2022.xml")]);

    // The file is in the Interactive Brokers list and not in the Trade Republic list.
    expect(await ibkrArea.locator(".file-list").innerText()).toContain("ibkr_2022.xml");
    expect(await trArea.locator(".file-list").count()).toBe(0);

    // Each area filters its file dialog by its own export type and names the export to pick.
    expect(await trArea.locator('input[type="file"]').getAttribute("accept")).toBe(".csv,text/csv");
    expect(await ibkrArea.locator('input[type="file"]').getAttribute("accept")).toBe(
      ".xml,application/xml,text/xml",
    );
    expect(await ibkrArea.getByText("Flex Query (XML)").count()).toBe(1);
  });
});
