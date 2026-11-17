# Logging

This project uses spdlog with a lightweight wrapper for structured, thread-safe, and async-capable logging.

## Initialization model

### Important

Logger initialization must happen exactly once.

```cpp
int main() {

    taxbroker::InitializeLogger();

    // application code

    spdlog::shutdown();
}
```

### Rules

* `InitializeLogger()` must be called:

  * exactly once
  * at application startup
  * before spawning worker threads

* `spdlog::shutdown()` must be called:

  * exactly once
  * at application shutdown

### Do NOT:

* Do NOT call `InitializeLogger()` in multiple files
* Do NOT call it inside worker threads
* Do NOT reinitialize logger at runtime

---

## Thread safety

* Logger initialization is protected with `std::call_once`
* Logging is thread-safe (spdlog `_mt` sinks)
* Async logging is enabled by default

---

## Async behavior

* Logging is asynchronous by default
* Uses a shared thread pool
* Prevents logging from blocking CSV parsing threads

### Notes

* If the queue is full, logging will block (backpressure)
* This is intentional to avoid log loss
* Overflow policy is `block`; switch to `overrun_oldest` in code for high-throughput production

---

## Flush behavior

* Logs with level `warn` and above are flushed immediately
* Additionally, logs are flushed every 1 second:

```cpp
spdlog::flush_every(std::chrono::seconds(1));
```

### Important

* In async mode, logs can be lost on crash
* Periodic flushing reduces this risk

---

## Output

### File logging

The log file is `taxbroker.log` in the per-user state folder: `$XDG_STATE_HOME/taxbroker/` when
`XDG_STATE_HOME` is an absolute path, else `~/.local/state/taxbroker/`. The folder is created with
mode `0700`. The file rotates at 5 MiB and the last 3 rotated files are kept
(`taxbroker.1.log` is the newest of them). `TBR_LOG_FILE` replaces the file (Docker uses
`/var/log/taxbroker/taxbroker.log`). Without `TBR_LOG_FILE` and without an absolute `HOME` there is
no file sink: the program goes on, and a warning says so. A file that cannot be opened also does
not stop the program; the warning names no path.

Tokens, the launch file path and the `Cookie` header are never logged.

---

### Stdout logging (optional)

Controlled by:

```
TBR_LOG_STDOUT=1|0
```

Default:

* enabled in debug builds
* disabled in release builds

### Recommendation

* Enable stdout when running in Docker
* Disable stdout for high-performance production runs

---

## Environment variables

* `TBR_LOG_LEVEL` = trace|debug|info|warn|error|critical|off (default `error`, for every build)
* `TBR_LOG_FILE` = log file path
* `TBR_LOG_FILE_MODE` = append|truncate
* `TBR_LOG_ASYNC` = 1|0
* `TBR_LOG_QUEUE_SIZE` = async queue size
* `TBR_LOG_THREADS` = async worker threads
* `TBR_LOG_STDOUT` = enable stdout logging

A value that is not valid (an unknown level, a queue size over 65536, more than 8 threads, ...) is
ignored and the default is used. The logger writes one warning with the name of the variable
(never its value).

---

## What the program logs

The table lists what each level writes, not what is on by default: the default level is `error`,
so a normal run writes nothing but errors. A user who reports a problem raises the level, see
"What to send when something fails".

A bug report is the log file. Every line holds fixed words, counts, statuses and message keys, and
never a value, a name, an ISIN, a file name, a path, a query or a token (R13 in `architecture.md`).

