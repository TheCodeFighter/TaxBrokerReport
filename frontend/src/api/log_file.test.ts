import { describe, expect, it, vi } from "vitest";

import { loadLogFile } from "./log_file";

describe("loadLogFile", () => {
  it("asks once for the log and gives its text and the file name of the server", async () => {
    const apiFetch = vi.fn().mockResolvedValue(
      new Response("first line\nsecond line\n", {
        status: 200,
        headers: { "Content-Disposition": 'attachment; filename="taxbroker-log.txt"' },
      }),
    );

    const answer = await loadLogFile(apiFetch);

    expect(apiFetch).toHaveBeenCalledTimes(1);
    expect(apiFetch).toHaveBeenCalledWith("/api/log");
    expect("file" in answer && answer.file.fileName).toBe("taxbroker-log.txt");
    expect("file" in answer && (await answer.file.blob.text())).toBe("first line\nsecond line\n");
  });

  it("uses a fixed file name when the header is missing", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response("line\n", { status: 200 }));

    const answer = await loadLogFile(apiFetch);

    expect("file" in answer && answer.file.fileName).toBe("taxbroker-log.txt");
  });

  it("says there is no log for a 404", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response('{"error":"no_log_file"}', { status: 404 }));

    expect(await loadLogFile(apiFetch)).toEqual({ problem: "no_log" });
  });

  it("says it failed for any other refusal", async () => {
    const apiFetch = vi.fn().mockResolvedValue(new Response("", { status: 500 }));

    expect(await loadLogFile(apiFetch)).toEqual({ problem: "failed" });
  });
});
