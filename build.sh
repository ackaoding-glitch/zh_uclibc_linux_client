#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<'HELP_EOF'
Usage:
  ./build.sh [options]

Unified entry point that builds and packages zh_client for RV1106.
Artifacts are not pushed to a board unless --push is specified.

Options:
  --push                Push packaged artifacts to the board after building
  --push-only           Skip build, only push existing artifacts and run
  --run-only            Skip build and push, only run zh_client on board
  --push-item NAME      Push only the named file/dir (repeatable; implies --push)
  -h, --help            Show this help

All other options are passed to the platform build script. Run the
following command for platform-specific options:
  ./scripts/rv1106_build_all.sh --help
HELP_EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

PUSH=0
SKIP_PUSH=0
ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --push) PUSH=1 ;;
        --push-only)
            PUSH=1
            ARGS+=("$1")
            ;;
        --run-only)
            PUSH=1
            ARGS+=("$1")
            ;;
        --skip-push)
            SKIP_PUSH=1
            ARGS+=("$1")
            ;;
        --push-item)
            PUSH=1
            ARGS+=("$1")
            shift
            ARGS+=("${1:?missing --push-item value}")
            ;;
        -h|--help)
            exec "$ROOT_DIR/scripts/rv1106_build_all.sh" --help
            ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done

if [ "$PUSH" -eq 1 ] && [ "$SKIP_PUSH" -eq 1 ]; then
    echo "--push and --skip-push cannot be used together" >&2
    exit 2
fi

if [ "$PUSH" -eq 0 ]; then
    [ "$SKIP_PUSH" -eq 1 ] || ARGS+=(--skip-push)
fi

exec "$ROOT_DIR/scripts/rv1106_build_all.sh" "${ARGS[@]}"
