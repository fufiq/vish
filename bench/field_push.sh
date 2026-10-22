#!/bin/sh
# Measurement of push linearity in the field (test 332): time at N and 4N in each engine; linear
# - a ratio of about 4 (was: the field-argument was copied - about 16).
#   N=20000 bench/field_push.sh
VISH=${VISH:-out/vish}
N=${N:-20000}
for mode in "" --jit --machine; do
  for n in $N $((N * 4)); do
    t=$( { /usr/bin/time -f %e $VISH run $mode tests/332_field_push_linear.vish $n >/dev/null; } 2>&1 | tail -1)
    echo "${mode:-interp} $n: $t с"
  done
done
