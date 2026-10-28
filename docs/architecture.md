# Architecture

TaxBrokerReport is intended to run entirely on the user's machine. The C++20 backend owns broker
parsing, validation, tax processing, and XML generation. A browser frontend will call the local
backend once an HTTP framework and frontend stack are selected.

The repository does not currently define a frontend framework or an HTTP transport. Code should
therefore keep domain results independent of JSON and avoid designing transport-specific endpoint
classes prematurely.

## Imported event metadata

Every imported trade, corporate action, dividend, interest payment, benefit, and private-market
event owns the same `EventMetadata`:

- `mTaxDate` is the broker-provided calendar date used for tax reporting. It is not derived from
  the source timestamp.
- `mSourceTimestamp` is an optional UTC instant with millisecond precision. Fractions finer than
  milliseconds are truncated.
- `mOrderingTimestamp` optionally supplies an explicitly assumed ordering instant. When absent,
  ordering uses `mSourceTimestamp`. Keep the original source instant for overlap/conflict checks.
- `mSources` contains every contributing `SourceReference`, each with the broker, source filename,
  source row, optional transaction ID, and stable input sequence. A parsed event starts with one
  source. An exact duplicate adds its source to the retained event instead of replacing provenance.

These types depend only on the C++ standard library. `SourceFilename::fromPath` removes both POSIX
and Windows directory components and rejects empty, `.` and `..` basenames, so event metadata
cannot retain an absolute host path. Source rows are one-based logical rows; for CSV input, the
header is row 1 and the first data row is row 2. A logical row is one CSV record. A quoted field
that spans several text lines is still one record, so a row number can differ from the line number
shown in a text editor.

A CSV record whose field count differs from the header is rejected with a `field_count_mismatch`
error on its own row, before any field is read. Later rows keep their correct numbers. A file with
an unclosed quote is rejected as a whole with a file-level `parse_error`, because the reader would
otherwise merge the following records into one field without any error. The check counts quote
characters, so two unclosed quotes in one file cancel out. The reader then merges rows into one
record. Such a record has the wrong field count and a line break inside a field, so the whole file
is rejected with a file-level `parse_error`. A quoted line break inside a record with the right
field count is valid and stays one row.

The header is checked before any row is read. The Trade Republic parser needs these 18 columns:
`datetime`, `date`, `category`, `type`, `asset_class`, `name`, `symbol`, `shares`, `price`,
`amount`, `fee`, `tax`, `currency`, `original_amount`, `original_currency`, `fx_rate`,
`description` and `transaction_id`. The other export columns are not used and may be missing, and
extra columns are allowed. A header that misses a required column, for example because the file
was saved with semicolons, is rejected with one file-level `invalid_header` error and no rows are
read. A file of 0 bytes stays a `parse_error`.

A row with a rejected field is skipped, and every invalid field of that row gets its own
diagnostic, in the column order of the export. A sign check (shares or amount against the trade
side) runs after the field checks and only on valid fields. A missing `datetime` is reported
first, and the other fields of that row are still checked.

Broker numbers must be plain dot decimals (see `calculations.md`). A comma or parentheses makes the
field `invalid_value` and skips the row. From 3 such rows in one file, the parser adds one
file-level `localized_number_format` warning that suggests downloading the export again. A row
counts once, even if several of its fields have a comma.

An empty line after the header is never a record and gets no row number, however many follow each
other. A line with only spaces or commas is a record and goes through the normal row checks.
Trade Republic exports contain neither.

Different selected files may have the same sanitized filename. The application therefore derives
a run-local source ID from the file's request position: `source-0001`, `source-0002`, and so on.
The frontend keeps the selected-file list in that same order. When filenames collide, it shows a
label such as `taxreport.csv (file 2)` instead of exposing either file's directory. The source ID
identifies an upload only within that report run and does not change event ordering or duplicate
detection.

When duplicate names would still be confusing, the frontend may let the user add a run-local
display label such as `2022` or `pension account`. The frontend keeps that label locally with the
source ID and may show `taxreport.csv (2022, file 1)`. It does not send the label as broker data,
use it for processing, or replace the source ID with it.

Trade Republic requires a non-empty `datetime` for imported events. A missing value produces a
`MissingField` error on `datetime` and excludes that row before instrument or event creation.
The diagnostic retains the source file, row and transaction ID for the user to review. Other
valid rows remain available. Ignored cash movements do not require a timestamp.

An invalid non-empty TR timestamp retains the otherwise valid event, leaves `mSourceTimestamp`
empty, and reports an `InvalidValue` warning that exact transaction order is unknown. The tax
date remains unchanged.

When a valid TR timestamp's original calendar date differs from `date`, report an
`InconsistentValue` warning on `datetime`. Retain `date` for reporting and set
`mOrderingTimestamp` to that date at 09:00:00 UTC. The hour is defined by
`kTrDateMismatchDefaultHour` in `traderepublic_parser.hpp`. The warning explicitly states
that the default hour is assumed and FIFO/corporate-action order may be incorrect. Preserve the
original `mSourceTimestamp`, so equal fallback times cannot hide conflicting exports.
Compare before UTC normalization: crossing midnight while normalizing an offset is not itself
a mismatch. Do not silently discard purchases or replace the broker tax date.

### Trade Republic income, trades and optional broker FX

Dividends, broker cash interest and bond interest share income validation. When
`original_currency` is supplied, preserve `original_amount` in that currency; otherwise use
`amount` and `currency`. Do not replace an invalid original amount with the local amount.
Preserve the tax date, source timestamp and complete source reference alongside the income.
Withholding remains separate in `currency`; blank tax is unknown, while explicit zero is known.

An empty `fx_rate` never rejects otherwise valid foreign income. Store no broker rate, including
when the foreign payment is supplied directly in `amount`/`currency`. Do not invent a unit rate
or infer a rate from the local and original amounts. Direct EUR income without broker FX keeps
its existing identity conversion; it does not establish a foreign-currency fallback.

A supplied `fx_rate` is validated even without `original_currency`. Malformed, whitespace-only,
zero, negative, out-of-range or nonzero values rounded to zero produce an `InvalidValue` error
on `fx_rate` and skip that row. The diagnostic retains filename, row and transaction ID without
repeating the value. Other valid rows and their diagnostics remain available through merging.
Valid source rates retain the established eight-decimal import rounding; a positive rate that
rounds to one rate tick is representable. This numeric validation does not verify rate direction.

Required income facts remain required even when broker FX is absent. Empty fields produce
`MissingField`; invalid supplied fields produce `InvalidValue`. Trade Republic's required
`datetime` policy above applies to income as well.

A trade follows the same broker-rate rule. A trade gets a broker rate only from `fx_rate`, except
the identity rate 1.0 for a plain EUR trade without `fx_rate`; any other trade without `fx_rate`
has no broker rate. When `original_currency` is supplied, the unit price and `original_amount` are
in that currency and the fee stays in `currency`; the trade keeps the fee currency separately.
Trade Republic trades in EUR today, so this shape is assumed from foreign income, not verified
from a foreign trade sample. With `original_currency`, `original_amount` is required: an empty
value is `MissingField` and skips the row. An invalid supplied `fx_rate`, `original_currency` or
`original_amount` is an `InvalidValue` error on that field and skips the row. Blocking foreign
trades from a report is a later processor decision.

