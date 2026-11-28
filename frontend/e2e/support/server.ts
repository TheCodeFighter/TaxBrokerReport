import { spawn, type ChildProcess } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

/** A running local server of the production build, with its one-time link. */
export interface RunningServer {
  /** The link the server prints (`http://127.0.0.1:<port>/#token=...`); it works once. */
  link: string;
  /** The address without the token, for a reload. */
  origin: string;
  /** Resolves with the exit code when the process ends (null when a signal ended it). */
  exited: Promise<number | null>;
  stop: () => Promise<void>;
}

const START_TIMEOUT_MS = 10_000;

/** The static server that `scripts/build.sh prod` copies to build/static (see scripts/e2e.sh). */
function serverBinary(): string {
  return resolve(process.cwd(), "../build/static/taxbroker_server");
}

/** How the server is started. */
export interface StartOptions {
  /**
   * True: start as a user does from the menu (no `TBR_LAUNCH_FILE`, the launch file in a runtime
   * folder, no browser program on the `PATH`), so the server stops when the last page closes.
   * False: start as in Docker, where the server keeps running when a page closes.
   */
  native: boolean;
}

/**
 * Starts the server on a free port with its own empty folders for the log, the saved profile and
 * the launch file, and waits for the line with the link. The folder is deleted by `stop`.
 */
export async function startServer(options: StartOptions = { native: false }): Promise<RunningServer> {
  const folder = await mkdtemp(join(tmpdir(), "tbr-e2e-"));
  const environment: Record<string, string> = {
    PATH: options.native ? "" : (process.env.PATH ?? ""),
    HOME: folder,
    XDG_DATA_HOME: join(folder, "data"),
    TBR_LOG_FILE: join(folder, "taxbroker.log"),
    TBR_SERVER_PORT: "0",
  };

  if (options.native) {
    environment.XDG_RUNTIME_DIR = join(folder, "run");
  } else {
    environment.TBR_LAUNCH_FILE = join(folder, "launch.html");
  }

  const child: ChildProcess = spawn(serverBinary(), [], {
    env: environment,
    stdio: ["ignore", "pipe", "pipe"],
  });

  const exited = new Promise<number | null>((done) => child.once("exit", (code) => done(code)));

  let output = "";
  let stopping = false;

  const linkPromise = new Promise<string>((resolveLink, reject) => {
    const timer = setTimeout(() => reject(new Error(`The server printed no link. Output: ${output}`)), START_TIMEOUT_MS);

    const onData = (chunk: Buffer) => {
      output += chunk.toString();

      const match = /Open (http:\/\/127\.0\.0\.1:\d+\/#token=\S+) in your browser/.exec(output);

      if (match?.[1] !== undefined) {
        clearTimeout(timer);
        resolveLink(match[1]);
      }
    };

    child.stdout?.on("data", onData);
    child.stderr?.on("data", onData);
    child.once("error", (error) => {
      clearTimeout(timer);
      reject(error);
    });
    child.once("exit", (code) => {
      clearTimeout(timer);

      // After the link was read, a late rejection changes nothing; `stop` ends the process.
      if (!stopping) {
        reject(new Error(`The server stopped with code ${code}. Output: ${output}`));
      }
    });
  });

  let link: string;

  // A failed start has no `stop` for the caller: end the process and delete the folder here.
  try {
    link = await linkPromise;
  } catch (error) {
    stopping = true;
    child.kill("SIGKILL");
    await rm(folder, { recursive: true, force: true });
    throw error;
  }

  const stop = async () => {
    stopping = true;

    // A server that already stopped (a crash during the test) sends no second exit event.
    if (child.exitCode === null && child.signalCode === null) {
      const stopped = new Promise<void>((done) => child.once("exit", () => done()));

      child.kill("SIGTERM");
      await stopped;
    }

    await rm(folder, { recursive: true, force: true });
  };

  return { link, origin: new URL(link).origin, exited, stop };
}
