# Host unit tests of the MULTI-Module core (src/mpm) with a fake port
#   make -C tests/mpm          build and run all the tests
CC ?= cc
CFLAGS ?= -O1 -Wall -Wno-unused-function
SRC := ../../src
BUILD := build
TESTS := core
CORE := $(SRC)/mpm/mpm.c $(SRC)/mpm/mpm_frame.c

all: $(TESTS:%=$(BUILD)/%)
	@fail=0; for t in $^; do echo "== $$t"; ./$$t > $$t.log 2>&1 || { cat $$t.log; fail=1; }; tail -1 $$t.log; done; exit $$fail

$(BUILD)/%: %.c $(CORE) $(wildcard $(SRC)/mpm/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $< $(CORE)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
