#!/bin/sh
# Builds bench/wheel_ab and runs it (docs/perf/wheel-ab.md). Run every timed mode in a job:
#   campaign job submit --quest logger-wheel-ab --level exclusive -- bench/wheel_ab/run.sh row 2 4
#
#   run.sh build                   Release build in $WAB_BUILD (default .scratch/wab)
#   run.sh check                   --check for every backend, pattern and P = 1, 4, 14
#   run.sh tsan                    (B)'s --check under ThreadSanitizer (its own build), P = 4 and 14
#   run.sh smoke                   each backend and pattern once, small, its configuration asserted
#   run.sh row PATTERN P [fmt]     one row: an A/A pair, then AB BA AB BA AB BA; the rule's verdict
#   run.sh check-c | tsan-c        the same checks for arm (C), which must also keep (A)'s order
#   run.sh row-c PATTERN P         one row of A against C (patterns 2 and 3; in pattern 1 C is A)
#   run.sh sizeof                  sizeof(Logging) of A, C and D (arm D's own build, d/CMakeLists.txt)
#   run.sh check-d | tsan-d        the same checks for arm (D), which must also keep (A)'s order
#   run.sh row-d PATTERN P [R]     one row of A against D under D77: R rounds (default 40), each
#                                  running A, its twin A2 and D once in a rotated order
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${WAB_BUILD:-$ROOT/.scratch/wab}
HERE=$ROOT/bench/wheel_ab

build() {  # build DIR BUILD_TYPE [extra cmake args]
	dir=$1
	type=$2
	shift 2
	cmake -S "$ROOT" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$type" -DLOGGER_BUILD_WHEEL_AB=ON "$@" >/dev/null
	cmake --build "$dir" --target logger_wheel_ab logger_wheel_ab_c >/dev/null
}

describe() {  # what is timed: commit, host, toolchain, binary
	echo "commit $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "host $(sysctl -n machdep.cpu.brand_string) $(sysctl -n hw.ncpu) threads, $(sw_vers -productName) $(sw_vers -productVersion)"
	echo "compiler $(c++ --version | head -1)"
	file "$1"
}

BUILD_D=${WAB_BUILD_D:-$ROOT/.scratch/wab-d}

build_d() {  # build_d DIR BUILD_TYPE [extra cmake args]: A, B and D against wheel@45f98eb
	dir=$1
	type=$2
	shift 2
	cmake -S "$HERE/d" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$type" "$@" >/dev/null
	cmake --build "$dir" >/dev/null
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
check-c)
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab_c"
	fail=0
	for pattern in 1 2 3; do
		for p in 1 4 14; do
			"$BUILD/logger_wheel_ab_c" --backend C --pattern $pattern --producers $p --check | grep '^check' || fail=1
		done
	done
	exit $fail
	;;
tsan-c)
	dir=$BUILD-tsan
	build "$dir" RelWithDebInfo -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
	describe "$dir/logger_wheel_ab_c"
	fail=0
	for pattern in 1 2 3; do
		for p in 4 14; do
			TSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=66 \
				"$dir/logger_wheel_ab_c" --backend C --pattern $pattern --producers $p --check --rounds 1 --bursts 4 \
				| grep '^check' || fail=1
		done
	done
	exit $fail
	;;
row-c)
	pattern=${2:?pattern}
	p=${3:?producers}
	build "$BUILD" Release
	describe "$BUILD/logger_wheel_ab"
	describe "$BUILD/logger_wheel_ab_c"
	out=$(mktemp "$BUILD/row-c.XXXXXX")
	for backend in A A A C C A A C C A A C C A; do
		bin=$BUILD/logger_wheel_ab
		if [ $backend = C ]; then
			bin=$BUILD/logger_wheel_ab_c
		fi
		"$bin" --backend $backend --pattern "$pattern" --producers "$p" | tee -a "$out"
	done
	python3 -I "$HERE/summarize.py" "$out"
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
sizeof)
	build_d "$BUILD_D" Release
	describe "$BUILD_D/sizeof_a"
	for arm in a c d; do
		echo "arm $arm"
		"$BUILD_D/sizeof_$arm"
	done
	;;
check-d)
	build_d "$BUILD_D" Release
	describe "$BUILD_D/logger_wheel_ab_d"
	fail=0
	for pattern in 1 2 3; do
		for p in 1 4 14; do
			"$BUILD_D/logger_wheel_ab_d" --backend D --pattern $pattern --producers $p --check | grep '^check' || fail=1
		done
	done
	exit $fail
	;;
tsan-d)
	dir=$BUILD_D-tsan
	build_d "$dir" RelWithDebInfo -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
	describe "$dir/logger_wheel_ab_d"
	fail=0
	for pattern in 1 2 3; do
		for p in 4 14; do
			TSAN_OPTIONS=halt_on_error=1:abort_on_error=0:exitcode=66 \
				"$dir/logger_wheel_ab_d" --backend D --pattern $pattern --producers $p --check --rounds 1 --bursts 4 \
				| grep '^check' || fail=1
		done
	done
	exit $fail
	;;
row-d)
	pattern=${2:?pattern}
	p=${3:?producers}
	rounds=${4:-40}
	build_d "$BUILD_D" Release
	describe "$BUILD_D/logger_wheel_ab"
	describe "$BUILD_D/logger_wheel_ab_d"
	out=$(mktemp "$BUILD_D/row-d.XXXXXX")
	set -- A A2 D
	n=$#
	r=0
	while [ $r -lt "$rounds" ]; do
		k=0
		while [ $k -lt $n ]; do
			# arm number (k + r) mod n: every arm takes every place in the round's order
			i=$(( (k + r) % n ))
			case $i in 0) arm=A ;; 1) arm=A2 ;; 2) arm=D ;; esac
			bin=$BUILD_D/logger_wheel_ab
			backend=A
			if [ $arm = D ]; then
				bin=$BUILD_D/logger_wheel_ab_d
				backend=D
			fi
			echo "run round=$r arm=$arm" | tee -a "$out"
			"$bin" --backend $backend --pattern "$pattern" --producers "$p" --rounds 1 | grep '^{' | tee -a "$out"
			k=$((k + 1))
		done
		r=$((r + 1))
	done
	python3 -I "$HERE/summarize.py" --paired "$out"
	;;
*)
	sed -n '2,10p' "$0"
	exit 2
	;;
esac
