#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

manual_dir="tests/test_data/manual"

usage() {
    cat <<EOF
Usage: profile.sh [--reuse-build] [--workload synthetic|mixed|large|custom|all] [--rows N]
                  [--runs N] [--callgrind | --no-callgrind]
                  [--trade-republic path.csv ...] [--ibkr path.xml ...] [--taxpayer path.json]
                  [--year YYYY] [--no-input]

    Measures 'taxbroker report' per workload: wall time (median of --runs runs), peak memory,
    and a Callgrind profile with the cost of each stage. It builds build/profile from scratch
    (Release, symbols, static, only the taxbroker executable) and runs inside Docker. Everything
    is written to the ignored runtime/profile/<workload>/ (time.txt, callgrind.out, top.txt,
    stages.txt, run.log and out/; the folder of the large workload is large_<rows>); nothing
    leaves runtime/. Paths must not contain whitespace.

    --reuse-build   use the existing build/profile and compile nothing
    --workload      synthetic  the report.sh --synthetic files, year 2024
                    mixed      the manual files of $manual_dir (4 Trade Republic, 4
                               Interactive Brokers), year 2024, with the answers of its README
                    large      a generated Trade Republic export of --rows rows, year 2024
                    custom     your own files (the file options below; --taxpayer is required)
                    all        synthetic, mixed and large (the default)
    --rows          rows of the generated export (default 50000); the file is
                    runtime/profile/input/large_trade_republic_<rows>.csv
    --runs          timing runs per workload (default 5)
    --callgrind     profile each workload under Callgrind (the default)
    --no-callgrind  only the timing runs
    --trade-republic, --ibkr   the exports of the custom workload (paths after the option)
    --taxpayer      the taxpayer JSON of the custom workload, for example runtime/taxpayer.json
    --year          the year of the custom workload (default: the previous calendar year)
    --no-input      the custom workload asks no questions (an open question blocks the run)

    The exit code is 1 when a run exits with an error or reports a parse or merge error (the
    mixed and custom workloads may report parse errors; a custom run may be blocked by a question).
    Times have a resolution of 1 ms; Callgrind instruction counts (Ir) vary by well under 0.01%.
EOF
}

reuse_build=false
workload=all
rows=50000
runs=5
callgrind=true
taxpayer=""
year=""
custom_no_input=false
csv_inputs=()
ibkr_inputs=()
list_option=""
list_count=0

end_list() {
    if [[ -n "$list_option" && "$list_count" -eq 0 ]]; then
        echo "$list_option needs at least one path." >&2
        exit 2
    fi

    list_option=""
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h | --help)
            usage
            exit 0
            ;;
        --trade-republic | --ibkr)
            end_list
            list_option="$1"
            list_count=0
            shift
            ;;
        --reuse-build)
            end_list
            reuse_build=true
            shift
            ;;
        --callgrind)
            end_list
            callgrind=true
            shift
            ;;
        --no-callgrind)
            end_list
            callgrind=false
            shift
            ;;
        --no-input)
            end_list
            custom_no_input=true
            shift
            ;;
        --workload)
            end_list

            if [[ $# -lt 2 || ! "$2" =~ ^(synthetic|mixed|large|custom|all)$ ]]; then
                echo "--workload requires synthetic, mixed, large, custom or all." >&2
                exit 2
            fi

            workload="$2"
            shift 2
            ;;
        --rows | --runs)
            end_list

            if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]{0,6}$ ]]; then
                echo "$1 requires a positive whole number." >&2
                exit 2
            fi

            if [[ "$1" == "--rows" ]]; then
                rows="$2"
            else
                runs="$2"
            fi

            shift 2
            ;;
        --year)
            end_list

            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]{4}$ ]]; then
                echo "--year requires a four-digit year." >&2
                exit 2
            fi

            year="$2"
            shift 2
            ;;
        --taxpayer)
            end_list

            if [[ $# -lt 2 ]]; then
                echo "--taxpayer requires a path." >&2
                exit 2
            fi

            taxpayer="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            case "$list_option" in
                --trade-republic)
                    csv_inputs+=("$1")
                    ;;
                --ibkr)
                    ibkr_inputs+=("$1")
                    ;;
                *)
                    echo "$1 is not after a file option such as --trade-republic." >&2
                    usage >&2
                    exit 2
                    ;;
            esac

            list_count=$((list_count + 1))
            shift
            ;;
    esac
