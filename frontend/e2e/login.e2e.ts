import { describe, expect, it } from "vitest";

import { newContext } from "./support/browser";
import { openReportPage } from "./support/report_page";
import { useSession } from "./support/session";

describe("login", () => {
  const session = useSession();

  it("E1 the link works once: the second browser profile is refused", async () => {
    // The session fixture already used the link in its own profile: the report page opens.
    const page = await openReportPage(session.context, session.server);

    expect(await page.getByRole("heading", { name: "Poročilo", level: 1 }).count()).toBe(1);

    // The same link in a second profile (no cookie): the page says it is no longer valid.
    const second = await newContext(session.browser);
    const secondPage = await second.newPage();

    await secondPage.goto(session.server.link);
    await secondPage.getByRole("heading", { name: "Povezava ne velja več" }).waitFor();

    expect(await secondPage.getByRole("heading", { name: "Poročilo", level: 1 }).count()).toBe(0);

    await second.close();

    // The first profile still works after a reload (the session cookie, no token in the address).
    await page.reload();
    await page.getByRole("heading", { name: "Poročilo", level: 1 }).waitFor();
  });
});
