import { describe, expect, it } from "vitest";

import { emptyTaxpayerForm, taxpayerFieldError, taxpayerFields, taxpayerObject } from "./taxpayer";

describe("taxpayerObject", () => {
  it("trims the text and leaves out empty fields", () => {
    const taxpayer = taxpayerObject({
      ...emptyTaxpayerForm,
      taxNumber: " 12345678 ",
      name: "Synthetic Taxpayer",
      address: "",
      city: "  ",
      birthDate: "1990-01-31",
      email: "",
    });

    expect(taxpayer).toEqual({
      taxNumber: "12345678",
      name: "Synthetic Taxpayer",
      birthDate: "1990-01-31",
      isResident: true,
    });
    expect(Object.keys(taxpayer)).not.toContain("address");
    expect(Object.keys(taxpayer)).not.toContain("city");
  });
});

describe("taxpayerFieldError", () => {
  it("names the input of a field the server refused", () => {
    expect(taxpayerFieldError("taxNumber")).toEqual({ field: "taxNumber", text: "taxpayerErrorTaxNumber" });
    expect(taxpayerFieldError("birthDate")).toEqual({ field: "birthDate", text: "taxpayerErrorBirthDate" });
    expect(taxpayerFieldError("email")).toEqual({ field: "email", text: "taxpayerErrorField" });
  });

  it("gives the general text without a field, or for a key that is not an input", () => {
    expect(taxpayerFieldError(undefined)).toEqual({ field: undefined, text: "taxpayerErrorGeneral" });
    expect(taxpayerFieldError("isResident")).toEqual({ field: undefined, text: "taxpayerErrorGeneral" });
    expect(taxpayerFieldError("constructor")).toEqual({ field: undefined, text: "taxpayerErrorGeneral" });
  });
});

describe("taxpayerFields", () => {
  it("gives the email a text input with the email keyboard, so the browser does not block a run", () => {
    expect(taxpayerFields.find((spec) => spec.key === "email")).toMatchObject({
      type: "text",
      inputMode: "email",
    });
  });
});
