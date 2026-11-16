/**
 * The limits of one upload, the same numbers as the server's. The browser checks them before it
 * sends, because a refused large body can end in a lost connection that the client would read as
 * "session ended". The server still answers 413 and 400 `too_many_files` on its own.
 */

/** `kMaxInputFileBytes` in the server: 100 MiB for one file. */
export const MAX_FILE_BYTES = 100 * 1024 * 1024;

/** `kMaxUploadFiles` in the server: at most 32 files in one run. */
export const MAX_FILES = 32;
