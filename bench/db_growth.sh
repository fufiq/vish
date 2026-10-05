#!/bin/sh
# Measurement of base growth (test 217): N rounds in tz vm; prints base size after
# each round and .tzdb size at the end.
# N=50 bench/db_growth .sh; FN=tune BODY="..." bench/db_growth .sh - editing another function
TZ=${TZ:-out/tz}
N=${N:-20}
PROG=${PROG:-tests/db_growth.tz}
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
  echo "save out/db_growth.tzdb"
} | $TZ vm $PROG | grep "база:"
ls -l out/db_growth.tzdb | awk '{print "файл", $5}'
