#!/bin/sh
# Runs the deferred tests (the one-producer run, `stress` and `ties`) N times (default 50) while one
# busy loop per CPU competes with them, so a producer is preempted between a call and its clear().
# The lateness bound is off (a loaded machine has no bound); once, order and never early are checked.
# Exits with the first failure.
#   test/deferred_load.sh BUILD_DIR [N]
set -u
export DEFERRED_LOADED=1
BUILD=$1
N=${2:-50}
CPUS=$(getconf _NPROCESSORS_ONLN)
pids=
i=0
while [ "$i" -lt "$CPUS" ]; do
	(while :; do :; done) &
	pids="$pids $!"
	i=$((i + 1))
done
trap 'kill $pids 2>/dev/null' EXIT INT TERM
ctest --test-dir "$BUILD" -R logger_deferred --repeat "until-fail:$N" --output-on-failure
