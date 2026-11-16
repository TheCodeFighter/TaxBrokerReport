import type { UiTextKey } from "../i18n/ui_texts";

/** The taxpayer fields of the form, as typed. Only `taxNumber` is required. */
export interface TaxpayerForm {
  taxNumber: string;
  name: string;
  address: string;
  city: string;
  postCode: string;
  postName: string;
  birthDate: string;
  email: string;
  phoneNumber: string;
}

export const emptyTaxpayerForm: TaxpayerForm = {
  taxNumber: "",
  name: "",
  address: "",
  city: "",
  postCode: "",
  postName: "",
  birthDate: "",
  email: "",
  phoneNumber: "",
};

/** One input of the taxpayer form; the keys are the keys of the API's taxpayer object. */
export interface TaxpayerFieldSpec {
  key: keyof TaxpayerForm;
  label: UiTextKey;
  type: "text" | "date" | "tel";
  inputMode?: "numeric" | "email";
  maxLength?: number;
  required?: boolean;
}

export const taxpayerFields: TaxpayerFieldSpec[] = [
  { key: "taxNumber", label: "taxpayerTaxNumber", type: "text", inputMode: "numeric", maxLength: 8, required: true },
  { key: "name", label: "taxpayerName", type: "text" },
  { key: "address", label: "taxpayerAddress", type: "text" },
  { key: "postCode", label: "taxpayerPostCode", type: "text" },
  { key: "postName", label: "taxpayerPostName", type: "text" },
  { key: "city", label: "taxpayerCity", type: "text" },
  { key: "birthDate", label: "taxpayerBirthDate", type: "date" },
  { key: "email", label: "taxpayerEmail", type: "text", inputMode: "email" },
  { key: "phoneNumber", label: "taxpayerPhone", type: "tel" },
];

/**
 * The taxpayer object of the request. Text is trimmed and an empty field is left out (the server
 * refuses an empty string). Only Slovenian tax residents are supported, so `isResident` is true.
 * No rule about a value is repeated here: the server checks them and names the field it refuses.
 */
export function taxpayerObject(form: TaxpayerForm): Record<string, string | boolean> {
  const taxpayer: Record<string, string | boolean> = {};

  for (const { key } of taxpayerFields) {
    const value = form[key].trim();

    if (value !== "") {
      taxpayer[key] = value;
    }
  }

  taxpayer.isResident = true;

  return taxpayer;
}

/** The text for a refused field (`bad_taxpayer` and its `field`); the form marks that input. */
export interface TaxpayerFieldError {
  field: keyof TaxpayerForm | undefined;
  text: UiTextKey;
}

const fieldTexts: Record<keyof TaxpayerForm, UiTextKey> = {
  taxNumber: "taxpayerErrorTaxNumber",
  name: "taxpayerErrorField",
  address: "taxpayerErrorField",
  postCode: "taxpayerErrorField",
  postName: "taxpayerErrorField",
  city: "taxpayerErrorField",
  birthDate: "taxpayerErrorBirthDate",
  email: "taxpayerErrorField",
  phoneNumber: "taxpayerErrorField",
};

export function taxpayerFieldError(field: string | undefined): TaxpayerFieldError {
  if (field !== undefined && Object.prototype.hasOwnProperty.call(fieldTexts, field)) {
    const key = field as keyof TaxpayerForm;

    return { field: key, text: fieldTexts[key] };
  }

  return { field: undefined, text: "taxpayerErrorGeneral" };
}
