#!/bin/sh
# Measurement of base growth (test 217): N rounds in vish vm; prints base size after
# each round and .vishdb size at the end.
# N=50 bench/db_growth .sh; FN=tune BODY="..." bench/db_growth .sh - editing another function
VISH=${VISH:-out/vish}
N=${N:-20}
PROG=${PROG:-tests/db_growth.vish}
FN=${FN:-other}
BODY=${BODY:-"other() => 7;"}
{
  echo stats
  i=0
  while [ $i -lt $N ]; do
    printf 'edit %s\n%s\n.\n' "$FN" "$BODY"
    echo check
    echo run
    echo jit
    echo stats
    i=$((i+1))
  done
  echo "save out/db_growth.vishdb"
} | $VISH vm $PROG | grep "база:"
ls -l out/db_growth.vishdb | awk '{print "файл", $5}'
