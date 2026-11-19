#!/bin/sh
# The mutants test/wake.cc must kill: a routine line (wake-slack), or a queue-order task record
# (wake-slack-task), wakes the LOG thread when it is due within the deferred lines' slack
# (kWakeSlackNs) of the thread's wake, so a burst of them wakes it once a line. Builds a copy of the tree with logger.cc changed and runs logger_wake_test; exits 0
# only if the test fails on it. Prints the switch counts, the fixed tree's first with `both`.
#   test/wake_mutant.sh [BUILD_ROOT] [both]     (default .scratch/mutant)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/.scratch/mutant}

build() {  # build NAME [ORIGINAL REPLACEMENT]
	src=$OUT/$1/src
	rm -rf "$src"
	mkdir -p "$src"
	git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 tar cf -) | tar xf - -C "$src"
	if [ $# -gt 1 ]; then
		grep -qF "$2" "$src/logger.cc" || { echo "mutant $1: its line is gone"; exit 2; }
		python3 -I -c 'import sys; p, a, b = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(a, b))' \
			"$src/logger.cc" "$2" "$3"
	fi
	cmake -S "$src" -B "$OUT/$1/build" -DCMAKE_BUILD_TYPE=Release >"$OUT/$1.log" 2>&1
	cmake --build "$OUT/$1/build" --target logger_wake_test -j 4 >>"$OUT/$1.log" 2>&1 \
		|| { tail -40 "$OUT/$1.log"; exit 2; }
}

mkdir -p "$OUT"
if [ "${2:-}" = both ]; then
	build wake-fixed
	"$OUT/wake-fixed/build/logger_wake_test" || echo "fixed tree: failed"
fi
build wake-slack 'LogWorker::get().wake_for(due);' 'LogWorker::get().wake_for(due - kWakeSlackNs);'
if "$OUT/wake-slack/build/logger_wake_test"; then
	echo "mutant wake-slack: survived"
	exit 1
fi
echo "mutant wake-slack: killed"
build wake-slack-task 'wake_for(wake_deadline(ordered, due));' 'wake_for(wake_deadline(false, due));'
if "$OUT/wake-slack-task/build/logger_wake_test"; then
	echo "mutant wake-slack-task: survived"
	exit 1
fi
echo "mutant wake-slack-task: killed"
