#!/bin/sh
# Mutants of the LOG thread's wheel that test/deferred.cc must kill: a deferred line filed 3 ms
# early, and 3 ms late, and a batch that leaves lines tied on due and call time in any order. Each
# builds a copy of the tree with logger.cc changed and runs logger_deferred_test (the ties mutant
# with its `ties` argument); exits 0 only if the test fails on every mutant. The late mutant is
# killed by the lateness bound, so the test runs with DEFERRED_LATENESS=1.
#   test/deferred_mutant.sh [BUILD_ROOT]     (default .scratch/mutant)
set -eu
export DEFERRED_LATENESS=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/.scratch/mutant}

mutant() {  # mutant NAME ORIGINAL REPLACEMENT [TEST ARGUMENT]
	name=$1
	src=$OUT/$name/src
	rm -rf "$src"
	mkdir -p "$src"
	git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 tar cf -) | tar xf - -C "$src"
	grep -qF "$2" "$src/logger.cc" || { echo "mutant $name: its line is gone"; exit 2; }
	python3 -I -c 'import sys; p, a, b = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(a, b))' \
		"$src/logger.cc" "$2" "$3"
	cmake -S "$src" -B "$OUT/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$OUT/$name.log" 2>&1
	cmake --build "$OUT/$name/build" --target logger_deferred_test >>"$OUT/$name.log" 2>&1 \
		|| { tail -40 "$OUT/$name.log"; exit 2; }
	if "$OUT/$name/build/logger_deferred_test" ${4:-}; then
		echo "mutant $name: survived"
		return 1
	fi
	echo "mutant $name: killed"
}

mkdir -p "$OUT"
fail=0
ARM='wheel_->arm(l, static_cast<std::uint64_t>(due));'
mutant early "$ARM" 'wheel_->arm(l, static_cast<std::uint64_t>(due - 3 * kTickNs));' || fail=1
mutant late "$ARM" 'wheel_->arm(l, static_cast<std::uint64_t>(due + 3 * kTickNs));' || fail=1
mutant ties 'return static_cast<std::int32_t>(a.seq - b.seq) < 0;' 'return false;' ties || fail=1
exit $fail
