import { describe, expect, it } from "vitest";

import { emptyTaxpayerForm } from "./taxpayer";
import {
  fillEmptyFields,
  formFromProfile,
  noticeText,
  profileLocationText,
  rememberBlocksRun,
  rememberBoxStep,
} from "./profile";

describe("formFromProfile", () => {
  it("fills the saved texts, empties an invalid field and names it in a notice", () => {
    const loaded = formFromProfile({
      saved: true,
      taxpayer: { taxNumber: "12345679", name: "Ana Novak", isResident: true },
      invalidFields: ["birthDate"],
    });

    expect(loaded.form).toEqual({ ...emptyTaxpayerForm, taxNumber: "12345679", name: "Ana Novak", birthDate: "" });
    expect(loaded.remember).toBe("on");
    expect(loaded.notices).toEqual([{ key: "profileInvalidFields", fields: ["taxpayerBirthDate"] }]);
  });

  it("gives an empty form, the box off and no notice when nothing is saved", () => {
    expect(formFromProfile({ saved: false })).toEqual({
      form: emptyTaxpayerForm,
      remember: "off",
      notices: [],
    });
  });

  it("gives an empty form, the box off and one notice for an unreadable file", () => {
    expect(formFromProfile({ saved: true, unreadable: true })).toEqual({
      form: emptyTaxpayerForm,
      remember: "off",
      notices: [{ key: "profileUnreadable" }],
    });
  });

  it("names an unknown key in one general notice, without the key", () => {
    const loaded = formFromProfile({ saved: true, taxpayer: { taxNumber: "12345679" }, ignoredKeys: true });

    expect(loaded.notices).toEqual([{ key: "profileIgnoredKeys" }]);
  });

  it("makes no notice for an invalid residence, which has no input", () => {
    const loaded = formFromProfile({ saved: true, taxpayer: { taxNumber: "12345679" }, invalidFields: ["isResident"] });

    expect(loaded.notices).toEqual([]);
  });

  it("ignores a saved value that is not a text", () => {
    const loaded = formFromProfile({ saved: true, taxpayer: { taxNumber: 12345678, name: "Ana Novak" } });

    expect(loaded.form.taxNumber).toBe("");
    expect(loaded.form.name).toBe("Ana Novak");
  });
});

describe("noticeText", () => {
  it("lists the labels of the emptied fields in the language of the page", () => {
    const notice = { key: "profileInvalidFields" as const, fields: ["taxpayerBirthDate" as const, "taxpayerCity" as const] };

    expect(noticeText(notice, "en")).toBe(
      "Some saved details were not valid and were left empty: Date of birth, City.",
    );
    expect(noticeText(notice, "sl")).toBe(
      "Nekateri shranjeni podatki niso veljavni in so ostali prazni: Datum rojstva, Kraj.",
    );
  });

  it("gives the text of a notice without fields", () => {
    expect(noticeText({ key: "profileUnreadable" }, "en")).toBe(
      "The saved details could not be read and were not used.",
    );
  });
});

describe("profileLocationText", () => {
  it("names the place of the file", () => {
    expect(profileLocationText({ location: "~/.local/share/taxbroker/profile.json" }, "en")).toBe(
      "Stays on this computer only, in: ~/.local/share/taxbroker/profile.json",
    );
  });

  it("names the data folder of the app when the place is not known", () => {
    expect(profileLocationText({}, "en")).toBe("Stays on this computer only, in the app's data folder.");
    expect(profileLocationText({}, "sl")).toBe("Podatki ostanejo samo na tem računalniku, v podatkovni mapi aplikacije.");
  });
});

describe("rememberBoxStep", () => {
  it("opens the warning when a saved profile is unticked", () => {
    expect(rememberBoxStep("on", "untick", true)).toBe("confirmDelete");
  });

  it("only turns the box off when no profile is saved, with no warning", () => {
    expect(rememberBoxStep("on", "untick", false)).toBe("off");
  });

  it("ticks the box without a call", () => {
    expect(rememberBoxStep("off", "tick", true)).toBe("on");
    expect(rememberBoxStep("off", "tick", false)).toBe("on");
  });

  it("goes back to ticked on Cancel", () => {
    expect(rememberBoxStep("confirmDelete", "cancel", true)).toBe("on");
  });

  it("goes back to ticked when the box is ticked again while the warning is open", () => {
    expect(rememberBoxStep("confirmDelete", "tick", true)).toBe("on");
  });

  it("starts the delete on Confirm", () => {
    expect(rememberBoxStep("confirmDelete", "confirm", true)).toBe("deleting");
  });

  it("is off after a delete and ticked again after a failed delete", () => {
    expect(rememberBoxStep("deleting", "deleted", true)).toBe("off");
    expect(rememberBoxStep("deleting", "deleteFailed", true)).toBe("on");
  });

  it("ignores an event that does not fit the state", () => {
    expect(rememberBoxStep("off", "confirm", true)).toBe("off");
    expect(rememberBoxStep("on", "cancel", true)).toBe("on");
    expect(rememberBoxStep("deleting", "untick", true)).toBe("deleting");
  });
});

describe("rememberBlocksRun", () => {
  it("blocks the run only while the warning is open or the delete runs", () => {
    expect(rememberBlocksRun("confirmDelete")).toBe(true);
    expect(rememberBlocksRun("deleting")).toBe(true);
    expect(rememberBlocksRun("on")).toBe(false);
    expect(rememberBlocksRun("off")).toBe(false);
  });
});

describe("fillEmptyFields", () => {
  it("fills an empty input and keeps what the user typed", () => {
    const current = { ...emptyTaxpayerForm, name: "Typed Name", taxNumber: "  " };
    const saved = { ...emptyTaxpayerForm, name: "Saved Name", taxNumber: "12345679" };

    expect(fillEmptyFields(current, saved)).toEqual({ ...emptyTaxpayerForm, name: "Typed Name", taxNumber: "12345679" });
  });
});
