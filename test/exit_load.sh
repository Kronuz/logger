#!/bin/sh
# Runs the exit-without-finish child of logger_queue_test N times (default 500) while one busy loop
# per CPU competes with it; each run must exit 0 and write every line. Exits with the first failure.
#   test/exit_load.sh BUILD_DIR [N]
set -u
BUILD=$1
N=${2:-500}
OUT=$BUILD/exit_load.out
CPUS=$(getconf _NPROCESSORS_ONLN)
pids=
i=0
while [ "$i" -lt "$CPUS" ]; do
	(while :; do :; done) &
	pids="$pids $!"
	i=$((i + 1))
done
trap 'kill $pids 2>/dev/null' EXIT INT TERM
i=0
while [ "$i" -lt "$N" ]; do
	rm -f "$OUT"
	"$BUILD/logger_queue_test" exit "$OUT" || { echo "run $i: child exited $?"; exit 1; }
	n=$(grep -c 'exit t' "$OUT")
	[ "$n" -eq 3600 ] || { echo "run $i: $n of 3600 lines"; exit 1; }
	i=$((i + 1))
done
echo "exit child: $N runs under load, all exit 0 with every line"
