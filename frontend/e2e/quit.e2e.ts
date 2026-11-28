import { describe, expect, it } from "vitest";

import { openReportPage } from "./support/report_page";
import { useSession } from "./support/session";

describe("quit", () => {
  const session = useSession();

  it("Q11 Cancel keeps the program; Quit and Quit again stops it with exit code 0", async () => {
    const page = await openReportPage(session.context, session.server);

    // Quit asks first; Cancel closes the question and the program keeps running.
    await page.getByRole("button", { name: "Izklopi program" }).click();
    await page.getByText("Neshranjeni rezultati se izgubijo.").waitFor();
    await page.getByRole("button", { name: "Prekliči" }).click();

    expect(await page.getByText("Neshranjeni rezultati se izgubijo.").count()).toBe(0);

    const statusAfterCancel = await page.evaluate(async () => {
      const response = await window.fetch("/api/version", { headers: { "X-TaxBroker": "1" } });

      return response.status;
    });

    expect(statusAfterCancel).toBe(200);

    // The second Quit stops the program: the last screen, and the process ends with code 0.
    await page.getByRole("button", { name: "Izklopi program" }).click();
    await page.getByRole("button", { name: "Izklopi", exact: true }).click();
    await page.getByRole("heading", { name: "Program je izklopljen" }).waitFor();

    expect(await page.getByText("Ta zavihek lahko zaprete.").count()).toBe(1);
    expect(await session.server.exited).toBe(0);
  });

  it("Q12 a call to a server that has stopped shows that the program is no longer running", async () => {
    const page = await openReportPage(session.context, session.server);

    await session.server.stop();

    await page.getByRole("button", { name: "Izklopi program" }).click();
    await page.getByRole("button", { name: "Izklopi", exact: true }).click();
    await page.getByRole("heading", { name: "Program ne teče več" }).waitFor();

    expect(await page.getByText("Zaženite ga znova.").count()).toBe(1);
  });
});

describe("the last page is closed", () => {
  const session = useSession({ native: true });

  // About 25 s in all (a 12 s wait, then the 10 s limit): the test has its own limit (the last
  // argument), so a slow runner does not hit the 60 s default of the browser tests.
  it("Q13 a reload keeps the program; closing the only page stops it with exit code 0", async () => {
    const page = await openReportPage(session.context, session.server);

    // A reload closes the page and opens it again within the 10 seconds: the program keeps running.
    await page.reload();
    await page.getByRole("heading", { name: "Poročilo", level: 1 }).waitFor();

    const stoppedAfterReload = await Promise.race([
      session.server.exited.then(() => true),
      new Promise<boolean>((done) => setTimeout(() => done(false), 12_000)),
    ]);

    expect(stoppedAfterReload).toBe(false);

    // The only page closes: after the 10 seconds the program stops by itself.
    await page.close();

    expect(await session.server.exited).toBe(0);
  }, 120_000);
});