For a trade with `original_currency`, a supplied `amount` must still be a valid number; it is not
stored. An invalid value is an `InvalidValue` error on `amount` and skips the row. An empty
`amount` stays allowed. Foreign income stays lenient: it ignores `amount` when `original_currency`
is supplied.

### Stable input sequence

The stable input sequence is scoped to the complete input request. `mSourceIndex` is the zero-based
position of the source file in the request, and `mEventIndex` is the zero-based source-order
position assigned by that source's parser. The current CSV parsers use the data-row position as the
event index. Parser completion order is never used, so files can be parsed concurrently without
changing event order.

The pair must uniquely identify each imported event in a request. It is the final ordering key and
does not represent broker time. Application JSON may expose only the derived source ID, never the
raw source index.

### Equality and duplicate identity

`EventMetadata` equality compares every metadata field, including the complete source collection.
This exact value equality is separate from duplicate detection. Source references are presented in
stable input-sequence, filename and row order; the earliest source is the event's primary ordering
source.

A transaction identity consists of the broker and transaction ID. The same ID from different
brokers therefore identifies different transactions. Matching identities mark duplicate
candidates even when their filename, row, or stable sequence differs. A candidate is an exact
duplicate only when its event kind, tax date, timestamp, instrument, and event-specific values also
match; otherwise it is a conflict. Events without transaction IDs are not automatically
deduplicated.

Trade and corporate-action quantities retain source-linked `UnitSourceEvidence`: original decimal
text, discarded fractional digits, exact canonical source value and rounding direction. Evidence
from every exact duplicate is kept in stable source order. Canonical source quantities compare
numerically, so extra trailing zeros do not create a conflict. Different exact source quantities
that round to the same stored units do create a conflict. An input without evidence can still
match stored facts, but supplies no rounding proof; known evidence across the complete identity
group must agree even when its first event has no evidence.

### Deterministic ordering

Events are normally ordered by tax date, effective timestamp presence, effective timestamp value,
and stable input sequence, in that order. The effective timestamp uses `mOrderingTimestamp` when
present, otherwise `mSourceTimestamp`. A default ordering time is not evidence of execution time.
On the same tax date, timestamped events precede events without timestamps. Broker, filename, row,
and transaction ID are not ordering fallbacks. The placement of
untimestamped events is a deterministic policy and does not claim that they occurred after every
timestamped event.

Tax processing has one event-kind priority: a supported split or reverse split is applied before
every purchase or sale for the same ISIN on its effective tax date. Trades on that date use the
adjusted position. Several actions for the same ISIN and date use reliable source timestamps. If
their order cannot be established, stable input sequence keeps the diagnostics deterministic but
must not be used to guess the result. The processor reports the ambiguity and leaves that ISIN
unprocessed.

## Ownership of pipeline data

Each pipeline stage either passes data on or computes new data, and its signature shows which:

- A stage that passes the same data on takes it by `&&` or by value and moves it. Nothing is
  copied on the way.
- A stage that computes new data from inputs that are still needed later reads them by `const&`
  and returns a new object.

| Stage | Takes | Why |
| --- | --- | --- |
| Parser | returns `ParseResult` by value | the result is new data |
| `StatementMerger::merge` | `std::vector<StatementMergeInput>&&` | events move into the merged statement |
| `TaxProcessor::prepareReportingYear` | `TaxProcessingRequest` by value | moves the merged statement into the result; the ledger holds only references into it |
| `FifoMatcher::match` | `const&` statement and ledger | computes matches; both are still needed |
| `CapitalGainsReportBuilder::build` | `const&` statement, ledger and FIFO result | computes report rows; all are still needed |
| `KdvpXmlWriter::write` | `const&` metadata and report | computes the XML text; the report is still needed |
| `ApplicationResultBuilder::build` | `ApplicationResultInput&&` | moves each report's XML into the result; the diagnostics become new application diagnostics |

A copy is made only where a caller still needs the data afterwards, and it is written out at the
call site.

## Statement-merger contract

`StatementMerger` is a transport-independent operation over parsed domain results. Each
`StatementMergeInput` carries the source file's request index separately from the collection's
arrival order. Request indices must be unique and must agree with the stable source indices on the
input's events. The merger may therefore receive parse results in task-completion order while still
producing the same result. A repeated or inconsistent source index is an input error and must be
reported; it is never repaired from vector position, filename or broker.

The merger takes the inputs by move (`std::vector<StatementMergeInput>&&`). Their events and parse
diagnostics are moved into the result, so each event exists once. A caller that still needs a
`ParseResult` after the merge, such as a debug tool that prints the parser diagnostics, passes an
explicit copy.

The operation is single-threaded. Its result owns one `BrokerStatement` presentation structure and
a separate chronological sequence of `StatementEventReference` values into that structure. Event
payloads and provenance are stored only in the presentation structure. A reference contains the
event kind, event index and an instrument index when that collection is instrument-based. Benefit
and private-market references have no instrument index.

Presentation order is independent of economic processing order:

1. collections appear as trades, dividends, interest, benefits and private-market events;
2. instrument collections use their broker-neutral identity, with deterministic identity and name
   ordering; and
3. events within an instrument use the normal event chronology.

Trade and dividend instruments use ISIN as their identity. Interest instruments use interest type
plus ISIN when one exists; without an ISIN, interest type plus payer or instrument name is the
available broker-neutral identity. Benefits and private-market records remain standalone events.
Names never replace an ISIN. A valid trade, action, dividend or bond coupon with a known ISIN and
blank display name is retained with a `MissingField` warning on `name`. Presentation uses the
earliest non-empty supplied name for the same ISIN under `StableSourceOrder`. If every supplied name
is blank.
identity, a warning retains every name variant and source, including original blanks.

Trades and actions retain source-linked `InstrumentSourceEvidence` with the original name, class
label and mapped class on each event. Dividends and bond coupons retain the same evidence for name
completion; broker cash interest keeps its established broker identity. Instrument summaries never
replace these original facts. Older in-memory inputs without evidence use their instrument fields;
missing evidence is not proof of an original label.

Valid trade/action facts are retained when `asset_class` is blank or unrecognized. Blank classes
produce a `MissingField` error; unrecognized labels produce `UnsupportedAssetClass`, both on
`asset_class`, with messages saying the event was retained. Required identity, date/datetime,
quantity and monetary validation still rejects unusable rows before classification evidence is
created. Recognized Crypto and PrivateFund categories retain their mapped class and existing
unsupported-treatment warnings.

A trade/action class disagreement retains every event and source and leaves the instrument class
`Unknown`. Within one CSV the parser reports `InconsistentValue`; merging reports
`InstrumentAssetClassConflict` from surviving event evidence in every file layout. Parser diagnostics
may therefore differ with file layout, but retained facts, unresolved classification and error state
do not. A later Stock row cannot clear an earlier Stock/Fund conflict. Missing/unrecognized evidence
also stays unresolved alongside a known class. Distinct unrecognized labels remain separate
`Unknown` variants, with `mUnrecognizedLabel` in merger diagnostics; original recognized labels
remain in event evidence. Sources and presentation follow the normal ordering rules.

