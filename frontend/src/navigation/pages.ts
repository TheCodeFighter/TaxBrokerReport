/** The pages of the application. The run screen is the page `report`. */
export type Page = "home" | "report" | "manual" | "about";

/** The pages in the order of the header links. */
export const pages: readonly Page[] = ["home", "report", "manual", "about"];

/** The address hash of each page: a hash, so the back button and a reload work without the server. */
const hashes: Record<Page, string> = {
  home: "#/",
  report: "#/report",
  manual: "#/manual",
  about: "#/about",
};

/** The address hash of a page. */
export function hashOf(page: Page): string {
  return hashes[page];
}

/** The page of an address hash; anything unknown, a login link included, is the home page. */
export function pageFromHash(hash: string): Page {
  for (const page of pages) {
    if (hashes[page] === hash) {
      return page;
    }
  }

  return "home";
}
