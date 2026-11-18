import type { ApiClient } from "../api/client";
import { localizeNumber } from "./number_format";
import { uiText, type Language, type Texts } from "./ui_texts";

export type ParamKind = "number" | "text" | "term";

export interface MessageEntry extends Texts {
  params: { name: string; kind: ParamKind }[];
}

/** The answer of `GET /api/messages`. */
export interface MessageCatalogue {
  messages: Record<string, MessageEntry>;
  terms: Record<string, Texts>;
}

export type MessageParams = Record<string, string | number>;

/**
 * Returns a function that loads the catalogue once. Every call gets the same answer; a failed
 * load is forgotten, so the next call tries again.
 */
export function createMessageLoader(apiFetch: ApiClient["apiFetch"]): () => Promise<MessageCatalogue> {
  let loading: Promise<MessageCatalogue> | undefined;

  return () => {
    if (loading === undefined) {
      loading = apiFetch("/api/messages")
        .then((response) => {
          if (!response.ok) {
            throw new Error("messages_unavailable");
          }

          return response.json() as Promise<MessageCatalogue>;
        })
        .catch((error: unknown) => {
          loading = undefined;
          throw error;
        });
    }

    return loading;
  };
}

/**
 * The text of a diagnostic in one language, built from its key and parameters. A term is
 * translated from the catalogue, a number is written as plain digits. An unknown key shows
 * the "unknown message" text with the key, never the English sentence of the server.
 */
export function renderDiagnostic(
  catalogue: MessageCatalogue,
  key: string,
  params: MessageParams,
  language: Language,
): string {
  const entry = catalogue.messages[key];

  if (entry === undefined) {
    return `${uiText(language, "unknownMessage")}: ${key}`;
  }

  return entry[language].replace(/\{(\w+)\}/g, (_placeholder, name: string) => {
    const value = params[name];

    if (value === undefined) {
      return "?";
    }

    const kind = entry.params.find((param) => param.name === name)?.kind;

    if (kind === "term") {
      return catalogue.terms[String(value)]?.[language] ?? String(value);
    }

    // A number is written as plain digits, like the server's own sentence: many are years
    // (`{year}` 2024), which digit grouping would turn into "2,024". A decimal number gets the
    // decimal comma in Slovenian.
    return localizeNumber(String(value), language);
  });
}