done

end_list

# The custom workload is the one with your own files; it runs only when it is named.
if [[ "$workload" == "custom" ]]; then
    if [[ -z "$taxpayer" || ( ${#csv_inputs[@]} -eq 0 && ${#ibkr_inputs[@]} -eq 0 ) ]]; then
        echo "--workload custom needs --taxpayer and a --trade-republic or --ibkr file." >&2
        exit 2
    fi
elif [[ -n "$taxpayer" || ${#csv_inputs[@]} -gt 0 || ${#ibkr_inputs[@]} -gt 0 ]]; then
    echo "The file options belong to --workload custom." >&2
    exit 2
fi

year="${year:-$(( $(date +%Y) - 1 ))}"

for input_path in "$taxpayer" "${csv_inputs[@]}" "${ibkr_inputs[@]}"; do
    if [[ -z "$input_path" ]]; then
        continue
    fi

    if [[ "$input_path" == /* || "$input_path" == -* || ! -f "$repo_root/$input_path" ]]; then
        echo "Input must be an existing repository-relative path: $input_path" >&2
        exit 2
    fi

    # The paths reach the measuring script as one text, split at spaces.
    if [[ "$input_path" =~ [[:space:]] ]]; then
        echo "Input paths must not contain whitespace: $input_path" >&2
        exit 2
    fi
done

ensure_dev_image
ensure_build_tree_writable

if [[ "$reuse_build" == true ]]; then
    if [[ ! -x "$repo_root/build/profile/src/taxbroker" ]]; then
        echo "==> build/profile contains no build. Build it with: scripts/profile.sh" >&2
        exit 1
    fi

    echo "==> Reusing build/profile as it is; nothing is compiled."
else
    reset_build_dir profile

    echo "==> Configuring build/profile (Release with symbols, static)..."
    retry_configure compose run --rm -T -e CC=clang -e CXX=clang++ dev sh -lc \
        'cmake -S /workspace -B /workspace/build/profile -G Ninja -DCMAKE_BUILD_TYPE=Release -DTAXBROKER_STATIC=ON -DTAXBROKER_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS=-g'

    echo "==> Building the taxbroker executable..."
    compose run --rm -T -e CC=clang -e CXX=clang++ dev sh -lc \
        'cmake --build /workspace/build/profile --parallel --target taxbroker'
fi

mkdir -p "$repo_root/runtime/profile" "$repo_root/runtime/logs"

custom_files=""

for csv_path in "${csv_inputs[@]}"; do
    custom_files+=" --trade-republic /workspace/$csv_path"
done

for ibkr_path in "${ibkr_inputs[@]}"; do
    custom_files+=" --ibkr /workspace/$ibkr_path"
done

echo "==> Profiling workload $workload..."

# The measuring script runs inside the container. It is read from standard input, so the answers
# of each workload come from files it writes itself.
set +e
compose run --rm -T \
    -e PROFILE_WORKLOAD="$workload" \
    -e PROFILE_ROWS="$rows" \
    -e PROFILE_RUNS="$runs" \
    -e PROFILE_CALLGRIND="$callgrind" \
    -e PROFILE_YEAR="$year" \
    -e PROFILE_TAXPAYER="$taxpayer" \
    -e PROFILE_CUSTOM_FILES="$custom_files" \
    -e PROFILE_CUSTOM_NO_INPUT="$custom_no_input" \
    dev sh -s <<'INNER'
set -u

root=/workspace
bin=$root/build/profile/src/taxbroker
profile=$root/runtime/profile
cli=$root/tests/test_data/cli
manual=$root/tests/test_data/manual
failed=0

mkdir -p "$profile/input"

# Writes the generated Trade Republic export: PROFILE_ROWS rows of buys, sales, dividends and
# interest, deterministic for a given row count, ending on 2024-12-31. Amounts are in cents (integers only).
generate_large_csv() {
    file="$profile/input/large_trade_republic_$PROFILE_ROWS.csv"

    awk -v rows="$PROFILE_ROWS" '
    function civil(z,   era, doe, yoe, y, doy, mp, d, m) {
        z += 719468
        era = int(z / 146097)
        doe = z - era * 146097
        yoe = int((doe - int(doe / 1460) + int(doe / 36524) - int(doe / 146096)) / 365)
        y = yoe + era * 400
        doy = doe - (365 * yoe + int(yoe / 4) - int(yoe / 100))
        mp = int((5 * doy + 2) / 153)
        d = doy - int((153 * mp + 2) / 5) + 1
        m = mp < 10 ? mp + 3 : mp - 9
        if (m <= 2) y++
        return sprintf("%04d-%02d-%02d", y, m, d)
    }
    function money(cents,   sign) {
        sign = cents < 0 ? "-" : ""
        if (cents < 0) cents = -cents
        return sprintf("%s%d.%02d", sign, int(cents / 100), cents % 100)
    }
    BEGIN {
        # The rows end on the last weekday of 2024 (day number 20088, a Tuesday), 20 rows a weekday,
        # so the year of the report is always 2024 and a bigger file only adds history.
        weekdays = int((rows + 19) / 20)
        day = 20088
        for (w = weekdays - 1; w >= 0; w--) {
            while ((day + 4) % 7 == 0 || (day + 4) % 7 == 6) day--
            days[w] = day
            day--
        }
        split("US0378331005 US0594603039 US4781601046 SI0031102120", isins, " ")
        print "datetime,date,account_type,category,type,asset_class,name,symbol,shares,price,amount,fee,tax,currency,original_amount,original_currency,fx_rate,description,transaction_id,counterparty_name,counterparty_iban,payment_reference,mcc_code"
        for (row = 0; row < rows; row++) {
            weekday = int(row / 20)
            slot = row % 20
            date = civil(days[weekday])
            stock = (weekday * 20 + slot) % 40
            isin = sprintf("XX00001%05d", stock + 1)
            name = "Synthetic Stock " (stock + 1)
            cents = 1000 + weekday
            stamp = sprintf("%sT09:00:%02dZ", date, slot)
            id = "profile-" (row + 1)

            if (slot == 19) {
                if (weekday % 2 == 0) {
                    dividends++
                    isin = isins[dividends % 4 + 1]
                    printf "%s,%s,DEFAULT,CASH,DIVIDEND,STOCK,Synthetic Payer,%s,,,5.00,,,EUR,,,,,%s,,,,\n", stamp, date, isin, id
                } else {
                    printf "%s,%s,DEFAULT,CASH,INTEREST_PAYMENT,,,,,,0.10,,,EUR,,,,Synthetic bank interest,%s,,,,\n", stamp, date, id
                }
            } else if (slot >= 14 && hold[stock] >= 3) {
                hold[stock] -= 3
                printf "%s,%s,DEFAULT,TRADING,SELL,STOCK,%s,%s,-3,%s,%s,,,EUR,,,,,%s,,,,\n", stamp, date, name, isin, money(cents), money(3 * cents), id
            } else {
                hold[stock] += 10
                printf "%s,%s,DEFAULT,TRADING,BUY,STOCK,%s,%s,10,%s,%s,,,EUR,,,,,%s,,,,\n", stamp, date, name, isin, money(cents), money(-10 * cents), id
            }
        }
    }' > "$file"
}

# Runs one workload. Arguments: name, answers file (or "none"), then the report arguments
# without --out. The CLI reads its answers from the file; Callgrind and the timing runs use it
# the same way.
profile_workload() {
    name="$1"
    answers="$2"
    shift 2

    dir="$profile/$name"
    rm -rf "$dir"
    mkdir -p "$dir/out"

    if [ "$answers" = "none" ]; then
        answers=/dev/null
    fi

    echo "==> Workload $name: $PROFILE_RUNS timing runs..."
    : > "$dir/time.txt"
    times=""
    i=1

    while [ "$i" -le "$PROFILE_RUNS" ]; do
        rm -f "$dir/run.log"
        # The wall time (milliseconds) goes to file descriptor 3, the CLI output to its own file.
        TBR_LOG_STDOUT=false TBR_LOG_FILE="$dir/run.log" perl -MTime::HiRes=gettimeofday,tv_interval -e '
            my $answers = shift @ARGV;
            open(STDIN, "<", $answers) or die "answers: $!";
            open(my $wall, ">&=", 3) or die "fd 3: $!";
            my $t0 = [gettimeofday];
            my $status = system(@ARGV);
            printf $wall "%.0f\n", tv_interval($t0) * 1000;
            exit($status >> 8);
        ' "$answers" time -v -o "$dir/memory_$i.txt" "$bin" report --year "$PROFILE_YEAR" --log-level info "$@" --out "$dir/out" \
            > "$dir/cli_output.txt" 2> "$dir/cli_error.txt" 3> "$dir/wall_$i.txt"
        status=$?

        # A custom run may be blocked by an open question (exit 1); the other workloads must succeed.
        if [ "$status" -ne 0 ] && { [ "$name" != "custom" ] || [ "$status" -ne 1 ]; }; then
            echo "==> Workload $name: run $i failed (exit $status); see $dir/cli_output.txt and cli_error.txt." >&2
            failed=1
            return 1
        fi

        wall=$(tail -n 1 "$dir/wall_$i.txt")
        rss=$(grep "Maximum resident" "$dir/memory_$i.txt" | sed 's/.*: //')
        echo "run $i: wall $wall ms, max RSS $rss kB" >> "$dir/time.txt"
        times="$times $wall"
        i=$((i + 1))
    done

    median=$(echo $times | tr ' ' '\n' | sort -n | awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }')
    echo "median wall ms: $median" >> "$dir/time.txt"
    echo "max RSS kB (last run): $rss" >> "$dir/time.txt"
    rm -f "$dir"/wall_*.txt "$dir"/memory_*.txt

    # The manual files of the mixed workload hold rows that are errors on purpose.
    if [ "$name" != "mixed" ] && [ "$name" != "custom" ] \
        && grep -q "parse errors [1-9]\|merge errors [1-9]" "$dir/run.log"; then
        echo "==> Workload $name: the run log shows a parse or merge error." >&2
        failed=1
    fi

    grep "Stage parse and merge\|Calculation done" "$dir/run.log" > "$dir/stages_log.txt"

    if [ "$PROFILE_CALLGRIND" = "true" ]; then
        echo "==> Workload $name: Callgrind run..."
        TBR_LOG_STDOUT=false TBR_LOG_FILE="$dir/run_callgrind.log" valgrind --tool=callgrind \
            --callgrind-out-file="$dir/callgrind.out" "$bin" report --year "$PROFILE_YEAR" \
            --log-level info "$@" --out "$dir/out_callgrind" < "$answers" > "$dir/cli_output_callgrind.txt" 2> "$dir/callgrind_error.txt"
        status=$?

        # Under Valgrind the CLI gives the same exit code as natively (a custom run may be blocked).
        if [ "$status" -ne 0 ] && { [ "$name" != "custom" ] || [ "$status" -ne 1 ]; }; then
            echo "==> Workload $name: the Callgrind run failed (exit $status); see $dir/cli_output_callgrind.txt and callgrind_error.txt." >&2
            failed=1
            return 1
        fi

        if [ ! -s "$dir/callgrind.out" ]; then
            echo "==> Workload $name: no Callgrind output; see $dir/callgrind_error.txt." >&2
            failed=1
            return 1
        fi

        callgrind_annotate --inclusive=yes --threshold=100 "$dir/callgrind.out" > "$dir/all_inclusive.txt" 2>&1
        callgrind_annotate --inclusive=no --threshold=100 "$dir/callgrind.out" > "$dir/all_exclusive.txt" 2>&1
        total=$(grep -m 1 "PROGRAM TOTALS" "$dir/all_inclusive.txt" | sed 's/ *PROGRAM TOTALS.*//; s/^ *//')
        echo "total Ir: $total" >> "$dir/time.txt"

        {
            echo "Top 15 functions, inclusive (Ir, share of the run, function):"
            grep -E "^ *[0-9,]+ \(" "$dir/all_inclusive.txt" | head -n 15
            echo
            echo "Top 15 functions, exclusive:"
            grep -E "^ *[0-9,]+ \(" "$dir/all_exclusive.txt" | head -n 15
        } > "$dir/top.txt"

        {
            echo "Stage table (inclusive Ir and share of the run; the largest match of each name):"
            for pattern in 'ApplicationRun::start' 'loadRunData' 'loadOfficialRates' \
                'BuiltInRateYear::BuiltInRateYear' 'loadReferenceData' 'readCsvText' \
                'TradeRepublicParser::parse' 'IbkrParser::parse' \
                'DeterministicStatementMerger::merge' 'TaxProcessor::prepareReportingYear' \
                'DecisionSession::start' 'calculationOn' 'calculateReport' 'prepareDividendPayments' \
                'prepareIncomeReports' 'makeDiagnosticMessage' 'writeReportXml' \
                'askOpenQuestions' 'replaceFiles' 'InitializeLogger'; do
                line=$(grep -E "^ *[0-9,]+ \(.*[:/ ][^ ]*($pattern)" "$dir/all_inclusive.txt" | head -n 1 \
                    | sed 's/^ *\([0-9,]* ([ 0-9.]*%)\).*/\1/')
                echo "${line:-not found}  $pattern"
            done
        } > "$dir/stages.txt"

        rm -rf "$dir/out_callgrind"
    fi

    echo "==> Workload $name: median $median ms, max RSS $rss kB"

    if [ -f "$dir/stages.txt" ]; then
        echo "    total Ir $total"
    fi
}

# W2 answers: the 2024 line of tests/test_data/manual/README.md.
write_mixed_answers() {
    printf 'capital\ndiscount\nno\n1 : 10\n\nyes\n1\nyes\n2 : 1\n\nyes\nyes\n\nyes\n1 : 1\n\ntaxable\n\n\nexclude\n' > "$profile/mixed_answers.txt"
}

case "$PROFILE_WORKLOAD" in
    synthetic | all)
        printf 'yes\n' > "$profile/synthetic_answers.txt"
        PROFILE_YEAR=2024 profile_workload synthetic "$profile/synthetic_answers.txt" \
            --taxpayer "$cli/taxpayer.json" \
            --trade-republic "$cli/report_fixture.csv" "$cli/report_income_fixture.csv" \
            --ecb-rates "$cli/report_ecb_rates.csv"
        ;;
esac

case "$PROFILE_WORKLOAD" in
    mixed | all)
        write_mixed_answers
        PROFILE_YEAR=2024 profile_workload mixed "$profile/mixed_answers.txt" \
            --taxpayer "$cli/taxpayer.json" \
            --trade-republic "$manual/trade_republic_2022.csv" "$manual/trade_republic_2023.csv" \
            "$manual/trade_republic_2024.csv" "$manual/trade_republic_2025.csv" \
            --ibkr "$manual/ibkr_2022.xml" "$manual/ibkr_2023.xml" "$manual/ibkr_2024.xml" "$manual/ibkr_2025.xml"
        ;;
esac

case "$PROFILE_WORKLOAD" in
    large | all)
        generate_large_csv
        echo "==> Generated $(wc -l < "$profile/input/large_trade_republic_$PROFILE_ROWS.csv") lines."
        printf 'yes\n' > "$profile/large_answers.txt"
        PROFILE_YEAR=2024 profile_workload "large_$PROFILE_ROWS" "$profile/large_answers.txt" \
            --taxpayer "$cli/taxpayer.json" \
            --trade-republic "$profile/input/large_trade_republic_$PROFILE_ROWS.csv"
        ;;
esac

if [ "$PROFILE_WORKLOAD" = "custom" ]; then
    if [ "$PROFILE_CUSTOM_NO_INPUT" = "true" ]; then
        # shellcheck disable=SC2086
        profile_workload custom none --no-input --taxpayer "$root/$PROFILE_TAXPAYER" $PROFILE_CUSTOM_FILES
    else
        # shellcheck disable=SC2086
        profile_workload custom none --taxpayer "$root/$PROFILE_TAXPAYER" $PROFILE_CUSTOM_FILES
    fi
fi

exit "$failed"
INNER
exit_code=$?
set -e

if [[ "$exit_code" -ne 0 ]]; then
    echo "==> profile.sh failed (exit code $exit_code)." >&2
    exit "$exit_code"
fi

echo "==> Done. Results are in runtime/profile/."
