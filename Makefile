CC = gcc
CFLAGS = -std=gnu11 -Wall -Wextra -g -fopenmp -Iinclude -Isrc
SHARED = src/ipc_util.c src/stage_support.c
CORE = src/main.c src/supervisor.c src/logger.c src/config.c \
       src/reception.c src/cleaning.c src/washing.c \
       src/crushing.c src/malaxation.c src/pressing.c src/quality_control.c

.PHONY: all clean test headless
all: olive_press

# Final Linux/OpenGL binary, requires freeglut development libraries.
olive_press: $(SHARED) $(CORE) src/gui.c
	$(CC) $(CFLAGS) -o $@ $^ -lm -lglut -lGLU -lGL

# Headless binary for Linux CI/Docker without an X server.
olive_press_headless: $(SHARED) $(CORE) tests/gui_headless_stub.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

qc_gui_driver: $(SHARED) src/quality_control.c tests/qc_gui_driver.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

gui_demo_driver: $(SHARED) src/gui.c tests/gui_demo_driver.c
	$(CC) $(CFLAGS) -o $@ $^ -lm -lglut -lGLU -lGL

test: olive_press_headless qc_gui_driver
	./qc_gui_driver qc
	./olive_press_headless config/test_fast.cfg
	./olive_press_headless config/test_alerts.cfg
	./olive_press_headless config/test_three_lots.cfg

clean:
	rm -f olive_press olive_press_headless qc_gui_driver gui_demo_driver *.log
