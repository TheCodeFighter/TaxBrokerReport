/** The part of a click that the handler reads. */
export interface LinkClick {
  defaultPrevented: boolean;
  button: number;
  metaKey: boolean;
  ctrlKey: boolean;
  shiftKey: boolean;
  altKey: boolean;
  /** An element (it has `closest`) or something else, such as the document. */
  target: unknown;
  preventDefault(): void;
}

/**
 * Makes a click on a page link (`href="#/..."`) replace the history entry instead of adding one.
 * A tab with one history entry is the only tab a script may close, so Quit can close it. A click
 * that opens the link elsewhere (middle button, Ctrl, Shift, Alt, Meta) and all other links keep
 * the browser's behaviour. Returns true when it handled the click.
 */
export function replacePageLink(click: LinkClick, replace: (hash: string) => void): boolean {
  if (click.defaultPrevented || click.button !== 0) {
    return false;
  }

  if (click.metaKey || click.ctrlKey || click.shiftKey || click.altKey) {
    return false;
  }

  const target = click.target as Partial<Pick<Element, "closest">> | null;
  const href = target?.closest?.("a[href]")?.getAttribute("href") ?? null;

  if (href === null || !href.startsWith("#/")) {
    return false;
  }

  click.preventDefault();
  replace(href);

  return true;
}