Before presentation grouping, the merger classifies every valid event with a non-empty transaction
ID by `(broker, transaction ID)`. Events in one identity group are exact duplicates only when all
of these values match:

- event kind, tax date, optional source timestamp and optional ordering timestamp;
- broker-neutral instrument identity and tax-relevant classification; and
- every event-specific monetary, unit, ratio, currency, fee, tax and descriptive value.

Trade/action duplicate comparison uses each event's original mapped class, never its instrument's
summary. When both inputs retain unrecognized labels, different labels conflict even though both
map to `Unknown`. Exact duplicates combine all original evidence and sources. Accumulated evidence
must agree across the whole group, including when its first candidate has no source evidence.
Duplicate classification happens before instrument aggregation; rejected same-ID groups cannot
contaminate the class of unrelated surviving events.

For an ISIN-identified instrument, a display-name difference does not make the events conflict;
the normal instrument-name warning retains each name and its sources. An exact duplicate produces
one event under its earliest `StableSourceOrder` source and combines every contributing source
reference. When two or more of the merged copies come from the same file (the same request
position, not the same filename), the merger also adds one `DuplicateTransactionInFile` warning
per such file. Copies are counted per event, not per source reference, so one event that already
carries several rows of its file is not a repeat, and copies with the same rows count as one copy.
The warning lists the source references of that file's copies, so the user sees every repeated
row, and it carries no raw rows or values. Copies from different files add no warning, because
loading the same export twice must count once. If any candidate in the identity group differs, no
candidate enters the merged ledger and one `ConflictingDuplicate` error retains all sources and
involved event kinds; the group then gets no `DuplicateTransactionInFile` warning. Missing or empty
transaction IDs never trigger this classification. Different transaction IDs do not establish
economic equivalence, including for corporate actions.

After transaction classification, instrument collections are ordered by their documented identity,
and events inside each concrete collection use `ChronologicalEventOrder`.

The chronological reference sequence crosses every presentation collection and uses tax date,
effective timestamp presence/value and stable input sequence. It does not process one broker or
one presentation collection to completion first. The merger does not apply the processing-only
same-day corporate-action priority, calculate FIFO, or resolve economic corporate-action identity.
Collection aggregation and chronological-reference construction are separate implementation
phases. The merger populates one reference per retained event after presentation ordering is final.
Consumers use this sequence, not collection traversal, as the chronological ledger. The sequence
retains all supplied years; reporting-year preparation belongs to the later processing stage.

Parser and merger diagnostics share the result without changing parser diagnostics. A
`SourcedParseDiagnostic` contains the original `ParseDiagnostic` plus its broker and source request
index. Merger diagnostics have their own typed codes and retain every relevant source reference.
Parsing diagnostics precede merging diagnostics. Parser diagnostics use file request order and
preserve each parser's diagnostic creation order. Merger diagnostics use earliest source request
index, earliest source event index, tax date, ISIN, severity (errors first), diagnostic code enum
order, then deterministic creation order; missing source, event, date and ISIN values sort last.
Messages are never sorting keys. Report scopes and final application diagnostic IDs are added
later: after all pipeline stages are combined, the full report-aware ordering from
[`diagnostics.md`](diagnostics.md) applies.

Input outcomes are defined as follows:

- no inputs produce empty presentation and chronological collections with no diagnostics;
- a row-level parse error preserves the parser's valid events and diagnostic;
- a file-level parse failure contributes its diagnostics and no events, while other inputs remain
  available;
- when every file fails, merged domain data is empty and every parser error remains visible; and
- inputs from different known brokers are accepted and combined through the same broker-neutral
  result.

The merger result has no report status. The later application service derives report and overall
statuses after applying each diagnostic's scope. Empty merged data is therefore not itself an error,
and parser errors cannot disappear merely because another input supplied valid data.

Broker values whose meaning is not verified remain source data, not calculated tax inputs. In
particular, a Trade Republic split row's decimal `shares` value does not establish the split ratio.
The application result asks the frontend for the action's new-shares-to-old-shares ratio. Only a
validated user confirmation turns that action into a processable split or reverse split.

The current `CorporateAction` model uses `UnresolvedSplit` with an empty `mRatio` for
unverified split evidence. In that state, `mUnitsDelta` stores the signed quantity rounded to
`Units` precision; it is not a verified change in inventory. `mUnitEvidence` retains the original
signed decimal, canonical exact value, discarded digits and source reference. Neither a positive
nor a negative quantity establishes economic direction. Explicitly resolved evidence uses `Split`
with a known `2 / 1` ratio or `ReverseSplit` with a known `1 / 10` ratio, for example. Direction
comes from the verified new-shares-to-old-shares ratio, never from the source quantity's sign.
`mRatio` is an exact `SplitRatio` (two whole numbers, new and old shares), never a ratio rounded
to the `CorpRatio` scale, so a broker that supplies a verified ratio can keep it exact.

Current consumers preserve this distinction:

- the Trade Republic parser creates unresolved actions and preserves signed source evidence;
- the statement merger compares type, quantity, exact source value and optional ratio, retaining
  each source through deduplication without resolving an action;
- the parser dump labels an unresolved value as `source_quantity` and the ratio as unresolved;
- the exact-arithmetic dump shows labeled hypothetical previews or an explicit caller-supplied
  preview ratio, without establishing the real economics from the CSV quantity.

Ratio confirmation belongs to the corporate-action stage ("Corporate-action contract" below);
the remaining identity rules and inventory application belong to the corporate-action processor
described in `fifo.md`. A resolved input supplied to a merger test represents explicit synthetic
evidence; it does not demonstrate broker verification or resolution.

After transaction deduplication, economic corporate-action identity is resolved separately across
all sources. One confirmed action changes all open lots of its ISIN across brokers exactly once.
Grouping by ISIN and calendar year is used only for
`corporate_action_sources_mismatch_possible`: always warn when action records for that ISIN/year
come from multiple source files or brokers, and show every date, type, ratio and source. The year
is never an action identity or a reason to combine two distinct actions. Ambiguous grouping returns
`corporate_action_identity_required`; a ratio prompt covers the combined action's complete source
set. The detailed identity, ordering and conflict rules are in `fifo.md`.

The processing request also retains a separate analysis view for 30-day loss eligibility, including
required previous-December and next-January events. It does not mutate the selected-year closing
FIFO position. Partially eligible losses become separate report rows, without duplicating their
inventory consumption. The user's answer to the loss question is an explicit input of the
decision session, as defined in `tax_rules.md` and `diagnostics.md`.

### Reporting-year preparation contract

`TaxProcessor::prepareReportingYear` accepts a `TaxProcessingRequest` containing the optional
selected-year integer and a merger-produced `StatementMergeResult`. The returned
`TaxProcessingResult` owns that complete merged result once, including all upstream diagnostics,
source facts and years. Its candidate views contain `StatementEventReference` indices into the
owned presentation, so copying or moving the result does not invalidate them. Keep that presentation
unchanged while using the indices.

