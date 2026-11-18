/** A file from the server that the user can save: the content and the name the server gave it. */
export interface SavedFile {
  blob: Blob;
  fileName: string;
}

/** The file name of `attachment; filename="taxbroker-support-2024.json"`, or the fallback name. */
export function fileNameOf(response: Response, fallback: string): string {
  const header = response.headers.get("Content-Disposition") ?? "";
  const match = /filename="([A-Za-z0-9._-]+)"/.exec(header);

  return match?.[1] ?? fallback;
}

/** Saves the file through a temporary link: the browser shows its own save dialog or folder. */
export function saveFile(file: SavedFile): void {
  const url = URL.createObjectURL(file.blob);
  const link = document.createElement("a");

  link.href = url;
  link.download = file.fileName;
  document.body.appendChild(link);
  link.click();
  link.remove();
  // Later, so a browser that starts the download after the click still finds the file.
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
