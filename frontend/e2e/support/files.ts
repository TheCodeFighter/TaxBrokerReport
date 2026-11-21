import { readFile } from "node:fs/promises";
import { resolve } from "node:path";

import type { Page } from "playwright-core";

/** One file for a file input, in the shape Playwright takes. */
export interface UploadFile {
  name: string;
  mimeType: string;
  buffer: Buffer;
}

/** A synthetic manual test file by name, such as `trade_republic_2022.csv`. */
export async function manualFile(name: string): Promise<UploadFile> {
  const path = resolve(process.cwd(), "../tests/test_data/manual", name);

  return { name, mimeType: "text/csv", buffer: await readFile(path) };
}

/** A small CSV written inside the test. */
export function inlineFile(name: string, text: string): UploadFile {
  return { name, mimeType: "text/csv", buffer: Buffer.from(text, "utf8") };
}

/** Puts the files into the first file input of the page (the Trade Republic area). */
export async function chooseBrokerFiles(page: Page, files: UploadFile[]): Promise<void> {
  await page.locator('input[type="file"]').first().setInputFiles(files);
}
