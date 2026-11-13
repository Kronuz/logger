#!/bin/sh
# The mutant test/exit.cc must kill: the exit-time join times out and the LOG thread is not stopped
# (abandon() does nothing), so it writes through handlers the statics' destruction freed. Builds a
# copy of the tree with logger.cc changed and runs logger_exit_test; exits 0 only if it fails.
#   test/exit_mutant.sh [BUILD_ROOT]     (default .scratch/mutant-exit)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/.scratch/mutant-exit}
src=$OUT/src
rm -rf "$src"
mkdir -p "$src"
git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 tar cf -) | tar xf - -C "$src"
grep -qF 'worker->abandon();' "$src/logger.cc" || { echo "mutant: its line is gone"; exit 2; }
python3 -I -c 'import sys; p = sys.argv[1]; s = open(p).read(); open(p, "w").write(s.replace("worker->abandon();", ";"))' "$src/logger.cc"
cmake -S "$src" -B "$OUT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$OUT/cfg.log" 2>&1
cmake --build "$OUT/build" --target logger_exit_test >"$OUT/build.log" 2>&1 || { tail -40 "$OUT/build.log"; exit 2; }
if "$OUT/build/logger_exit_test"; then
	echo "mutant: survived"
	exit 1
fi
echo "mutant: killed"
