# Convenience only. The gate itself lives in tools/gate.py.
#
# `make check` and `python tools/gate.py` run identical checks. The Python driver
# exists because a C++ compiler is the one dependency we cannot avoid, while
# `make` is avoidable -- and GitHub's Windows and macOS images do not guarantee
# it, so a Makefile-only gate fails on the runner for reasons that have nothing
# to do with the code.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow \
            -Wconversion -Wsign-conversion -Werror
PYTHON   ?= python3

.PHONY: all check check-cpp check-py test layouts clean

all: check

## check: the whole gate
check:
	$(PYTHON) tools/gate.py

## check-cpp: only the firmware logic
check-cpp: $(BIN)
	./$(BIN) firmware/partitions

## check-py: only the flashing tool
check-py:
	$(PYTHON) tests/test_flash_tool.py

test: check

## layouts: regenerate the shipped partition tables from the code's geometry
layouts:
	$(PYTHON) tools/gen_layouts.py

clean:
	rm -rf build
