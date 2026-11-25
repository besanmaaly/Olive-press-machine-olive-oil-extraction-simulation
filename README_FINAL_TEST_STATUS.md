# Olive Press — combined candidate / integration status

Created on 25 November 2026. **Not a final verified graphical release.**

## Source selection
- Partner 1: `main.c`, `supervisor.c/.h`, `logger.c/.h`, and Makefile reference (in `docs/Makefile_from_partner1.txt`).
- Partner 2 UPDATED from `partner2_olive_press.zip`: `reception.c`, `cleaning.c`, `washing.c`, `config.c`, associated configs and tests. This was the newly uploaded Partner 2 archive, **not** an updated Partner 3 archive.
- Partner 3: `crushing.c`, `malaxation.c`, `pressing.c`, `p3_stage_util.h` from earlier uploads.
- Partner 4: `quality_control.c`, `gui.c`, QC and GUI drivers from preceding P4 milestone.
- Frozen interfaces: `common.h`, `ipc_util.c`, `stage_support.c` verified byte-identical in newly supplied Partner 2 zip and previous combined source.

## Tests run here (Linux)
- GCC compile: `make olive_press_headless qc_gui_driver` PASS.
- Partner 4 QC test: 9/9 cases PASS.
- Partner 2: `bash tests/run_p2_tests.sh quick` completed output `25 ok, 0 failed`, 21 belt checks, 14 OpenMP checks, and 50 pipeline repetitions.
- Combined headless: `./olive_press_headless config/fast.cfg` exit 0, 16 lots, `result=complete`.
- `./olive_press_headless config/test_alerts.cfg`: see run log; alerts are expected.

## IMPORTANT remaining verification
- Native Linux full OpenGL build `make` requires `freeglut3-dev`, `libglu1-mesa-dev`, and access to a graphical display, and has **not** been run here. The `gui_headless_stub.c` is test-only; it is never linked into `olive_press`.
- GUI wired into the real supervisor, pause/resume/stop signals, sustained multi-batch stress, and shutdown should still be exercised in the target environment.
- Configuration file `config/default.cfg` still has `gui_auto_close_ms = 5000`, which deliberately closes the integrated GUI after completion. Change only in a separate test config if needed.
- Prefer the team's final source tree as the canonical deliverable after everyone agrees to version selection.

## Build and run in Linux
```
sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential freeglut3-dev libglu1-mesa-dev
make
./olive_press config/default.cfg
```

Docker without display:
```
make test
bash tests/run_p2_tests.sh quick
./olive_press_headless config/fast.cfg
```

## macOS vs Linux
macOS GUI demo requires its separate platform-specific FreeGLUT/GLUT adaptation; the final executable targets Linux. Docker on macOS without an X server can compile GUI but cannot display its window directly.
