#!/bin/sh
# Runs the one-producer deferred test with its lateness bounds asserted (DEFERRED_LATENESS=1):
# a line is written within one 1 ms tick plus the measured timed-wake lateness. Run it on a quiet
# machine; shared CI runners and loaded machines have no such bound.
#   test/deferred_lateness.sh BUILD_DIR [N]
set -u
BUILD=$1
N=${2:-20}
DEFERRED_LATENESS=1 ctest --test-dir "$BUILD" -R 'logger_deferred$' --repeat "until-fail:$N" --output-on-failure
