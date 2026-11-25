# Olive Press integration - 25 November 2026

## Contents and provenance
- Sources provided by team: common.h, main.c, supervisor.c/h, logger.c/h, reception.c, cleaning.c, washing.c, config.c, ipc_util.c, stage_support.c, crushing.c, malaxation.c, pressing.c, p3_stage_util.h.
- Partner 4 sources taken from Julnar_P4_QC_GUI_milestone.zip: quality_control.c, gui.c, tests/qc_gui_driver.c, tests/gui_demo_driver.c.
- The original shared common.h, ipc_util.c and stage_support.c were **not edited**. GUI header changed to `#include <GL/freeglut.h>` to make the previously observed FreeGLUT API errors compile correctly on Ubuntu.
- Custom integration Makefile adds independent headless and QC tests; original Partner 1 Makefile saved as docs/Makefile_from_partner1.txt.
- Tests/gui_headless_stub.c is ONLY for Docker tests that do not have a GUI; it is NOT the actual GUI and is not linked in final build.

## Run on Ubuntu with freeglut
`sudo apt update && sudo apt install -y --no-install-recommends build-essential freeglut3-dev libglu1-mesa-dev`
`make`
`./olive_press config/default.cfg`

The app requires a display server for its Linux GLUT window. To run without one, use `make olive_press_headless` and `./olive_press_headless config/test_fast.cfg`. In a headless session use configs with `enable_gui=0`.

## Tests
`make test` runs QC test driver, one-lot normal pipeline, one-lot fault-injection pipeline. `config/test_fast.cfg` and `config/test_alerts.cfg` override selected defaults for fast tests. Default config is unchanged.

## Local verification (environment without GL dev headers)
- Headless combined binary compiled with GCC 13-like Linux toolchain; one-lot normal end-to-end test exited 0, `SUMMARY ... result=complete`.
- Fault injection end-to-end test exited 0, observed 5 warnings and 1 critical alert, `result=complete`.
- QC 9-case regression test still needs to be run by make test on the user's Ubuntu environment; earlier user run reported 9/9 PASS.
- Full OpenGL application linking and on-screen Linux rendering NOT verified in this environment because FreeGLUT headers/libraries are absent. Previously user successfully displayed GUI on macOS with a platform-specific driver.

## Next tests
- Multi-batch scenario and pause/resume signals in final Ubuntu build.
- Full GUI connected to Supervisor via X server or Ubuntu desktop (macOS native GUI demo does not verify Linux GUI child process).
- Confirm normal completion with default config and chosen `gui_auto_close_ms`.

## Important note
Do not replace the team's frozen common.h or IPC/stage helper files with older local copies. Always merge carefully and communicate any proposed interface changes to Partner 1.
- Three-batch / three-lot headless pipeline: exit 0 and `result=complete` (tested after the first integration passes).
