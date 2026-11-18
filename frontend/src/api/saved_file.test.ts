import { describe, expect, it } from "vitest";

import { fileNameOf } from "./saved_file";

describe("fileNameOf", () => {
  it("takes the plain name from the Content-Disposition header", () => {
    const response = new Response("", { headers: { "Content-Disposition": 'attachment; filename="Doh-KDVP_2025.xml"' } });

    expect(fileNameOf(response, "fallback.xml")).toBe("Doh-KDVP_2025.xml");
  });

  it("uses the fallback when the header is missing", () => {
    expect(fileNameOf(new Response(""), "fallback.xml")).toBe("fallback.xml");
  });

  it("uses the fallback for a name with a slash or a space", () => {
    const response = new Response("", { headers: { "Content-Disposition": 'attachment; filename="../x y.xml"' } });

    expect(fileNameOf(response, "fallback.xml")).toBe("fallback.xml");
  });
});
