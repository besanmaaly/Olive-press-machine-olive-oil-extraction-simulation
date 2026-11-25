#!/bin/bash
# tests/run_p2_tests.sh -- builds and runs every Partner 2 test. Run from the project root:
#     bash tests/run_p2_tests.sh            (add "quick" for a shorter stress phase)
# All code is compiled with -Wall -Wextra -Werror -g, so any warning stops the script.
cd "$(dirname "$0")/.." || exit 2
CF="-std=gnu11 -Wall -Wextra -Werror -g -fopenmp -Iinclude"
SH="src/ipc_util.c src/stage_support.c"
B=build_p2; mkdir -p $B
pass=0; fail=0
ok()  { echo "  [OK]   $1"; pass=$((pass+1)); }
bad() { echo "  [FAIL] $1"; fail=$((fail+1)); }
expect_rc() { # expect_rc <expected> <name> cmd...
  exp=$1; name=$2; shift 2; "$@" >/dev/null 2>&1; rc=$?
  [ "$rc" = "$exp" ] && ok "$name (exit $rc)" || bad "$name (exit $rc, expected $exp)"; }

echo "== build (zero warnings required)"
gcc $CF tests/config_test.c src/config.c -lm -o $B/config_test || exit 1
gcc $CF -O2 tests/cleaning_belt_test.c $SH src/cleaning.c -lm -o $B/cleaning_belt_test || exit 1
gcc $CF -O2 tests/omp_compare_test.c $SH src/cleaning.c src/washing.c -lm -o $B/omp_compare_test || exit 1
gcc $CF tests/pipeline_p2_test.c src/config.c $SH src/reception.c src/cleaning.c src/washing.c -lm -o $B/pipeline_p2_test || exit 1
gcc $CF -DREAL_CLEANING -DREAL_WASHING -DREAL_RECEPTION tests/stage_harness.c tests/mock_stages.c $SH \
    src/reception.c src/cleaning.c src/washing.c -lm -o $B/stage_harness || exit 1
ok "all P2 sources and tests compile without warnings"

echo "== 1. config parser"
expect_rc 0 "default.cfg equals built-in defaults" $B/config_test config/default.cfg --check-defaults
for f in fast no_faults stress; do expect_rc 0 "$f.cfg loads" $B/config_test config/$f.cfg; done
expect_rc 0 "bad_unknown.cfg -> warning only" $B/config_test config/bad_unknown.cfg
for f in bad_syntax bad_range bad_number bad_value bad_missing; do expect_rc 1 "$f.cfg -> error" $B/config_test config/$f.cfg; done
expect_rc 1 "missing file -> error" $B/config_test /nonexistent.cfg
$B/config_test config/bad_range.cfg 2>&1 | grep -q "bad_range.cfg:4:" && ok "error message has file:line" || bad "error message has file:line"
$B/config_test config/bad_unknown.cfg 2>&1 | grep -q "unknown key 'bogus_key' ignored" && ok "unknown key warning text" || bad "unknown key warning text"
$B/config_test config/bad_unknown.cfg 2>/dev/null | grep -q "^batch_mass_kg = 300 300" && ok "single number gives min = max" || bad "single number gives min = max"

echo "== 2. reception (harness, dummy cleaning)"
$B/stage_harness reception 0 100 > $B/rec.out 2>&1
runs=$(grep -c "BELT RUN" $B/rec.out); stops=$(grep -c "BELT STOP" $B/rec.out)
[ "$runs" = 3 ] && [ "$stops" = 2 ] && ok "3 batches: RUN x3, STOP x2, no STOP after last" || bad "belt signals RUN=$runs STOP=$stops"
grep -q "dummy-cleaning\] received" $B/rec.out && ok "output pipe closed at the end (EOF seen)" || bad "EOF"

echo "== 3. cleaning <-> belt (timing test, 21 checks)"
$B/cleaning_belt_test > $B/belt.out 2>&1; rc=$?; tail -1 $B/belt.out
[ $rc = 0 ] && ok "cleaning_belt_test" || { bad "cleaning_belt_test"; grep FAIL $B/belt.out; }

echo "== 4. washing (harness)"
$B/stage_harness washing 8 50 | tail -1 | grep -q "8 lots out" && ok "washing forwards all lots" || bad "washing harness"

echo "== 5. OpenMP on/off + physics"
$B/omp_compare_test 60 2000000 > $B/omp.out 2>&1; rc=$?; grep -E "TIME|CPUs|==" $B/omp.out
[ $rc = 0 ] && ok "omp_compare_test" || { bad "omp_compare_test"; grep FAIL $B/omp.out; }

echo "== 6. pipeline reception -> cleaning -> washing"
N=${1:+10}; N=${N:-40}
for args in "config/fast.cfg" "config/default.cfg 100 1 0" "config/default.cfg 100 1 1" "config/default.cfg 1000 1 1" "config/default.cfg 5 200 0 300"; do
  f=0; for i in $(seq 1 $N); do timeout 100 $B/pipeline_p2_test $args >/dev/null 2>&1 || f=$((f+1)); done
  [ $f = 0 ] && ok "pipeline '$args' x$N" || bad "pipeline '$args': $f of $N runs failed"
done

echo "== summary: $pass ok, $fail failed"
[ $fail = 0 ]
