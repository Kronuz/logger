#!/bin/sh
# The library's deferred lines on the wheel against arm D, the prototype it ports
# (docs/perf/wheel-port.md). bench/wheel_ab's harness and summary, with the labels of `run.sh row-d`
# swapped: the reference A and its A/A twin A2 are arm D (logger_wheel_ab_d), the candidate D is the
# library (logger_wheel_ab --backend A), so summarize.py's r = D/A is library / arm D.
#   docs/perf/wheel-port.sh PATTERN [ROUNDS]     P = 1, 4 and 14, ROUNDS each (default 40)
# Run it as one exclusive job per pattern. Exits 1 when a row is past 1.10 (median, p99 or heap).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$ROOT/bench/wheel_ab
BUILD=${WAB_BUILD_D:-$ROOT/.scratch/wab-d}
pattern=${1:?pattern}
rounds=${2:-40}

mkdir -p "$BUILD"
{ cmake -S "$HERE/d" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "$BUILD"; } >"$BUILD/build.log" 2>&1 \
	|| { tail -60 "$BUILD/build.log"; exit 1; }
echo "commit $(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "host $(sysctl -n machdep.cpu.brand_string) $(sysctl -n hw.ncpu) threads, $(sw_vers -productName) $(sw_vers -productVersion)"
echo "compiler $(c++ --version | head -1)"
echo "labels: A, A2 = arm D (reference); D = the library (candidate); r = library / arm D"

fail=0
for p in 1 4 14; do
	out=$BUILD/port-$pattern-$p.log
	: >"$out"
	r=0
	while [ $r -lt "$rounds" ]; do
		k=0
		while [ $k -lt 3 ]; do
			# arm number (k + r) mod 3: every arm takes every place in the round's order
			case $(( (k + r) % 3 )) in
			0) arm=A bin=$BUILD/logger_wheel_ab_d backend=D ;;
			1) arm=A2 bin=$BUILD/logger_wheel_ab_d backend=D ;;
			*) arm=D bin=$BUILD/logger_wheel_ab backend=A ;;
			esac
			echo "run round=$r arm=$arm" >>"$out"
			"$bin" --backend $backend --pattern "$pattern" --producers "$p" --rounds 1 | grep '^{' >>"$out"
			k=$((k + 1))
		done
		r=$((r + 1))
	done
	echo "== pattern $pattern, P = $p, $rounds rounds"
	python3 -I "$HERE/summarize.py" --paired "$out" | tee "$out.summary"
	grep -q 'no worse than A: True' "$out.summary" || fail=1
done
exit $fail   # 1: a row's median or p99 r is past 1.10, or its heap past 1.10 times arm D's