| Level | Line | When |
| --- | --- | --- |
| info | `TaxBrokerReport <version> started: <program> (<build type>, <os> <cpu>)` | the start of the command line (`report`, after `--log-level` is applied) and of the server (`server`); compile-time facts only, no host name, user name or kernel release |
| info | `Server listening on <host>:<port>`, `Server stopped` | start and stop |
| error | the start error (for example `cannot listen: the port is in use or not allowed`) | the server cannot start; the same line goes to the error output |
| warning | `The server listens on a non-loopback address; publish its port on 127.0.0.1 only.` | `TBR_SERVER_HOST` is not a loopback address (Docker) |
| info / warning | `The browser program was started` / `The browser program could not be started; the link in the output works` | the opener of the launch file |
| warning | `Log setting <name> is not valid and is ignored: the default is used` | a wrong `TBR_LOG_*` variable |
| warning | `Request refused: <status> <word>` | the guard: `host_not_allowed`, `bad_target`, `origin_repeated`, `origin_missing`, `origin_not_allowed`, `client_header`, `no_session`, `session_wrong`; an unknown API path (`not_found`) or method (`method_not_allowed`) |
| warning | `Accept failed: <system message>; trying again` | the server cannot accept a connection (for example no free file handle); one line for a series of failures, the server retries every 100 ms |
| warning | `Request not finished: timeout` / `connection_closed` | the body of a request stopped (an upload that stalls or is cancelled); an idle connection that ends writes nothing |
| warning | `Request refused: 413 body_over_the_limit (limit <n>)`, `Request refused: <status> unreadable_request` | the HTTP layer: a declared body over the limit; a request it cannot read (400, 431 header too long, 413 body too long). A refused request (wrong session, host, ...) with a large or chunked body gets the guard's own line and answer instead (`Request refused: 401 session_wrong`), without its body being read |
| info / warning | `Browser login done` / `Login refused: <status> <word>` | the login: `link_used`, `link_expired`, `link_wrong`, `bad_request` |
| warning | `Run request refused: <status> <code> [(<limit or reason>)]` | every refusal of the run API; `bad_taxpayer (<taxpayer key>)` names the fixed key, never the value |
| info | `Run started: year, filing, developer mode, brokers [<broker> x<n>], ecb rate files, reference files` (`Run restarted: ...`) | an upload |
| info | `Run result: revision=<n> status=<s> capitalGains=<s> dividends=<s> bankInterest=<s> otherInterest=<s> requests=<n> errors=<n> warnings=<n>` | after an upload, answers or deletes |
| info | `Run details: requests [<type> x<n>]; errors [<code> x<n>]; warnings [<code> x<n>]; excluded [<reason> x<n>]; no data [<report> <reason> x<n>]; answered [<report> <answer> x<n>]` | the same moments: which questions are open, which diagnostic codes were raised, why an item or a report is left out, and which failure question the user answered |
| info | `Answers accepted: <n> [<type> x<n>]`, `Answers deleted: <n or all>`, `Report downloaded: <report>, revision <n>, <bytes> bytes[, unsafe developer export]`, `Support report downloaded: revision <n>, <bytes> bytes` | the steps of the run API |
| info | `Stage rates: supplied files <n>`, `Stage reference: supplied files <n>`, `Stage parse and merge: sources <n>, events <n>, parse errors <n>, parse warnings <n>, merge errors <n>, merge warnings <n>, conflicts <n> (<ms> ms)`, `Stage year: instruments <n>, capital events <n>, events of the year <n>` (or `no ledger, the year <year> is not supported`), `Calculation done (<ms> ms)` | the load of a run (once per upload) and each calculation (also after an answer); counts and times only |
| debug | `Stage classify: instruments <n>, left out <n>`, `Stage fifo: pools <n>, diagnostics <n>`, `Stage loss and builder: loss requests <n>, blocked ISINs <n>, excluded ISINs <n>, lists <n>`, `Stage income: dividend rows <n>, deposit interest rows <n>, other interest rows <n>`, `Stage xml: forms written <n>` | each calculation; counts only |
| info | `Log level set: <level>` | the "Advanced" section of the Support page; shown when the new level is `info` or `debug` |
| warning | `Log level request refused: <status> <code> [(limit <n>)]` | `400 bad_log_level`, `413 request_too_large (limit 1024)`, `405 method_not_allowed`, `404 not_found` |
| warning | `Report stopped: <reason>` | the command line stopped before the calculation or while writing the files; the reason is a fixed word: `input_files`, `taxpayer_file`, `input_file`, `input_fault (<kind> <message id>)`, `output_folder`, `output_file` or `output_replace`; never a file name. The details are in the error output |
| error | `Report failed unexpectedly (<exception type>)` | the command line stopped on an exception; the text goes to the error output only |
| error | `XML writer of <form> stopped (<exception type>)` | a writer threw; never the text, which can hold a value |
| debug | `Run result requested: revision <n>` | `GET /api/run` |
| info / warning / error | `Profile loaded (fields <n>, invalid <n>)`, `Profile saved`, `Profile deleted` / `Profile unreadable` / `Profile not saved (<exception type>)`, `Profile not deleted (<exception type>)` | the profile routes; never a value or a path |
| warning | `Profile request refused: <status> <code> [(<taxpayer key or limit>)]` | a refused profile call: `400 bad_taxpayer (<key>)`, `413 request_too_large (limit 32768)`, `405 method_not_allowed`, `404 not_found` |
| error | `Run request failed: <route> (<exception type>)`, `A request handler failed: <verb> <step> (<exception type>)` | a handler threw; the text of the exception is never logged |

The parser, the input-file checks and the XML writers log their own errors (source index and row
number and the stable message key, never the row or the English sentence, which can hold a
cell). The command line and the server write the same run lines (`Run started`, the stage lines,
`Run result`, `Run details`), because the application service writes the start and the stages and
one shared function writes the result.

## What to send when something fails

1. Save the support report. On the command line add `--support-report` to the same command: it
   writes `support_<year>.json` into the output folder, also when a rate or reference file stops
   the run. In the page open "Support" in the header and press "Save support report" (`taxbroker-support-<year>.json`).
   It holds the version and build, how many files each broker has, the status of each form, the
   open question types and every diagnostic with its code, message key, stage, field, ISIN and file
   number and row. It holds no amount, unit, price, name, tax number, transaction ID, file name,
   path or time (`diagnostics.md`, "Support report").
2. Read it before you send it: it is a plain text file.
3. Only when the maintainer asks for more detail, run the same command again with
   `taxbroker report ... --log-level info` (or `debug`), or choose "Steps (info)" in the "Advanced"
   section of the Support page and run again. The level lasts for that command, or for that server session;
   the next start is at `error` again. `TBR_LOG_LEVEL=info` does the same for every start. Then send
   the file `taxbroker.log` (and `taxbroker.1.log` if the run was long) from the state folder above.
   It holds the version and the build, the steps with their counts and times, and the diagnostic
   codes. It holds no amount, name, ISIN, tax number, file name or path.

---

## Usage

Logging macros:

```cpp
LOG_TRACE("...");
LOG_DEBUG("...");
LOG_INFO("...");
LOG_WARN("...");
LOG_ERROR("...");
LOG_CRITICAL("...");
```

---

## Design notes

* Logger is initialized once
* Configuration is read from environment variables at startup
* Only the level can change at run time: `--log-level` for one command line run, and
  `GET`/`POST /api/log-level` (the "Advanced" section of the Support page) for the rest of a server session. Every
  level follows the privacy rule, so a higher level holds more lines, never a private fact

---

## Performance considerations

* Avoid excessive logging in tight loops (e.g. per CSV row)
* Prefer aggregated or sampled logs
* Async logging prevents most contention, but queue saturation can still block

---

## Summary

* Initialize once
* Shutdown once
* Do not reconfigure at runtime, except the level
* Use async logging for parallel workloads
* Use stdout only when operationally needed