A missing year or a value outside 2000..9999 returns no `mLedger` and one `InvalidReportingYear`
processing diagnostic at `HistoricalValidation`, on `reporting_year`, affecting all four reports.
It invents no source, date or instrument. Upstream data and diagnostics remain available.
A valid year returns a `ReportingYearLedger`, even when all its views are empty:

- `mCapitalEvents` retains trades and corporate-action evidence through inclusive 31 December.
  Its first `mHistoricalCapitalEventCount` entries precede 1 January and establish opening inventory.
- `mSelectedYearEvents` retains all six event kinds within inclusive 1 January–31 December for
  later classification and report preparation. These are candidates, not final report rows.
- `mLossAnalysisEvents` retains trades and corporate-action evidence between 1 January minus
  30 days and 31 December plus 30 days, inclusive. For 2024 these bounds are 2 December 2023
  and 30 January 2025. All supplied ISINs remain candidates; equivalent-instrument declarations
  and the exact windows of individual losses belong to later loss processing.
- `mHasSelectedYearSales` records whether an in-year source trade is a sale. A false value supports
  the later capital `no_data` explanation only after classification and blocking errors are resolved.

Every view preserves merger chronology and uses only `mTaxDate` for membership. Earlier history
outside the analysis window remains owned and available as its opening context. Following-year
analysis events never enter the selected-year ledger or output candidates. Unresolved classifications
and action evidence are preserved; preparation does not infer support, apply actions, reorder same-day
splits, consume inventory, calculate loss eligibility or establish statement coverage. Those operations
belong to later processors. Boundary preparation success does not assign final report statuses,
clear upstream errors or mean that XML was generated. Full application status/API integration remains
later work. Parser diagnostics JSON is not changed by it.

Report exclusions preserve source facts and replacement allocations needed by retained losses.
Unresolved dependencies on excluded data require a loss decision for the dependent sale; they
cannot be cleared by removing the source instrument's report rows. Relevant following-January
actions use the same identity, ratio and multiple-source warning rules in the analysis view.

Exact FIFO lot basis retains the complete product of the acquisition-date converted
eight-decimal `UnitPrice` and imported Units. Proportional allocations remain exact rational
values until their specified output boundary. Controlled quantity adjustments retain their
original source values, apply through a run-local overlay and rebuild affected history. The
backend tracks the cumulative absolute EUR impact of inventory reconciliation per ISIN/run; details are in `calculations.md`
and `fifo.md`. This additional processor state does not change parser diagnostics.

The arithmetic layer returns typed numeric failures without report assumptions. The intermediate
`ProcessingDiagnostic` carries the stage, affected reports and safe source/instrument context.
The numeric adapter supplies caller-scoped arithmetic errors; reporting-year preparation reuses
the same envelope for year validation. `ApplicationResultBuilder` combines
the parser, merger and processing diagnostics into the application result: report scope, blocking
flag, deterministic order and IDs, the status of each form and the overall status, with version 2
JSON. `DecisionSession` holds the answers and recalculates the run, as defined in
`diagnostics.md`, "Structured user-decision contract". The 30-day loss rule is the first stage
that asks a question ("Loss eligibility contract"); the exclusion choices remain later integration.

Foreign-currency processing follows [`calculations.md`](calculations.md). The application result
contains one structured request for every missing official `(currency, tax date)` rate. A
user-entered official rate is shared across brokers and instruments only for that exact pair. A
broker rate is an event-specific fallback, requires an explicit frontend warning, and remains
marked as non-official in the result.

The application result also contains the selected-year informational fee summary. It groups
preserved fees by broker and original currency. It provides a broker-rate EUR equivalent only
when every required fee rate is available from that broker. A fee never creates an official-rate
request. Missing fee rates leave the affected EUR totals unavailable and do not block FURS XML
output.

Final models and result statuses are split by form: capital gains (Doh-KDVP), dividends (Doh-Div),
bank-deposit interest (Doh-DHO), and other interest (Doh-Obr). Broker cash interest and ordinary bond
coupons remain separate subtypes inside the common other-interest report. A broker parser supplies
facts such as product category and payer identity; the tax processor applies date-specific rules.
Unknown classifications and missing income fields produce structured requests. Explicit
classification exclusions remain visible in `excludedItems`, separate from each report's failed ISINs.

A Doh-Obr error safely isolated to an ISIN produces
`other_interest_failure_resolution_required`. The user excludes all listed failed ISINs' Doh-Obr
income or fails that form. Each report owns its exclusion list and failure choice; capital and
other-interest choices never apply to each other. Preserve original evidence and unaffected income.
Non-isolatable errors and final XML failures retain their normal scope, as defined in `diagnostics.md`.

A standalone reference catalogue supplies verified Slovenian-company and imported company/ETF
metadata with source, revision and effective-date provenance. It can prefill payer and treaty
facts; payment evidence determines settled Slovenian withholding and actual foreign tax.
Treaty exemptions require a separate applicable legal basis. The income result retains treatment
decisions and filing-evidence requirements. Catalogue conflicts and missing facts use
`income_details_required`; changing a referenced catalogue revision invalidates affected decisions.

The deposit allowance considers all imported and user-supplied qualifying bank interest. A
below-threshold TR total retains its reminder about other banks. Slovenian retail government bonds
are outside the current TR/IBKR import scope; their special treatment is not part of this workflow.

The complete application diagnostic model, revision-bound decisions, deterministic ordering,
privacy boundary, and four independent form statuses are defined in [`diagnostics.md`](diagnostics.md).
Application JSON uses version 2 for this expanded result. Parser diagnostics JSON is a developer
format until the beta (see "JSON contract" below).

### FIFO matching contract

`FifoMatcher::match` takes the merged statement and the `ReportingYearLedger` made from it. It
reads `mCapitalEvents` in order and changes nothing in the statement. The result has one
`FifoIsinResult` for each trade instrument, in the presentation order (by ISIN), and the numeric
diagnostics of the run. Each entry holds:

- `mMatches`: the internal FIFO matches of the sales dated in the selected year, in ledger order.
  A match links one sale to the units it takes from one purchase, with the exact purchase basis,
  the exact sale value (sale `UnitPrice` times matched `Units`) and the signed gain (sale value
  minus basis), all in ticks of `10^-16` EUR and never rounded. A negative gain is a loss-making
  portion. A sale that uses two purchases has two matches. They are not Doh-KDVP filing rows.
- `mEarlierMatches`: the matches of the sales before the selected year, in ledger order. Only
  the 30-day loss rule reads them: an earlier loss takes its replacement units first.
- `mOpenLots`: the lots still open in FIFO order at 31 December, each with its purchase event,
  date, remaining units and exact remaining basis.
- `mStatus`, `mStoppedAt` and `mAvailableUnits`: why and where the pool stopped, if it did.
  A pool stopped at a corporate action keeps the lots at the start of the action's day.

