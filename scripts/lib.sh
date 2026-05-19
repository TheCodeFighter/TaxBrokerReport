#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
compose_file="$repo_root/docker/docker-compose.yml"

ensure_uid_gid_env() {
    # Export UID for compose user mapping and provide a default GID if missing.
    export UID

    if [[ -z "${GID:-}" ]]; then
        GID="$(id -g)"
    fi

    export GID
}

ensure_build_tree_writable() {
    local build_dir="$repo_root/build"
    local current_user
    local current_group
    current_user="$(id -un)"
    current_group="$(id -gn)"

    if [[ ! -d "$build_dir" ]]; then
        return 0
    fi

    if [[ ! -w "$build_dir" ]]; then
        cat <<EOF >&2
==> Build directory is not writable by the current user.
==> Fix on the host:
    sudo chown -R $current_user:$current_group "$build_dir"
EOF
        return 1
    fi

    local ninja_log="$build_dir/.ninja_log"
    if [[ -e "$ninja_log" && ! -w "$ninja_log" ]]; then
        cat <<EOF >&2
==> $ninja_log is not writable by the current user.
==> Fix on the host:
    sudo chown -R $current_user:$current_group "$build_dir"
EOF
        return 1
    fi
}

ensure_build_outputs_executable() {
    local build_dir="$repo_root/build"

    if [[ ! -d "$build_dir" ]]; then
        return 0
    fi

    if [[ ! -w "$build_dir" ]]; then
        ensure_build_tree_writable
        return 1
    fi

    local server_path="$build_dir/src/taxbroker_server"
    if [[ -e "$server_path" && ! -x "$server_path" ]]; then
        chmod u+x "$server_path"
    fi

    find "$build_dir" -maxdepth 4 -type f -name "*_tests" ! -executable -exec chmod u+x {} +
}

resolve_log_file_path() {
    local log_path="${TBR_LOG_FILE:-}"

    if [[ -z "$log_path" ]]; then
        log_path="logs/taxbroker.log"
    fi

    if [[ "$log_path" == /var/log/taxbroker ]]; then
        log_path="$repo_root/logs"
    elif [[ "$log_path" == /var/log/taxbroker/* ]]; then
        log_path="$repo_root/logs/${log_path#/var/log/taxbroker/}"
    elif [[ "$log_path" == /workspace/* ]]; then
        log_path="$repo_root/${log_path#/workspace/}"
    elif [[ "$log_path" != /* ]]; then
        log_path="$repo_root/$log_path"
    fi

    echo "$log_path"
}

resolve_container_log_file_path() {
    local log_path="${TBR_LOG_FILE:-}"

    if [[ -z "$log_path" ]]; then
        echo "/var/log/taxbroker/taxbroker.log"
        return 0
    fi

    if [[ "$log_path" == /var/log/taxbroker/* || "$log_path" == /workspace/* ]]; then
        echo "$log_path"
        return 0
    fi

    if [[ "$log_path" != /* ]]; then
        echo "/workspace/$log_path"
        return 0
    fi

    if [[ "$log_path" == "$repo_root/"* ]]; then
        echo "/workspace/${log_path#$repo_root/}"
        return 0
    fi

    return 1
}

ensure_log_path_writable() {
    local log_path
    log_path="$(resolve_log_file_path)"
    local current_user
    local current_group
    current_user="$(id -un)"
    current_group="$(id -gn)"
    if [[ "$log_path" == /* && "$log_path" != "$repo_root"* ]]; then
        return 0
    fi
    local log_dir
    log_dir="$(dirname "$log_path")"

    if [[ ! -d "$log_dir" ]]; then
        cat <<EOF >&2
==> Log directory does not exist: $log_dir
==> Fix on the host:
    mkdir -p "$log_dir"
    sudo chown -R $current_user:$current_group "$log_dir"
EOF
        return 1
    fi

    if [[ ! -w "$log_dir" ]]; then
        cat <<EOF >&2
==> Log directory is not writable by the current user: $log_dir
==> Fix on the host:
    sudo chown -R $current_user:$current_group "$log_dir"
EOF
        return 1
    fi

    if [[ -e "$log_path" && ! -w "$log_path" ]]; then
        cat <<EOF >&2
==> Log file is not writable by the current user: $log_path
==> Fix on the host:
    sudo chown -R $current_user:$current_group "$log_dir"
EOF
        return 1
    fi
}

compose() {
    ensure_uid_gid_env
    docker compose --project-directory "$repo_root/docker" -f "$compose_file" "$@"
}

ensure_dev_image() {
    if ! docker image inspect taxbrokerreport-dev >/dev/null 2>&1; then
        compose build dev
    fi
}