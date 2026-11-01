# Tab completion for the scripts in this folder, for bash and zsh. Add to ~/.bashrc or
# ~/.zshrc (in zsh after compinit):
#
#   source /path/to/TaxBrokerReport/scripts/completion.bash
#
# Paths complete from the current folder, so run the scripts from the repository root.
# Options and paths complete in any order. Keep the lists below in step with each usage().
#
# A spec is "--flag", "--option=kind" (one value per comma-separated kind) or "+--option=..."
# for an option that can repeat. After "::" come the kinds of the positional arguments;
# a last kind ending in "+" repeats. Kinds: csv, json, file, dir, year, none, "words:a b c".

# Prints the bash candidates of one kind for the word being completed.
_tbr_candidates() {
    local kind="$1"
    local current="$2"
    local year
    local IFS=$' \t\n'

    case "$kind" in
        csv | json)
            compgen -d -- "$current"
            compgen -f -X "!*.$kind" -- "$current"
            ;;
        file)
            compgen -f -- "$current"
            ;;
        dir)
            compgen -d -- "$current"
            ;;
        year)
            year=$(( $(date +%Y) - 1 ))
            compgen -W "$year $(( year - 1 )) $(( year - 2 )) $(( year - 3 )) $(( year - 4 ))" -- "$current"
            ;;
        words:*)
            compgen -W "${kind#words:}" -- "$current"
            ;;
    esac
}

# Adds the candidates of one kind; fails when there are none. Bash fills COMPREPLY with one
# candidate per line, so paths with spaces stay whole; zsh uses its own completers.
if [[ -n "${ZSH_VERSION:-}" ]]; then
    _tbr_reply() {
        emulate -L zsh
        setopt "${_comp_options[@]}"
        local year=$(( $(date +%Y) - 1 ))

        case "$1" in
            csv | json) _files -g "*.$1(-.)" ;;
            file) _files ;;
            dir) _directories ;;
            year) compadd -- $year $(( year - 1 )) $(( year - 2 )) $(( year - 3 )) $(( year - 4 )) ;;
            words:*) compadd -- ${=${1#words:}} ;;
            *) return 1 ;;
        esac
    }
else
    _tbr_reply() {
        local IFS=$'\n'
        COMPREPLY=($(_tbr_candidates "$1" "$2"))
        [[ ${#COMPREPLY[@]} -gt 0 ]]
    }
fi

_tbr_complete() {
    local -a specs=()
    local -a positional=()
    local -a used=()
    local -a value_kinds=()
    local -a candidates=()
    local current="${COMP_WORDS[COMP_CWORD]}"
    local index=1
    local position=0
    local after_separator=false
    local spec name word kind rest

    while [[ $# -gt 0 && "$1" != "::" ]]; do
        specs+=("$1")
        shift
    done

    [[ $# -gt 0 ]] && shift
    positional=("$@")

    # Walk the words before the cursor: skip option values, count positional arguments.
    while (( index < COMP_CWORD )); do
        word="${COMP_WORDS[index]}"
        index=$(( index + 1 ))

        if [[ "$after_separator" == true || "$word" != -* ]]; then
            position=$(( position + 1 ))
            continue
        fi

        used+=("$word")

        for spec in "${specs[@]}"; do
            spec="${spec#+}"
            name="${spec%%=*}"

            if [[ "$word" == "$name" && "$name" == "--" ]]; then
                after_separator=true
            elif [[ "$word" == "$name" && "$spec" == *=* ]]; then
                value_kinds=()
                rest="${spec#*=}"

                while [[ "$rest" == *,* ]]; do
                    value_kinds+=("${rest%%,*}")
                    rest="${rest#*,}"
                done

                value_kinds+=("$rest")

                # The cursor is on one of this option's values.
                if (( index + ${#value_kinds[@]} > COMP_CWORD )); then
                    _tbr_reply "${value_kinds[COMP_CWORD - index]}" "$current"
                    return 0
                fi

                index=$(( index + ${#value_kinds[@]} ))
            fi
        done
    done

    if [[ "$current" != -* || "$after_separator" == true ]]; then
        kind=""

        if (( position < ${#positional[@]} )); then
            kind="${positional[position]}"
        elif (( ${#positional[@]} > 0 )) && [[ "${positional[${#positional[@]} - 1]}" == *+ ]]; then
            kind="${positional[${#positional[@]} - 1]}"
        fi

        if [[ -n "$kind" ]] && _tbr_reply "${kind%+}" "$current"; then
            return 0
        fi

        if [[ "$after_separator" == true ]]; then
            return 0
        fi
    fi

    # Offer the options that are not used yet; repeatable ones stay.
    for spec in "${specs[@]}"; do
        name="${spec%%=*}"

        if [[ "$name" != +* ]]; then
            for word in "${used[@]}"; do
                [[ "$word" == "$name" ]] && continue 2
            done
        fi

        candidates+=("${name#+}")
    done

    _tbr_reply "words:${candidates[*]}" "$current"
}

_tbr_build() { _tbr_complete --help :: "words:dev image docker-build prod"; }
_tbr_coverage() { _tbr_complete --help; }
_tbr_cppcheck() { _tbr_complete --reuse-build --help; }
_tbr_valgrind() { _tbr_complete --reuse-build --help; }
_tbr_fuzz() { _tbr_complete --seconds=none --replay=file --help; }
_tbr_format() { _tbr_complete --check --help; }
_tbr_test() { _tbr_complete --reuse-build "--suite=words:unit integration" --help :: none; }
_tbr_debug_tests() { _tbr_complete --help :: "words:build" "words:unit integration"; }
_tbr_dump_tr_parse() { _tbr_complete --help :: csv file file; }
_tbr_dump_tr_merge() { _tbr_complete --help :: csv file dir; }
_tbr_dump_tr_diagnostics() { _tbr_complete --output=file --help :: csv+; }
_tbr_dump_tr_fifo() { _tbr_complete --year=year --output=file --help :: csv+; }

_tbr_dump_tr_kdvp() {
    _tbr_complete --synthetic --year=year --tax-number=none --self-report --output=file --help :: csv+
}

_tbr_dump_exact_arithmetic() {
    _tbr_complete --output=file "+--split-row=none,none,none" --help :: csv
}

_tbr_report() {
    _tbr_complete --reuse-build --synthetic --year=year --taxpayer=json --self-report --no-input \
        --out=dir --command-help --help -- :: csv+
}

_tbr_scripts=(build coverage cppcheck valgrind fuzz format test debug_tests dump_tr_parse
    dump_tr_merge dump_tr_diagnostics dump_tr_fifo dump_tr_kdvp dump_exact_arithmetic report)

if [[ -n "${ZSH_VERSION:-}" ]]; then
    # The shared functions above expect bash's zero-based arrays.
    _tbr_zsh() {
        local -a COMP_WORDS=("${words[@]}")
        local COMP_CWORD=$(( CURRENT - 1 ))
        local script="${${words[1]:t}%.sh}"

        setopt localoptions ksharrays
        "_tbr_$script"
    }

    if (( $+functions[compdef] )); then
        compdef _tbr_zsh "${_tbr_scripts[@]/%/.sh}"
    else
        echo "completion.bash: run compinit before sourcing it." >&2
    fi
else
    for _tbr_script in "${_tbr_scripts[@]}"; do
        complete -o filenames -F "_tbr_$_tbr_script" "$_tbr_script.sh"
    done

    unset _tbr_script
fi

unset _tbr_scripts
