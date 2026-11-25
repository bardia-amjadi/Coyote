#!/usr/bin/env bash
# Linux/macOS version of run-selftest.bat (see there). Needs coyote on the PATH.
cd "$(dirname "$0")/.." || exit 2
exec coyote run selftest/Runner/src/main.coy -- "$@"