An earlier-year sale uses up lots; its matches go to `mEarlierMatches`, never to `mMatches`. One
pool exists for each ISIN, across files, brokers and accounts; broker, file and row never change
the matching. `isPastHoldingPeriod(match)` tells whether a match uses units past the holding period
([`tax_rules.md`](tax_rules.md), "Long-held investments"); the loss rule and the report builder
share it.

A pool stops at its first event that it cannot apply and ignores its later events. Other ISINs
continue. The failing sale is never applied partly: the lots stay as they were just before it,
and the matches made before the stop stay in the result as evidence.

| Status | Stopped at |
| --- | --- |
| `Matched` | Nothing; every trade up to 31 December was applied. |
| `CorporateAction` | The first corporate action, before the trades of its day: a trade of the ISIN on that date is not applied, even when it comes first in the ledger, and `mStoppedAt` is the action. Splits are applied by the later split work. |
| `NonEurTrade` | The first trade whose currency is not EUR, also before the selected year. |
| `InsufficientUnits` | A sale that needs more units than the earlier purchases left open. The result has one `incomplete_history` error. |
| `NumericFailure` | An invalid trade value or an overflow. The result has a diagnostic. |

The matcher reports two kinds of diagnostics, both for the capital report only:

- numeric failures (`unrepresentable_value` and `arithmetic_overflow`, stage `FifoTax`); and
- one `incomplete_history` error for an `InsufficientUnits` pool (stage `HistoricalValidation`).
  It names the investment and carries the failing sale's tax date and sources (file, row and
  transaction ID). It has no message parameters, so no unit count or amount is in the message.
  Only the first failing sale of an ISIN is reported, because the pool stops there;
  `mAvailableUnits` keeps the open units. The error blocks the capital report; the choice to
  exclude the ISIN or fail the report is later work.

The corporate-action errors come from the corporate-action stage ("Corporate-action contract"),
the non-EUR errors from the capital report gates (see "CLI report command (walking skeleton)").
Split adjustment and currency conversion are later work. The 30-day loss rule follows the
corporate-action stage ("Loss eligibility contract"). The Doh-KDVP filing rows are built from
the matches as described in "Doh-KDVP report model contract".

### Corporate-action contract

`CorporateActionResolver::resolve` (`include/processors/corporate_actions.hpp`) takes the merged
statement, the reporting-year ledger, the FIFO result of the same run and the session's
`RunDecisions`, and changes nothing in its inputs. It asks for the ratio of a corporate action
without a verified ratio, as [`fifo.md`](fifo.md), "Finding the split ratio", describes it. It
never depends on the broker, except to tell whether two rows of one day come from one broker.

- **Which ISINs.** Only a pool stopped at a corporate action (`FifoIsinStatus::CorporateAction`)
  of an ISIN with a sale in the selected year: only that pool blocks the report. Every corporate
  action of the ISIN up to 31 December is listed, grouped by date. A later action is never asked
  before the earlier one is applied, because the position before it depends on that.
- **Result.** `CorporateActionResult` has one `ResolvedCorporateAction` per date (by ISIN, then by
  date): its events, effective date, state and, for `RatioConfirmed`, the reduced ratio and the
  effective date (`mConfirmedDate`) that the user supplied (never the broker); `mEffectiveDate`
  stays the broker's row date. States: `RatioConfirmed`, `WaitingForRatio`,
  `WaitingForEarlierAction`, `Cancelled`, `SameDayAmbiguous`, `NoPosition`, `NumericFailure`
  and `NotAskedForClass`.
- **First action of the pool.** A bond, derivative, crypto or private fund is not asked
  (`NotAskedForClass`, `corporate_action_not_applied`): the gates block its sales anyway. Two rows
  of the same broker on that date are `SameDayAmbiguous` (`corporate_action_ambiguous`, key
  `.same_day`, no request); rows of different brokers on that date are one action. No open units
  just before it is `NoPosition` (`corporate_action_not_applied`). Open units whose checked sum
  does not fit `Units` are `NumericFailure` (`corporate_action_unrepresentable`, no request).
  Otherwise the stage asks one
  `corporate_action_ratio_required` question with `CorporateActionRatioQuestion` data (every
  source record, the open units of all brokers just before the action and the effective dates that
  keep them, `mEarliestDate` to `mLatestDate`) and one
  `corporate_action_ambiguous` error (key `.ratio_required`) linked to it. A recorded ratio gives
  `RatioConfirmed` and `corporate_action_not_applied` (key `.ratio_confirmed`, parameters
  `newShares` and `oldShares`) until the lot step applies it; a cancel gives `Cancelled` and
  `corporate_action_ambiguous` (key `.cancelled`, no request).
- **Later actions.** Two rows of the same broker on a later date are `SameDayAmbiguous`
  (`.same_day`, not linked to the open question, because answering it does not resolve them).
  Each other later action gets `WaitingForEarlierAction` and `corporate_action_not_applied` (key
  `.waiting_for_earlier_action`, parameter `earlierDate`), linked to the open ratio question of
  the first action when there is one, so capital gains needs input rather than failing.
- Every diagnostic has the stage `corporate_action`, affects capital gains only and carries the
  action's date and the sources of all its rows. No units, amounts or ratios as text are in the
  messages.

### Loss eligibility contract

`LossEligibilityChecker::check` (`include/processors/loss_eligibility.hpp`) takes the merged
statement, the reporting-year ledger, the FIFO result of the same run and the session's
`RunDecisions`. It applies the 30-day loss rule as [`tax_rules.md`](tax_rules.md), "How the app
applies the rule", describes it, and changes nothing in its inputs. ISINs whose pool stopped are
skipped: the report builder blocks them anyway. So are bonds, derivatives, crypto and private
funds: they never reach Doh-KDVP, and the capital report gates block them. An ISIN of unknown
class is checked, because it may still be classified as a share or a fund.

- **Inputs per ISIN.** The sales up to 31 December with their matches (`mEarlierMatches` and
  `mMatches`), every purchase in `mCapitalEvents`, and from `mLossAnalysisEvents` the purchases
  and corporate actions after 31 December. Sales after 31 December are left out. The stock after
  each sale comes from walking the ISIN's events.
- **Result.** `LossEligibilityResult` has one `LossEligibilityIsin` per trade instrument, by the
  same index as the FIFO result, each with its sales of the year that have a loss unit
  (`LossSaleEligibility`): sale, window, loss units, the replacements taken (purchase, date, units),
  the known ineligible units, the units blocked by a `yes` answer and a state: `Decided`,
  `WaitingForAnswer`, `Cancelled` or `CorporateActionInWindow` (with the action).
