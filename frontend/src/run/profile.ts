import type { ProfileJson } from "../api/profile";
import { uiText, uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import { emptyTaxpayerForm, taxpayerFields, type TaxpayerForm } from "./taxpayer";

/**
 * The "Remember my details" box: `off`; `on`; `confirmDelete` (the box was unticked while a file
 * is saved, so the warning is open); `deleting` (the delete call is running).
 */
export type RememberState = "off" | "on" | "confirmDelete" | "deleting";

export type RememberEvent = "tick" | "untick" | "cancel" | "confirm" | "deleted" | "deleteFailed";

/**
 * The next state of the box. Unticking with a saved file opens the warning; without one it only
 * stops the saving. An event that does not fit the state changes nothing.
 */
export function rememberBoxStep(state: RememberState, event: RememberEvent, hasSavedProfile: boolean): RememberState {
  if (state === "off" && event === "tick") {
    return "on";
  }

  if (state === "on" && event === "untick") {
    return hasSavedProfile ? "confirmDelete" : "off";
  }

  // Ticking the box again while the warning is open is the same as Cancel.
  if (state === "confirmDelete" && (event === "cancel" || event === "tick")) {
    return "on";
  }

  if (state === "confirmDelete" && event === "confirm") {
    return "deleting";
  }

  if (state === "deleting" && event === "deleted") {
    return "off";
  }

  if (state === "deleting" && event === "deleteFailed") {
    return "on";
  }

  return state;
}

/** While the warning is open or the delete runs, the run is not sent: the choice comes first. */
export function rememberBlocksRun(state: RememberState): boolean {
  return state === "confirmDelete" || state === "deleting";
}

/** A notice about the saved profile; `fields` are the labels of the fields that were left empty. */
export interface ProfileNotice {
  key: UiTextKey;
  fields?: UiTextKey[];
}

/** What the page takes from the saved profile. */
export interface LoadedProfile {
  form: TaxpayerForm;
  remember: RememberState;
  notices: ProfileNotice[];
}

/**
 * The form, the box and the notices for a saved profile. Without a file the form is empty and the
 * box is off. An unreadable file is a notice. Otherwise each saved text fills its input, the box
 * starts ticked, and the fields the server found invalid (left empty) and an unknown key are
 * named in notices.
 */
export function formFromProfile(profile: ProfileJson): LoadedProfile {
  const form: TaxpayerForm = { ...emptyTaxpayerForm };

  if (!profile.saved) {
    return { form, remember: "off", notices: [] };
  }

  if (profile.unreadable === true) {
    return { form, remember: "off", notices: [{ key: "profileUnreadable" }] };
  }

  for (const { key } of taxpayerFields) {
    const value = profile.taxpayer?.[key];

    if (typeof value === "string") {
      form[key] = value;
    }
  }

  const notices: ProfileNotice[] = [];
  const emptied = taxpayerFields.filter(({ key }) => profile.invalidFields?.includes(key) === true);

  if (emptied.length > 0) {
    notices.push({ key: "profileInvalidFields", fields: emptied.map(({ label }) => label) });
  }

  if (profile.ignoredKeys === true) {
    notices.push({ key: "profileIgnoredKeys" });
  }

  return { form, remember: "on", notices };
}

/** The text of a notice in the language of the page. */
export function noticeText(notice: ProfileNotice, language: Language): string {
  if (notice.fields === undefined) {
    return uiText(language, notice.key);
  }

  const labels = notice.fields.map((label) => uiText(language, label)).join(", ");

  return uiTextWith(language, notice.key, { fields: labels });
}

/** The hint under the box: where the file is, without the user name. */
export function profileLocationText(profile: Pick<ProfileJson, "location">, language: Language): string {
  return profile.location === undefined
    ? uiText(language, "profileHintNoLocation")
    : uiTextWith(language, "profileHint", { location: profile.location });
}

/**
 * The form after the saved profile arrived: a saved text fills only an input that is still empty,
 * so what the user typed meanwhile stays.
 */
export function fillEmptyFields(current: TaxpayerForm, saved: TaxpayerForm): TaxpayerForm {
  const merged = { ...current };

  for (const { key } of taxpayerFields) {
    if (current[key].trim() === "") {
      merged[key] = saved[key];
    }
  }

  return merged;
}
