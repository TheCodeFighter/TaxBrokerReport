import { describe, expect, it } from "vitest";

import { manualFile } from "./support/files";
import { countRequests, openReportPage, startRun } from "./support/report_page";
import { answerYear2022, MANUAL_FILE_NAMES } from "./support/scenarios";
import { useSession } from "./support/session";

const IDLE_MS = 5000;

describe("frontend quality goal", () => {
  const session = useSession();

  it("F15 sends no request while the page waits on a question or shows a finished result", async () => {
    const page = await openReportPage(session.context, session.server);
    const files = await Promise.all(MANUAL_FILE_NAMES.map((name) => manualFile(name)));

    // The result with four open questions: the page waits for the user.
    await startRun(page, { files, year: 2022 });

    const waiting = countRequests(page);

    await page.waitForTimeout(IDLE_MS);

    expect(waiting.count()).toBe(0);

    // The finished result: nothing is polled either.
    await answerYear2022(page);

    const finished = countRequests(page);

    await page.waitForTimeout(IDLE_MS);

    expect(finished.count()).toBe(0);
  });
});