- **Question.** The open sales of one ISIN give one `loss_eligibility_confirmation_required`
  request with `LossEligibilityQuestion` data and one `loss_eligibility_unresolved` error per open
  sale (key `loss_eligibility_unresolved.confirmation_required`, the sale's sources and date),
  linked to the request. The checker looks the question up in `RunDecisions`: `no_other_activity`
  decides the open units as eligible, `other_activity` as blocked, and a cancel gives each open
  sale the error with key `loss_eligibility_unresolved.cancelled` and no request. The request's
  diagnostic indices count from the checker's first diagnostic; `appendStageOutput` moves them to
  the numbering of `ApplicationResultInput`.
- **Without a way to ask.** A caller that does not ask (the debug tools) passes
  `OpenLossQuestions::AssumeBlocked`. Without a recorded answer, the open units are
  then treated as with `other_activity` (`mAssumedBlocked`), with no request and one
  `loss_eligibility_unresolved` warning per ISIN (key
  `loss_eligibility_unresolved.assumed_blocked`, the sources of its open sales and the date of the
  first), so the user can correct the loss in eDavki.
- No units, amounts or prices are in the messages.

### Doh-KDVP report model contract

Tax processing produces broker-neutral report models, and the XML writers only format them. The
models hold no CSV, JSON, HTTP, frontend or XML fields and need no XML library. This section
covers shared metadata and the Doh-KDVP model (`include/taxbroker/report_models.hpp`); the income
models (Doh-Div, Doh-DHO, Doh-Obr) follow with the income work.

**Shared metadata.** `ReportMetadata` holds the taxpayer (`TaxpayerMetadata`), the reporting year
and the filing type (`Original` or `SelfReport`). It exists once for a run, and a form writer
receives it next to the model of its form; no form model repeats a taxpayer field. Only the tax
number and the residence are required; name, address, city, post code, post name, birth date,
email and phone are optional, as in the FURS schema. The model does not check the values. The
code that reads the user's input does, whether the user typed it or loaded it from a file. The
taxpayer fields can come from a saved profile once profiles are kept between runs, and the user
can change any of them before each run. The filing type is chosen for each run (`Original` unless the user picks
`SelfReport`) and is never saved with the profile. The
writer adds constants that are not data: the taxpayer type `FO`, the period dates (1 January and
31 December of the year), the list type `PLVP`, row numbers and the workflow codes.

**Doh-KDVP model.** `CapitalGainsReportKDVP` holds one `KdvpInventoryList` (a *popisni list*) for
each ISIN with a sale in the selected year, in ISIN order. A list has the ISIN, the name, `IsFond` and
its rows in ledger order. A row is a `KdvpPurchaseRow` (F1 date, F2 method `B`, F3 units, F4 unit
value) or a `KdvpSaleRow` (F6 date, F7 units, F9 unit value, F10), plus F8, the stock. The rows
are the filing rows of [`tax_rules.md`](tax_rules.md), "Doh-KDVP filing rows"; the internal FIFO
matches are not part of the model. A row keeps a `StatementEventReference` to its source trade
(an index, no file or row), so a later summary can name the sources. `UnitPrice` and `Units` keep
their eight decimals, and rounding to the XML text is the writer's job.

`KdvpLossRule` has only `NotBlocked` (F10 true) and `Blocked` (F10 false). It has no "unresolved"
value, so an open loss check can never be written as a decision. Only the loss rule
produces `Blocked`; a partly eligible sale will become two sale rows that share the source sale.
`IsFond` is always false for now: the class check (what belongs in Doh-KDVP) is the job of the
classification gate, not of the builder.

**Building the rows.** `CapitalGainsReportBuilder::build` takes the merged statement, the
reporting-year ledger, the FIFO result, the loss eligibility result and the ISINs that the user
agreed to leave out. The inputs must come from the same run: the FIFO and loss results have one
entry per trade instrument, by the same index. For an ISIN with at least one sale in the selected
year:

- A purchase row is made for each purchase that the matches of the year use, at its ledger
  position, with F3 the sum of its matched units. A sale row is made for each sale of the year,
  with F7 the sum of its matched units. F8 is the running total of the listed purchase units minus
  the listed sale units.
- Matches that use units past the holding period ([`tax_rules.md`](tax_rules.md), "Long-held
  investments") are tax free and are left out of F3 and F7. A sale with only such units has no
  row, and an ISIN with no listed unit has no list. The units left out are in `mLongHeld` (ISIN,
  name, sale, units) for every ISIN that is not blocked, so they stay visible beside the report.
- A sale row gets `Blocked` when the loss rule blocks the loss of every listed unit (`F7`), else
  `NotBlocked`.
- The ISIN is blocked, with no list, for the first of these reasons that applies:
  `FifoStopped` (the pool stopped; all its sales of the year are named) or `LossNotReportable` (a
  sale whose loss-rule state is not `Decided`, or whose rule blocks more than none and fewer than
  all of its listed units; only those sales are named). A loss on units past the period is left
  out with them, because it cannot reduce the tax base. A gain of zero is not a loss. A stopped
  pool without a sale in the year does not block. A loss from an earlier-year sale is not in the
  matches of the year and never blocks.
- A blocked ISIN that the user agreed to leave out goes to `mExcluded` and stays visible beside
  the report; any other blocked ISIN goes to `mBlocked`. The report exists only when `mBlocked` is
  empty, and then holds a list for every ISIN without a problem. An ISIN without a problem is never
  left out, even if it is on the user's list.

The builder adds no diagnostics and no message keys. The capital report gates turn `mBlocked`
into errors (see "CLI report command (walking skeleton)"), and the application result
turns them into report statuses. Asking the user which blocked ISINs to leave out comes with the
explicit ISIN exclusion.

### Doh-KDVP XML writer

`KdvpXmlWriter::write` (`include/xml/kdvp_xml_writer.hpp`) turns `ReportMetadata` and
`CapitalGainsReportKDVP` into one Doh-KDVP document. It lives in its own `taxbroker_xml` target,
the only one that links pugixml, so `taxbroker_core` stays free of XML. It only formats and
checks: it never recalculates FIFO, stock, loss eligibility or holding periods, and never shortens,
cleans or rounds a value to make it fit. The result holds the XML text or, when any field does
not fit the form, one `xml_value_out_of_range` error per field and no XML.

- **Constants the writer adds:** taxpayer type `FO`, `PeriodStart` 1 January and `PeriodEnd`
  31 December of the year, `InventoryListType` `PLVP`, `SecurityCount` the number of lists and the
  other four counts `0`, F2 `B` (*nakup*) and F5 `0.0000` on every purchase row. F5 is the
  inheritance or gift tax paid at the purchase, a known zero for a bought security; the FURS
  instructions fill column 5 on every purchase row, and the legacy output that eDavki accepted
  wrote `0.0000`. The filing type becomes the workflow code (`DocumentWorkflowID`) and its name
  (`DocumentWorkflowName`), written in `edp:Workflow` and in `KDVP`: `O` with `Original` (as in the
  legacy output), or `I` with `SamoPrijava` for a self-report (*samoprijava*). The legacy `S` with
  `SelfReport` is wrong: eDavki expects `I` and `SamoPrijava` (maintainer's eDavki draft import,
  2026-10-27, [`verification.md`](verification.md)).
- **Taxpayer fields:** tax number, name (`name`), address (`address1`), city, post code
  (`postNumber`), post name, birth date and residence (`resident`) in the header; residence
  again as `IsResident`, phone as `TelephoneNumber` and email as `Email` in `KDVP`. An absent
  optional field is left out.
- **Left out:** `Securities/Code`, `KDVPItem/Name`, `F11`, the foreign-tax and tax-relief
  fields. A list without a name has no `Securities/Name`.
- **Numbering and order:** lists and rows in model order. `ItemID` counts from 1, the row `ID`
  from 0 in each list, as in the legacy files that eDavki accepted.
- **Numbers:** F3, F4, F7, F8 and F9 are the exact stored values with four to eight decimals
  ([`calculations.md`](calculations.md), "XML precision"). The largest `int64` at eight decimals
  has 11 integer digits, so no stored value is too large for the form.
- **F10:** on every sale row, `true` for `NotBlocked` and `false` for `Blocked`; purchase rows have
  none. Two sale rows of one source sale stay two rows.
- **Checks** (each failure is an error for the field): tax number of eight digits and not
  `00000000`; year 1000 to 9999; dates with a year from 1 to 9999; ISIN of 1 to 12 characters;
  list name of at most 100 characters and post code of at most 12 (characters, not bytes);
  every text valid UTF-8 without control characters, U+FFFE or U+FFFF; F3, F4, F7, F9 and F8 not
  negative. The schema accepts a negative F8 and an empty ISIN, but only the planned
  incomplete-history developer export may write a negative F8.
- **An empty model** (no lists) gives a valid document without `KDVPItem`; the caller decides that
  the form has no data and writes no file.

The FURS schemas are in `resources/xml/schemas/`: the official `Doh_KDVP_9.xsd` and
`EDP-Common-1.xsd` as downloaded, and in `snapshot-2026-05-12/` the older copy that the legacy
app used. The unit and integration tests check every document they write against both with
libxml2. The application does not link libxml2 and does not check the schema at run time.

The integration tests also read the rows of each document back and run their own FIFO over them
(`tests/support/kdvp_row_fifo.hpp`, no production code), which must give the gains and `F8`
values written as literals in each test. A valid schema check is not eDavki acceptance: the schema
makes every F1 to F11 optional. The eDavki draft imports are recorded separately, in
[`verification.md`](verification.md).

### CLI report command (walking skeleton)

```text
taxbroker report --year YYYY --taxpayer taxpayer.json [--self-report] --out DIR CSV [CSV ...]
```

The `taxbroker` executable (`src/cli/`) produces a real Doh-KDVP file for a bounded set of cases
and blocks every other case with a structured error; it never guesses. `taxbroker --help` and
`taxbroker report --help` explain the options, the taxpayer file, the output files, the exit codes
and what is supported, blocked and skipped. The whole command is in the `taxbroker_cli` library
(`runCli`), so the tests run it in process; the executable only sets up logging. The
orchestration moves into the application service once that service connects all processing
stages.

**Pipeline.** Each CSV is read with the path-based Trade Republic parser, with its position on the
command line as the source index. Then merge and reporting-year preparation, once. The rest runs
as the calculation of a `DecisionSession`, once per revision: FIFO, the corporate-action
stage, the loss rule (`OpenLossQuestions::Ask`), the report builder (no exclusions), the capital
report gates and the XML writer; the session builds the application result. A loss that the files
decide in full is written as they decide it. A sale whose blocked units are only part of its
listed units still blocks the report.

**Questions** (`include/cli/decision_prompts.hpp`). After the first calculation the command asks
the open loss and ratio questions in the terminal, one at a time in request order, and reads each
answer from standard input. It only shows the request and passes the answer on: the session
checks it, records it and calculates again, and the next question is the first open one of the
new result (new revision, new request IDs). A rejected answer prints the session's message and
the question is asked again; a line the command cannot read (another word, a number that is not
whole, a share count of 0 or below, a date not in the form `YYYY-MM-DD`) is asked again without a
session call.

- Loss question: `yes` (`other_activity`), `no` (`no_other_activity`) or `cancel`, in any case.
- Ratio question: `new : old` (whole numbers) or `before -> after` (share counts above 0 with up
  to 8 decimals, turned into the ratio after / before as `splitRatioFromShareCounts` does), or
  `cancel`; then the effective date (an empty line takes the broker's row date); then the
  preview (`previewSplitUnits`, with the warning of `isUnusualSplitRatio`) and `yes` or `no` (no
  asks for the ratio again). A ratio without a preview (a 0 in `new : old`, equal numbers, a
  position that does not fit) is passed on as it is, so the session's rejection names the problem.
- A share count takes a decimal comma (the Slovenian standard) or a point: `0,52 -> 5,2` and
  `0.52 -> 5.2` are the same. One number never has both, and there is no digit grouping, so
  `1,234` is 1.234 and `1.000,5` is refused; the confirmed preview catches a mistake. This is
  only for typed answers: broker CSV import stays strict ([`calculations.md`](calculations.md)).
  The CLI output keeps the point new logic is implemented.
- A failure-resolution question is not asked yet (no stage emits one), and a request without
  question data cannot be answered; both stay open.
- With `--no-input`, nothing is read and the questions stay open. When the input ends, the
  current and every later question stay open. An open question keeps capital gains
  `needs_input`, a cancelled one makes it `failed`; either way no XML is written (exit 1).
- The summary lists the open requests ("Open questions", with ID, type and investment) and the
  answers of this run ("Answered in this run"). The answers live only in the session's memory:
  nothing else is written, and the next run asks again.

Until next tasks apply splits, a confirmed ratio still blocks the report with
`corporate_action_not_applied` (key `.ratio_confirmed`). The XML is written only when the builder made a report with at least
one list and no gate gave an error; a report without lists is `no_data` with
`no_selected_year_disposals`. The capital status of the application result decides the files and
the exit code, so a blocking parser or merger error (a rejected trade or corporate-action row, a
file-level error, a `conflicting_duplicate` of a trade or corporate action, an
`instrument_asset_class_conflict`) also blocks the report, whatever ISIN or year it belongs to.
An event the merger rejected is not in the ledger, so only this rule blocks it.

**Capital report gates** (`CapitalReportGates::check`, `include/processors/capital_report_gates.hpp`).
They look only at ISINs with a sale in the selected year, as the builder does. All errors affect
capital gains only and carry no units, amounts or prices:

| Case | Code (stage) | One diagnostic per |
| --- | --- | --- |
| Class crypto, private fund, bond or derivative | `unsupported_tax_treatment` error (`fifo_tax`), parameter `assetClass` | ISIN; sources of its sales of the year, date of the first |
| Class unknown (missing, unrecognized or conflicting) | `tax_classification_unknown` error (`fifo_tax`) | ISIN; as above |
| Pool stopped at a non-EUR trade | `missing_exchange_rate` error (`fifo_tax`), parameter `currency` | ISIN; date and sources of that trade |
| Sale whose loss the 30-day rule blocks only in part | `unsupported_tax_treatment` error (`fifo_tax`), key `unsupported_tax_treatment.partial_loss_eligibility` | sale |
| Corporate action after 31 December inside the window of a December loss | `corporate_action_not_applied` error (`corporate_action`), key `corporate_action_not_applied.loss_window` | one per action, even when several sales share it; date and sources of the action |
| Private-market event dated in the selected year | `unsupported_tax_treatment` **warning** (`fifo_tax`) | event |

Only `Stock` and `Fund` pass the class check; `IsFond` stays `false` for both. A pool stopped for
missing history or a numeric failure already has its FIFO diagnostic and gets nothing more, and
one stopped at a corporate action up to 31 December has its diagnostics from the corporate-action
stage. Only
the first stop of a pool is reported, because FIFO stops there. A crypto purchase without a sale
in the year keeps only its parser warning. Private-market events (`PRIVATE_MARKET_BUY`,
`PRIVATE_MARKET_SELL`, `BONUS`) have only a cash amount, without units or a price, so they are
skipped, a sale included; the warning keeps each one visible (maintainer decision, 2026-10-26).
For the same reason a rejected private-market row, or a merger error on a private-market event,
affects no report: it stays an error and is listed in the summary, but it blocks nothing
(maintainer decision, 2026-10-26). When private markets are supported, these errors get the
capital scope back.
An open or cancelled loss question already has its `loss_eligibility_unresolved` error from the
loss rule, so the gates add nothing for it. Each gate goes away when its rule exists: applied
splits and confirmed corporate-action ratios, conversion with official exchange rates, the split
rows of a partly blocked loss, and classification requests.

**Taxpayer file.** One JSON object with `taxNumber` (eight digits, not `00000000`) and
`isResident` (only `true` for now), and the optional strings `name`, `address`, `city`,
`postCode`, `postName`, `birthDate` (`YYYY-MM-DD`), `email` and `phoneNumber`. Any other key is an
error, `filingType` included: the filing type is chosen for each run (`--self-report`), never
read from the file. An error names the key, never the value.

**Output.** In the `--out` folder (created when missing): `Doh-KDVP_<year>.xml` only when the
capital report is `generated`, and `summary_<year>.txt` for every run that got that far; the same
summary goes to stdout. A blocked or no-data run deletes an old `Doh-KDVP_<year>.xml` there and
says so, so a file of an earlier run is never taken for this run's result. The summary has the
input files (file names and source IDs only), the capital status, each ISIN with a sale in the year
(listed with its sale rows and units, the units with `F10` false and how many of them were assumed
without an answer, blocked with its codes and
sales, or only units past the holding period), the units past the holding period (`mLongHeld`: ISIN, sale date, units), the
skipped private-market events, and the diagnostics in application order (capital errors, capital
warnings, then the others with their severity). It has units and dates, no EUR amounts.

Both files are first written as `<name>.tmp` in the output folder and only then renamed over the
files of an earlier run, or the old XML is deleted. A folder at an output path is refused before
anything is written. So a run that fails keeps the files of an earlier run as they were.

**Exit codes.** `0`: the capital report is `generated` or `no_data`. `1`: it is blocked (`failed`).
`2`: the command could not run (an option, a missing or invalid file, the output folder); no
output file is changed (a missing output folder may already be created) and stderr names the
problem with file names only, never a folder.

## Parser diagnostics

Parser diagnostics have three separate responsibilities:

1. `include/taxbroker/diagnostics.hpp` defines the in-memory domain model. A `ParseResult` owns its
   diagnostics, so parsing separate CSV files concurrently does not require shared mutable state.
   Each diagnostic carries a message key and named parameters for translation, and the English
   sentence for logs and developer tools (see "Message keys and translation" in
   [`diagnostics.md`](diagnostics.md)).
2. `include/taxbroker/api/diagnostics_json.hpp` and its implementation under `src/server/api/`
   convert that model into the versioned frontend contract. The parsing and tax-processing core
   does not depend on a JSON library.
3. The optional `taxbroker_tr_dump` developer tool writes a JSON artifact. Production parsing does
   not write diagnostics to disk; the future local API should return them directly in its response.

Application logs remain useful for developers and operational troubleshooting, but the frontend
must never parse log messages.

### JSON contract

```json
{
  "schemaVersion": 1,
  "broker": "trade_republic",
  "status": "completed_with_errors",
  "summary": {
    "warningCount": 1,
    "errorCount": 1,
    "totalCount": 2
  },
  "diagnostics": [
    {
      "severity": "warning",
      "code": "unsupported_asset_class",
      "message": "Asset class 'CRYPTO' was preserved, but its tax treatment is not supported yet.",
      "messageKey": "unsupported_asset_class.tax_unsupported",
      "messageParams": {
        "assetClass": "CRYPTO"
      },
      "source": {
        "file": "TransactionExport.csv",
        "row": 275
      },
      "transactionId": "example-transaction-id",
      "field": "asset_class"
    }
  ]
}
```

The `status` value is derived from diagnostics:

- `success`: no diagnostics;
- `completed_with_warnings`: one or more warnings and no errors;
- `completed_with_errors`: at least one error.

Warnings describe preserved data that needs attention later. Errors describe rejected rows or a
file-level parsing failure. Optional location fields are omitted when unavailable rather than sent
as `null`.

`message` is the English sentence for logs and developer tools. The frontend builds its text from
`messageKey` and `messageParams`; it never parses `message`.

The strings used by `severity`, `code`, `messageKey`, `broker`, and `status` are API values for the
frontend. No JSON contract is frozen before the beta: `schemaVersion` stays 1, and a field may
still be renamed or removed. From the first released frontend on, these values are not renamed
within a schema version. Add a new code or key for new behavior, and increment `schemaVersion` for
a breaking contract change. This versioning protects the current backend and future frontend
contract; it does not cover the legacy project.

For privacy, the response exposes only the uploaded filename, never an absolute host path. It also
does not duplicate raw financial field values. The CSV row number, transaction ID, field name,
message key with its parameters, and an English message provide enough context for the local UI.
The separate application result defined in [`diagnostics.md`](diagnostics.md) copies the key and
parameters of parser diagnostics.

### Local debug artifacts

Running `scripts/dump_tr_parse.sh` produces:

- `runtime/debug/tr_parsed_debug.txt` for the parsed C++ data;
- `runtime/diagnostics/tr_parse_diagnostics.json` for the frontend-shaped diagnostics.

The tool shows what the parser retains from a local export, including parsed financial data and
complete event metadata. Only the source filename is stored, never its path. The `runtime/`
directory is ignored by Git; its potentially private artifacts must remain local.

Running `scripts/dump_tr_kdvp.sh` writes the Doh-KDVP XML of the selected year to
`runtime/debug/tr_kdvp_<year>.xml`; with `--synthetic` it reads the synthetic
`tests/test_data/csv/kdvp_dump_fixture.csv` instead of the real export and writes
`runtime/debug/tr_kdvp_synthetic_<year>.xml`. Blocked ISINs are left out, so with a blocked ISIN
the file is not a complete return; the tool says so.

Running `scripts/report.sh` runs `taxbroker report` in Docker and writes to
`runtime/report/<year>/`; with `--synthetic` it reads `tests/test_data/cli/report_fixture.csv` and
`tests/test_data/cli/taxpayer.json`. A real taxpayer file stays below `runtime/`.

Parser logs may later be shared for support, so they follow a stricter boundary: source index, CSV
row, and diagnostic reason are allowed; paths, transaction IDs, and raw financial values are not.
