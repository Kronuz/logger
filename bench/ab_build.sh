#!/bin/sh
# Builds logger_bench (Release) from each source tree and copies it to OUT/NAME, for logger_ab.py.
#   ab_build.sh OUT NAME=SRC [NAME=SRC ...]
# The trees must carry the same bench/lg.cc; the script refuses one that differs from the first.
set -eu
out=$1
shift
mkdir -p "$out"
first=
for arm in "$@"; do
	name=${arm%%=*}
	src=$(cd "${arm#*=}" && pwd)
	if [ -z "$first" ]; then
		first=$src/bench/lg.cc
	elif ! cmp -s "$first" "$src/bench/lg.cc"; then
		echo "ab_build: $src/bench/lg.cc differs from $first" >&2
		exit 1
	fi
	cmake -S "$src" -B "$out/build-$name" -DCMAKE_BUILD_TYPE=Release -DLOGGER_BUILD_BENCH=ON >/dev/null
	cmake --build "$out/build-$name" --target logger_bench -j 4 >/dev/null
	cp "$out/build-$name/logger_bench" "$out/$name"
	file "$out/$name"
	git -C "$src" log -1 --format="$name: %h %s"
done
