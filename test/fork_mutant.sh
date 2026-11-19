#!/bin/sh
# The mutant test/exit.cc's `fork` mode must kill: the fork child keeps the parent LOG thread's mutex
# and condition variable (no pthread_atfork reset), so a child forked while the LOG thread held the
# mutex blocks when it logs. Builds a copy of the tree with logger.cc changed and runs `logger_exit_test fork`; exits 0
# only if the test fails on it. With `both`, runs the fixed tree's test first.
#   test/fork_mutant.sh [BUILD_ROOT] [both]     (default .scratch/mutant)
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
	cmake --build "$OUT/$1/build" --target logger_exit_test -j 4 >>"$OUT/$1.log" 2>&1 \
		|| { tail -40 "$OUT/$1.log"; exit 2; }
}

mkdir -p "$OUT"
if [ "${2:-}" = both ]; then
	build fork-fixed
	"$OUT/fork-fixed/build/logger_exit_test" fork || { echo "fixed tree: failed"; exit 2; }
fi
build fork-no-reset '::pthread_atfork(nullptr, nullptr, &LogWorker::after_fork_child);' '(void)&LogWorker::after_fork_child;'
start=$(date +%s)
if "$OUT/fork-no-reset/build/logger_exit_test" fork; then
	echo "mutant fork-no-reset: survived"
	exit 1
fi
echo "mutant fork-no-reset: killed after $(( $(date +%s) - start )) s"
