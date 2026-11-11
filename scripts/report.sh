#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

default_csv="tmp/TransactionExport.csv"
synthetic_csv="tests/test_data/cli/report_fixture.csv"
synthetic_income_csv="tests/test_data/cli/report_income_fixture.csv"
synthetic_ecb="tests/test_data/cli/report_ecb_rates.csv"
synthetic_taxpayer="tests/test_data/cli/taxpayer.json"

usage() {
    cat <<EOF
Usage: report.sh [--reuse-build] [--synthetic] [--year YYYY] [--taxpayer path.json]
                 [--self-report] [--no-input] [--out runtime/dir]
                 [--trade-republic path.csv ...] [--ecb-rates path.csv ...]
                 [--reference-files path.csv ...]

    Runs 'taxbroker report' inside Docker: Trade Republic CSVs in, the Doh-KDVP XML
    and summary_<year>.txt out. Paths are relative to the repository. The output can
    contain private financial data and stays below ignored runtime/. The exit code is
    the command's: 0 written or no data, 1 blocked, 2 failed.
    'taxbroker report --help' (printed with --command-help) explains the command.
    The options can come in any order. A file option takes the paths after it up to the
    next option; a path outside a file option is an error. Give a path that starts with
    "-" as ./-name.csv.

    --reuse-build   run the existing build/dev and compile nothing
    --synthetic     use the synthetic CSVs $synthetic_csv (trades in 2024)
                    and $synthetic_income_csv (dividends, bank interest and bond
                    coupons in 2024), the synthetic USD rates $synthetic_ecb
                    (they replace the built-in 2024 year) and $synthetic_taxpayer;
                    it asks the Doh-DHO allowance question (yes writes Doh-DHO)
    --taxpayer      the taxpayer JSON; keep a real one below runtime/, for example
                    runtime/taxpayer.json (required without --synthetic)
    --self-report   write a self-report instead of an original return
    --no-input      ask no questions: an open question blocks the report
                    (without it, the command asks in this terminal)
    --trade-republic  one or more Trade Republic exports (default: $default_csv)
    --ecb-rates     ECB rate files (one calendar year each, EXR csvdata). The years 2016 to
                    2025 are built in; a file replaces its built-in year and adds other years
    --reference-files  a newer payers.csv, relief_statements.csv or broker_payers.csv; each
                    replaces the built-in file of the same name
    --command-help  print 'taxbroker report --help' and stop

    Default year:   the previous calendar year (2024 with --synthetic)
    Default output: runtime/report/<year>, or runtime/report/synthetic_<year>
EOF
}

reuse_build=false
synthetic=false
command_help=false
year=""
taxpayer=""
output_dir=""
cli_options=()
ecb_inputs=()
reference_inputs=()
csv_inputs=()
# The file option that takes the next plain paths, and how many it has so far.
list_option=""
list_count=0

# A file option needs at least one path before the next option.
end_list() {
    if [[ -n "$list_option" && "$list_count" -eq 0 ]]; then
        echo "$list_option needs at least one path." >&2
        exit 2
    fi

    list_option=""
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --trade-republic|--ecb-rates|--reference-files)
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
        --synthetic)
            end_list
            synthetic=true
            shift
            ;;
        --command-help)
            end_list
            command_help=true
            shift
            ;;
        --self-report)
            end_list
            cli_options+=(--self-report)
            shift
            ;;
        --no-input)
            end_list
            cli_options+=(--no-input)
            shift
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
        --out)
            end_list

            if [[ $# -lt 2 ]]; then
                echo "--out requires a path." >&2
                exit 2
            fi

            output_dir="$2"
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
                --ecb-rates)
                    ecb_inputs+=("$1")
                    ;;
                --reference-files)
                    reference_inputs+=("$1")
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

prepare_dev_build "$reuse_build"

if [[ "$command_help" == true ]]; then
    compose run --rm -e TBR_LOG_STDOUT=false -e TBR_LOG_FILE=/workspace/runtime/logs/taxbroker_report.log \
        dev /workspace/build/dev/src/taxbroker report --help
    exit 0
fi

if [[ "$synthetic" == true ]]; then
    if [[ ${#csv_inputs[@]} -gt 0 ]]; then
        echo "--synthetic takes no --trade-republic paths." >&2
        exit 2
    fi

    if [[ ${#ecb_inputs[@]} -gt 0 ]]; then
        echo "--synthetic takes no --ecb-rates paths." >&2
        exit 2
    fi

    csv_inputs=("$synthetic_csv" "$synthetic_income_csv")
    ecb_inputs=("$synthetic_ecb")
    year="${year:-2024}"
    taxpayer="${taxpayer:-$synthetic_taxpayer}"
    output_dir="${output_dir:-runtime/report/synthetic_$year}"
fi

year="${year:-$(( $(date +%Y) - 1 ))}"
output_dir="${output_dir:-runtime/report/$year}"

if [[ -z "$taxpayer" ]]; then
    echo "--taxpayer is required without --synthetic (for example runtime/taxpayer.json)." >&2
    exit 2
fi

if [[ ${#csv_inputs[@]} -eq 0 ]]; then
    csv_inputs=("$default_csv")
fi

# Resolve the destination before writing so ../ and symlinks cannot escape runtime/.
output_absolute="$(realpath -m -- "$repo_root/$output_dir")"
runtime_absolute="$repo_root/runtime"

if [[ "$output_dir" != runtime/* || "$output_absolute" != "$runtime_absolute/"* ]]; then
    echo "Output must remain below runtime/." >&2
    exit 2
fi

for input_path in "$taxpayer" "${csv_inputs[@]}" "${ecb_inputs[@]}" "${reference_inputs[@]}"; do
    if [[ "$input_path" == /* || "$input_path" == -* || ! -f "$repo_root/$input_path" ]]; then
        echo "Input must be an existing repository-relative path: $input_path" >&2
        exit 2
    fi
done

csv_paths=()

for csv_path in "${csv_inputs[@]}"; do
    csv_paths+=("/workspace/$csv_path")
done

if [[ ${#ecb_inputs[@]} -gt 0 ]]; then
    cli_options+=(--ecb-rates)

    for ecb_path in "${ecb_inputs[@]}"; do
        cli_options+=("/workspace/$ecb_path")
    done
fi

if [[ ${#reference_inputs[@]} -gt 0 ]]; then
    cli_options+=(--reference-files)

    for reference_path in "${reference_inputs[@]}"; do
        cli_options+=("/workspace/$reference_path")
    done
fi

mkdir -p "$repo_root/runtime/logs"

# The command reads its answers from this terminal; piped input needs no TTY in the container.
tty_options=()

if [[ ! -t 0 ]]; then
    tty_options+=(-T)
fi

echo "==> Writing the Doh-KDVP report for $year to $output_dir..."
set +e
compose run --rm "${tty_options[@]}" \
    -e TBR_LOG_STDOUT=false \
    -e TBR_LOG_FILE=/workspace/runtime/logs/taxbroker_report.log \
    dev /workspace/build/dev/src/taxbroker report \
    --year "$year" \
    --taxpayer "/workspace/$taxpayer" \
    "${cli_options[@]}" \
    --out "/workspace/$output_dir" \
    --trade-republic "${csv_paths[@]}"
exit_code=$?
set -e

echo "==> taxbroker report finished with exit code $exit_code."
exit "$exit_code"
