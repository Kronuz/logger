#!/bin/sh
# Mutants of the LOG thread's wheel that test/deferred.cc must kill: a deferred line filed 3 ms
# early, and 3 ms late. Each builds a copy of the tree with logger.cc changed and runs
# logger_deferred_test; exits 0 only if the test fails on every mutant.
#   test/deferred_mutant.sh [BUILD_ROOT]     (default .scratch/mutant)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/.scratch/mutant}
ARM='wheel_->arm(l, static_cast<std::uint64_t>(due));'

mutant() {  # mutant NAME REPLACEMENT
	name=$1
	src=$OUT/$name/src
	rm -rf "$src"
	mkdir -p "$src"
	git -C "$ROOT" ls-files -z | (cd "$ROOT" && xargs -0 tar cf -) | tar xf - -C "$src"
	grep -qF "$ARM" "$src/logger.cc" || { echo "mutant $name: the arm call is gone"; exit 2; }
	python3 -I -c 'import sys; p, a, b = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(a, b))' \
		"$src/logger.cc" "$ARM" "$2"
	cmake -S "$src" -B "$OUT/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$OUT/$name.log" 2>&1
	cmake --build "$OUT/$name/build" --target logger_deferred_test >>"$OUT/$name.log" 2>&1 \
		|| { tail -40 "$OUT/$name.log"; exit 2; }
	if "$OUT/$name/build/logger_deferred_test"; then
		echo "mutant $name: survived"
		return 1
	fi
	echo "mutant $name: killed"
}

mkdir -p "$OUT"
fail=0
mutant early 'wheel_->arm(l, static_cast<std::uint64_t>(due - 3 * kTickNs));' || fail=1
mutant late 'wheel_->arm(l, static_cast<std::uint64_t>(due + 3 * kTickNs));' || fail=1
exit $fail
