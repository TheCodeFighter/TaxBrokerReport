import type { Page } from "playwright-core";

import { pickWord, question, sendAnswer, typeInto } from "./report_page";

/** The manual files of the four years, by name, in the order of the history. */
export const MANUAL_FILE_NAMES = [
  "trade_republic_2022.csv",
  "trade_republic_2023.csv",
  "trade_republic_2024.csv",
  "trade_republic_2025.csv",
];

/**
 * The four answers of 2022 from tests/test_data/manual/README.md: 30-day loss of the Partly
 * Blocked Share yes, of the Loss Share no, treaty text for BR "Article 10", the first payer of
 * NVIDIA.
 */
export async function answerYear2022(page: Page): Promise<void> {
  const partlyBlocked = question(page, "Synthetic Partly Blocked Share (XX0000000104)");
  await pickWord(partlyBlocked, "other_activity");
  await sendAnswer(partlyBlocked);

  const lossShare = question(page, "Synthetic Loss Share (XX0000000103)");
  await pickWord(lossShare, "no_other_activity");
  await sendAnswer(lossShare);

  const treaty = question(page, "Davčna pogodba: BR, 2022");
  await pickWord(treaty, "text");
  await typeInto(treaty, "Besedilo pogodbe (do 100 znakov)", "Article 10");
  await sendAnswer(treaty);

  const payer = question(page, "NVIDIA Corporation (US67066G1040)");
  await pickWord(payer, "0");
  await sendAnswer(payer);
}
