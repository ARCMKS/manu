CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
LDFLAGS ?=

FFI_CFLAGS := $(shell pkg-config --cflags libffi 2>/dev/null)
FFI_LIBS   := $(shell pkg-config --libs libffi 2>/dev/null)
ifeq ($(strip $(FFI_LIBS)),)
  FFI_LIBS := -lffi
endif

CORE_SRC  := $(filter-out src/main.c, $(wildcard src/*.c))
MAIN_SRC  := src/main.c
TEST_SRC  := tests/test.c $(wildcard tests/t_*.c)
TEST_MAIN := tests/test_main.c

CFLAGS_ALL := $(CFLAGS) $(FFI_CFLAGS) -DPRISM_FFI_AVAILABLE -Iinclude

.PHONY: all build test run clean debug

all: manu.exe tests.exe

build: clean manu.exe

manu.exe: $(CORE_SRC) $(MAIN_SRC)
	$(CC) $(CFLAGS_ALL) -o $@ $^ $(FFI_LIBS) $(LDFLAGS)

tests.exe: $(CORE_SRC) $(TEST_SRC) $(TEST_MAIN)
	$(CC) $(CFLAGS_ALL) -o $@ $^ $(FFI_LIBS) $(LDFLAGS)

test: tests.exe
	./tests.exe $(FILTER)

test-%: tests.exe
	./tests.exe $*

run: manu.exe
	./manu.exe

clean:
	@cmd /c "if exist manu.exe del /Q manu.exe"
	@cmd /c "if exist tests.exe del /Q tests.exe"

debug: CFLAGS  = -std=c11 -O1 -g -fsanitize=address -fno-omit-frame-pointer -Wall -Wextra
debug: LDFLAGS = -fsanitize=address
debug: clean all