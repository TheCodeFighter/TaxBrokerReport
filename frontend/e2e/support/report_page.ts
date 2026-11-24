import { readFile } from "node:fs/promises";

import type { Browser, BrowserContext, Locator, Page } from "playwright-core";

import { newContext } from "./browser";
import { chooseBrokerFiles, chooseFilesInArea, chooseReferenceFiles, type UploadFile } from "./files";
import type { RunningServer } from "./server";

/** The made-up tax number of tests/test_data/cli/taxpayer.json (it has a correct check digit). */
export const TAX_NUMBER = "12345679";

/**
 * A browser profile that is logged in with the one-time link of the server. The link works once,
 * so a test file logs in once here and opens a new page of this context for each scenario.
 */
export async function loginContext(browser: Browser, server: RunningServer): Promise<BrowserContext> {
  const context = await newContext(browser);
  const page = await context.newPage();

  await page.goto(server.link);
  await page.getByRole("heading", { name: "Tax Broker Report", level: 1 }).waitFor();
  await page.close();

  return context;
}

/** A new page of a logged-in context, on the report page. */
export async function openReportPage(context: BrowserContext, server: RunningServer): Promise<Page> {
  const page = await context.newPage();

  await page.goto(`${server.origin}/#/report`);
  await page.getByRole("heading", { name: "Poročilo", level: 1 }).waitFor();

  return page;
}

/** What a run needs from the form. */
export interface RunInput {
  /** File contents, or paths of files on disk for a very large file. */
  files: UploadFile[] | string[];
  /** Files added to the file area with the given title, after the first area's files. */
  areaFiles?: { title: string; files: UploadFile[] }[];
  year: number;
  /** Reference files (payers and treaty texts), added in "Druge datoteke". */
  referenceFiles?: UploadFile[];
  /** Extra taxpayer fields by their visible label, such as `{ "Ime in priimek": "Test" }`. */
  taxpayer?: Record<string, string>;
}

/** Fills the form, presses "Ustvari poročila" and waits until the result is shown. */
export async function startRun(page: Page, input: RunInput): Promise<void> {
  await fillRunForm(page, input);
  await page.getByRole("button", { name: "Ustvari poročila" }).click();
  await resultRegion(page).waitFor();
}

/** Fills the files, the year and the taxpayer, and sends nothing. */
export async function fillRunForm(page: Page, input: RunInput): Promise<void> {
  await chooseBrokerFiles(page, input.files);

  for (const area of input.areaFiles ?? []) {
    await chooseFilesInArea(page, area.title, area.files);
  }

  if (input.referenceFiles !== undefined) {
    await chooseReferenceFiles(page, input.referenceFiles);
  }

  await page.getByRole("combobox", { name: "Leto poročanja" }).selectOption(String(input.year));
  await page.getByRole("textbox", { name: "Davčna številka" }).fill(TAX_NUMBER);

  for (const [label, value] of Object.entries(input.taxpayer ?? {})) {
    await page.getByRole("textbox", { name: label, exact: true }).fill(value);
  }
}

/** The "Rezultat" part of the page. */
export function resultRegion(page: Page): Locator {
  return page.getByRole("region", { name: "Rezultat", exact: true });
}

/** The card of one form, by its code: `Doh-KDVP`, `Doh-Div`, `Doh-DHO` or `Doh-Obr`. */
export function card(page: Page, form: string): Locator {
  return page.getByRole("region", { name: form, exact: true });
}

/** The list "Izpuščene naložbe" (investments left out) of one card; it has no match when empty. */
export function excludedInvestments(page: Page, form: string): Locator {
  return card(page, form).locator("h4", { hasText: "Izpuščene naložbe" }).locator("+ ul");
}

/** The status word on a card (`pripravljeno`, `ni podatkov`, `čaka na odgovor`, `ni uspelo`). */
export async function cardStatus(page: Page, form: string): Promise<string> {
  return (await card(page, form).locator(".badge").first().innerText()).trim();
}

/**
 * One open question by its title (an investment with its ISIN, or the title of the screen). The
 * same investment can be asked about on two forms; `form` then names the card that holds it.
 */
export function question(page: Page, title: string, form?: string): Locator {
  const scope = form === undefined ? page : card(page, form);

  return scope.getByRole("region", { name: title, exact: true });
}

/** The ratio question: the ratio new : old, the date of the row as it is, and the confirmation. */
export async function answerRatio(scope: Locator, newShares: string, oldShares: string): Promise<void> {
  await scope.getByRole("textbox", { name: "Nove delnice", exact: true }).fill(newShares);
  await scope.getByRole("textbox", { name: "Stare delnice", exact: true }).fill(oldShares);
  await scope.getByRole("checkbox").check();
  await sendAnswer(scope);
}

/** Picks the radio button with this server word, such as `no_other_activity`. */
export async function pickWord(scope: Locator, word: string): Promise<void> {
  await scope.locator(`input[type="radio"][value="${word}"]`).check();
}

/** Types into the text field with this label inside the scope. */
export async function typeInto(scope: Locator, label: string, text: string): Promise<void> {
  await scope.getByRole("textbox", { name: label, exact: true }).fill(text);
}

/** Presses "Pošlji odgovor" and waits until the answered question has left the page. */
export async function sendAnswer(scope: Locator): Promise<void> {
  await scope.getByRole("button", { name: "Pošlji odgovor" }).click();
  await scope.waitFor({ state: "detached" });
}

/** Presses "Prekini vprašanje" and then "Da, prekini vprašanje"; waits until the question is gone. */
export async function cancelAnswer(scope: Locator): Promise<void> {
  await scope.getByRole("button", { name: "Prekini vprašanje" }).click();
  await scope.getByRole("button", { name: "Da, prekini vprašanje" }).click();
  await scope.waitFor({ state: "detached" });
}

/** The saved file of a click: its name and its text. */
export interface SavedFile {
  name: string;
  text: string;
}

/** Presses the button and returns the file that the page saves. */
export async function download(page: Page, button: Locator): Promise<SavedFile> {
  const saved = page.waitForEvent("download");

  await button.click();

  const file = await saved;
  const path = await file.path();

  return { name: file.suggestedFilename(), text: await readFile(path, "utf8") };
}

/** Counts the requests the page sends to the server whose path contains `part`, from now on. */
export function countRequests(page: Page, part = "/api/"): { count: () => number } {
  let seen = 0;

  page.on("request", (request) => {
    if (new URL(request.url()).pathname.includes(part)) {
      seen += 1;
    }
  });

  return { count: () => seen };
}

/** Opens every "Podrobnosti" (details) element of the page, so that the sources of the diagnostics show. */
export async function openAllDetails(page: Page): Promise<void> {
  for (const element of await page.locator("details").all()) {
    await element.evaluate((node) => {
      (node as HTMLDetailsElement).open = true;
    });
  }
}
