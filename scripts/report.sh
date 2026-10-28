#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

source "$script_dir/lib.sh"

synthetic_csv="tests/test_data/cli/report_fixture.csv"
synthetic_taxpayer="tests/test_data/cli/taxpayer.json"

usage() {
    cat <<EOF
Usage: report.sh [--reuse-build] [--synthetic] [--year YYYY] [--taxpayer path.json]
                 [--self-report] [--no-input] [--out runtime/dir] [--] [csv-path ...]

    Runs 'taxbroker report' inside Docker: Trade Republic CSVs in, the Doh-KDVP XML
    and summary_<year>.txt out. Paths are relative to the repository. The output can
    contain private financial data and stays below ignored runtime/. The exit code is
    the command's: 0 written or no data, 1 blocked, 2 failed.
    'taxbroker report --help' (printed with --command-help) explains the command.
    Options and CSV paths can come in any order; everything after -- is a CSV path.

    --reuse-build   run the existing build/dev and compile nothing
    --synthetic     use the synthetic CSV $synthetic_csv
                    (trades in 2024) and $synthetic_taxpayer
    --taxpayer      the taxpayer JSON; keep a real one below runtime/, for example
                    runtime/taxpayer.json (required without --synthetic)
    --self-report   write a self-report instead of an original return
    --no-input      ask no questions: an open question blocks the report
                    (without it, the command asks in this terminal)
    --command-help  print 'taxbroker report --help' and stop

    Default year:   the previous calendar year (2024 with --synthetic)
    Default input:  tmp/TransactionExport.csv
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
csv_inputs=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --reuse-build)
            reuse_build=true
            shift
            ;;
        --synthetic)
            synthetic=true
            shift
            ;;
        --command-help)
            command_help=true
            shift
            ;;
        --self-report)
            cli_options+=(--self-report)
            shift
            ;;
        --no-input)
            cli_options+=(--no-input)
            shift
            ;;
        --year)
            if [[ $# -lt 2 || ! "$2" =~ ^[0-9]{4}$ ]]; then
                echo "--year requires a four-digit year." >&2
                exit 2
            fi

            year="$2"
            shift 2
            ;;
        --taxpayer)
            if [[ $# -lt 2 ]]; then
                echo "--taxpayer requires a path." >&2
                exit 2
            fi

            taxpayer="$2"
            shift 2
            ;;
        --out)
            if [[ $# -lt 2 ]]; then
                echo "--out requires a path." >&2
                exit 2
            fi

            output_dir="$2"
            shift 2
            ;;
        --)
            shift
            csv_inputs+=("$@")
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            csv_inputs+=("$1")
            shift
            ;;
    esac
done

prepare_dev_build "$reuse_build"

if [[ "$command_help" == true ]]; then
    compose run --rm -e TBR_LOG_STDOUT=false -e TBR_LOG_FILE=/workspace/runtime/logs/taxbroker_report.log \
        dev /workspace/build/dev/src/taxbroker report --help
    exit 0
fi

if [[ "$synthetic" == true ]]; then
    if [[ ${#csv_inputs[@]} -gt 0 ]]; then
        echo "--synthetic takes no CSV paths." >&2
        exit 2
    fi

    csv_inputs=("$synthetic_csv")
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
    csv_inputs=(tmp/TransactionExport.csv)
fi

# Resolve the destination before writing so ../ and symlinks cannot escape runtime/.
output_absolute="$(realpath -m -- "$repo_root/$output_dir")"
runtime_absolute="$repo_root/runtime"

if [[ "$output_dir" != runtime/* || "$output_absolute" != "$runtime_absolute/"* ]]; then
    echo "Output must remain below runtime/." >&2
    exit 2
fi

for input_path in "$taxpayer" "${csv_inputs[@]}"; do
    if [[ "$input_path" == /* || "$input_path" == -* || ! -f "$repo_root/$input_path" ]]; then
        echo "Input must be an existing repository-relative path: $input_path" >&2
        exit 2
    fi
done

csv_paths=()

for csv_path in "${csv_inputs[@]}"; do
    csv_paths+=("/workspace/$csv_path")
done

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
    -- "${csv_paths[@]}"
exit_code=$?
set -e

echo "==> taxbroker report finished with exit code $exit_code."
exit "$exit_code"
