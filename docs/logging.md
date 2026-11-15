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

### File logging (always enabled)

Logs are written to:

```
logs/taxbroker.log
```

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

* `TBR_LOG_LEVEL` = trace|debug|info|warn|error|critical
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

## What the server logs

A bug report is the log file. Every line holds fixed words, counts, statuses and message keys, and
never a value, a name, an ISIN, a file name, a path, a query or a token (R13 in `architecture.md`).

| Level | Line | When |
| --- | --- | --- |
| info | `Server listening on <host>:<port>`, `Server stopped` | start and stop |
| info / warning | `The browser program was started` / `The browser program could not be started; the link in the output works` | the opener of the launch file |
| warning | `Log setting <name> is not valid and is ignored: the default is used` | a wrong `TBR_LOG_*` variable |
| warning | `Request refused: <status> <word>` | the guard: `host_not_allowed`, `bad_target`, `origin_repeated`, `origin_missing`, `origin_not_allowed`, `client_header`, `no_session`, `session_wrong`; an unknown API path (`not_found`) or method (`method_not_allowed`) |
| info / warning | `Browser login done` / `Login refused: <status> <word>` | the login: `link_used`, `link_expired`, `link_wrong`, `bad_request` |
| warning | `Run request refused: <status> <code> [(<limit or reason>)]` | every refusal of the run API |
| info | `Run started: year, filing, developer mode, brokers [<broker> x<n>], ecb rate files, reference files` (`Run restarted: ...`) | an upload |
| info | `Run result: revision=<n> status=<s> capitalGains=<s> dividends=<s> bankInterest=<s> otherInterest=<s> requests=<n> errors=<n> warnings=<n>` | after an upload, answers or deletes |
| info | `Run details: requests [<type> x<n>]; errors [<code> x<n>]; warnings [<code> x<n>]` | the same moments: which questions are open and which diagnostic codes were raised |
| info | `Answers accepted: <n>`, `Answers deleted: <n or all>`, `Report downloaded: <report>, revision <n>, <bytes> bytes[, unsafe developer export]` | the steps of the run API |
| debug | `Run result requested: revision <n>` | `GET /api/run` |
| error | `Run request failed: <route> (<exception type>)`, `A request handler failed: <verb> <step> (<exception type>)` | a handler threw; the text of the exception is never logged |

The parser, the input-file checks and the XML writers log their own errors (source index and row
number, never the row). The processors and the command line write no log line of their own yet.

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

* Logger is initialized once and treated as immutable
* Configuration is read from environment variables at startup
* Runtime reconfiguration is NOT supported

---

## Performance considerations

* Avoid excessive logging in tight loops (e.g. per CSV row)
* Prefer aggregated or sampled logs
* Async logging prevents most contention, but queue saturation can still block

---

## Summary

* Initialize once
* Shutdown once
* Do not reconfigure at runtime
* Use async logging for parallel workloads
* Use stdout only when operationally needed
