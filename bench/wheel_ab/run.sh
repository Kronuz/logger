#!/bin/sh
# Builds bench/wheel_ab and runs it (docs/perf/wheel-ab.md). Run every timed mode in a job:
#   campaign job submit --quest logger-wheel-ab --level exclusive -- bench/wheel_ab/run.sh row 2 4
#
#   run.sh build                   Release build in $WAB_BUILD (default .scratch/wab)
#   run.sh check                   --check for every backend, pattern and P = 1, 4, 14
#   run.sh tsan                    (B)'s --check under ThreadSanitizer (its own build), P = 4 and 14
#   run.sh smoke                   each backend and pattern once, small, its configuration asserted
#   run.sh row PATTERN P [fmt]     one row: an A/A pair, then AB BA AB BA AB BA; the rule's verdict
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${WAB_BUILD:-$ROOT/.scratch/wab}
HERE=$ROOT/bench/wheel_ab

build() {  # build DIR BUILD_TYPE [extra cmake args]
	dir=$1
	type=$2
	shift 2
	cmake -S "$ROOT" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$type" -DLOGGER_BUILD_WHEEL_AB=ON "$@" >/dev/null
	cmake --build "$dir" --target logger_wheel_ab >/dev/null
}

describe() {  # what is timed: commit, host, toolchain, binary
	echo "commit $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "host $(sysctl -n machdep.cpu.brand_string) $(sysctl -n hw.ncpu) threads, $(sw_vers -productName) $(sw_vers -productVersion)"
	echo "compiler $(c++ --version | head -1)"
	file "$1"
}

mode=${1:-}
case $mode in
build)
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab"
	;;
check)
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab"
	fail=0
	for backend in A B; do
		for pattern in 1 2 3; do
			for p in 1 4 14; do
				"$BUILD/logger_wheel_ab" --backend $backend --pattern $pattern --producers $p --check | grep '^check' || fail=1
			done
		done
	done
	exit $fail
	;;
tsan)
	dir=$BUILD-tsan
	build "$dir" RelWithDebInfo -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
	describe "$dir/logger_wheel_ab"
	fail=0
	for pattern in 1 2 3; do
		for p in 4 14; do
			TSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=66 \
				"$dir/logger_wheel_ab" --backend B --pattern $pattern --producers $p --check --rounds 1 --bursts 4 \
				| grep '^check' || fail=1
		done
	done
	exit $fail
	;;
smoke)
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab"
	out=$(mktemp "$BUILD/smoke.XXXXXX")
	for backend in A B; do
		for pattern in 1 2 3; do
			"$BUILD/logger_wheel_ab" --backend $backend --pattern $pattern --producers 4 --rounds 1 --bursts 2 | tee -a "$out"
		done
	done
	python3 -I "$HERE/summarize.py" --smoke "$out"
	;;
row)
	pattern=${2:?pattern}
	p=${3:?producers}
	fmt=
	if [ "${4:-}" = fmt ]; then
		fmt=--formatted
	fi
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab"
	out=$(mktemp "$BUILD/row.XXXXXX")
	for backend in A A A B B A A B B A A B B A; do
		"$BUILD/logger_wheel_ab" --backend $backend --pattern "$pattern" --producers "$p" $fmt | tee -a "$out"
	done
	python3 -I "$HERE/summarize.py" "$out"
	;;
*)
	sed -n '2,10p' "$0"
	exit 2
	;;
esac
