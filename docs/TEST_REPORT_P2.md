# P2 test results (for docs/TEST_REPORT.md)

Run everything with `bash tests/run_p2_tests.sh` (builds with `-Wall -Wextra -Werror -g -fopenmp`; raw output in `docs/p2_test_output.txt`). Result: **25 ok, 0 failed**, plus 21 checks inside `cleaning_belt_test` and 14 inside `omp_compare_test`.

| Area | Test | Result |
|---|---|---|
| Parser | `config_test default.cfg --check-defaults` | MATCH (identical to `config_set_defaults`) |
| Parser | `bad_syntax/range/number/value/missing.cfg` | error `file:line: message`, exit 1, no crash |
| Parser | `bad_unknown.cfg` | warning only, exit 0; single number gives min = max |
| Parser | CRLF, no final newline, 3 values in a range, negative/huge seed, NaN, 600-char line, 300-char path, out-of-range percentages, window < 200 | handled as specified (see `docs/p2_test_output.txt` / edge cases) |
| Reception | harness + dummy cleaning | RUN x3, STOP x2 (none after last batch), EOF at the end |
| Cleaning | `cleaning_belt_test` T1-T8 | stops mid-lot, holds lots while stopped, wait time excluded from QC time, SIGTERM never hangs, signal storms lose nothing |
| Washing | harness + `omp_compare_test` | mass unchanged, impurity removal inside configured range |
| OpenMP | `omp_compare_test` | on/off and 2/4 threads give **bit-identical** lots (fixed-point integer reduction) |
| Pipeline | `pipeline_p2_test`, 200 runs incl. `time_scale=1000, tick=1 ms, interval 0` | no lost lot, no hang, no zombie, belt events correct |
| valgrind | `--trace-children=yes --leak-check=full` | 0 invalid accesses, 0 definite leaks (only libgomp worker-thread stacks "possibly lost" because stages leave with `_exit`) |
| gdb | deliberate NULL write in a copy of `config.c` | `SIGSEGV` located at `apply_value` line 226, file line 14, value "2" via `bt` |

## Bugs found by the tests and fixed
1. **Hang after ~1 in 10 runs (zero batch interval, fast tick).** STOP and RUN pending together: the kernel delivers SIGUSR1 first, but SIGUSR2's handler runs nested before SIGUSR1's body, so the final value was "stopped". Fix: handlers installed with `sa_mask = {SIGUSR1, SIGUSR2}` (`install_belt_handler` in `cleaning.c`). Test `T7` fails 3/3 before the fix, passes after.
2. **OpenMP worker threads could swallow signals** (process-directed signals may go to any thread). Fix: `SIGTERM/USR1/USR2` blocked around the parallel region, so workers inherit a blocked mask.
3. **Unprocessed lot forwarded on SIGTERM** (`stage_run` always forwards): cleaning now always applies its (instant) physics before returning.

## OpenMP better or worse?
Measured on a **1-CPU** machine: 0.27-0.38 s with and without OpenMP (about x1.0), as expected: no speed-up is possible with one core. Re-run `./omp_compare_test 60 2000000` on a multi-core PC and put those numbers here. With the default `sim_elements_per_lot = 500000` the per-lot work is only a few ms, so thread start-up cost can eat the gain; this is the honest discussion point.
