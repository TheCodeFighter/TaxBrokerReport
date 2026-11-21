import { mkdtemp, open, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

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

/** The same file under another name; the bytes are shared, not copied. */
export function renamed(file: UploadFile, name: string): UploadFile {
  return { ...file, name };
}

/** The same file with the first `from` in its text replaced by `to`; a missing `from` is a mistake. */
export function edited(file: UploadFile, from: string, to: string): UploadFile {
  const text = file.buffer.toString("utf8");

  if (!text.includes(from)) {
    throw new Error(`The file ${file.name} does not contain ${JSON.stringify(from)}.`);
  }

  return { ...file, buffer: Buffer.from(text.replace(from, to), "utf8") };
}

/** A file on disk for an input too large for Playwright's buffer (over 50 MB); `remove` deletes it. */
export interface DiskFile {
  path: string;
  remove: () => Promise<void>;
}

const TR_HEADER =
  "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,currency," +
  "original_amount,original_currency,fx_rate,description,transaction_id,counterparty_name,counterparty_iban," +
  "payment_reference,mcc_code\n";

/** One cash deposit row of 2022 with its own transaction ID, padded by `pad` characters. */
function depositRow(index: number, pad: number): string {
  return (
    `2022-01-04T11:00:00Z,2022-01-04,DEFAULT,CASH,CUSTOMER_INBOUND,,,,,,500.00,,,EUR,,,,` +
    `Synthetic deposit${"x".repeat(pad)},big-${index},,,,\n`
  );
}

/**
 * A Trade Republic file of exactly `bytes` bytes: the header, then synthetic deposit rows with
 * unique IDs (no trade, so no tax result), the last row padded to hit the size. It is written to
 * a new temp folder, never to the repository.
 */
export async function tradeRepublicFileOfSize(name: string, bytes: number): Promise<DiskFile> {
  const folder = await mkdtemp(join(tmpdir(), "tbr-e2e-file-"));
  const path = join(folder, name);
  const handle = await open(path, "w");

  try {
    let remaining = bytes - TR_HEADER.length;
    let index = 0;
    let batch = TR_HEADER;

    // Stop while at least one more whole row (plus room for a padded last row) still fits.
    while (remaining >= 2 * depositRow(index, 0).length + 400) {
      const row = depositRow(index, 0);

      batch += row;
      remaining -= row.length;
      index += 1;

      if (batch.length > 4_000_000) {
        await handle.write(batch);
        batch = "";
      }
    }

    const base = depositRow(index, 0).length;

    batch += depositRow(index, remaining - base);
    await handle.write(batch);
  } finally {
    await handle.close();
  }

  return { path, remove: () => rm(folder, { recursive: true, force: true }) };
}

/** Puts the files (contents or paths on disk) into the first file input, the Trade Republic area. */
export async function chooseBrokerFiles(page: Page, files: UploadFile[] | string[]): Promise<void> {
  await page.locator('input[type="file"]').first().setInputFiles(files);
}

/** Puts the files into the last file input of the page (the reference files, in "Druge datoteke"). */
export async function chooseReferenceFiles(page: Page, files: UploadFile[]): Promise<void> {
  await page.locator('input[type="file"]').last().setInputFiles(files);
}
