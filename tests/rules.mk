# What the host test Makefiles share: TESTS built from <test>.c with CODE
# (the sources under test) in build/, then run (make: all of them, the log
# of the failed ones shown). The Makefile sets TESTS, CODE, and DEFS, INC and
# DEPS (more prerequisites) when it needs them, before including this file
CC ?= cc
CFLAGS ?= -O1 -Wall -Wno-unused-function
SRC := ../../src
BUILD := build

all: $(TESTS:%=$(BUILD)/%)
	@fail=0; for t in $^; do echo "== $$t"; ./$$t > $$t.log 2>&1 || { cat $$t.log; fail=1; }; tail -1 $$t.log; done; exit $$fail

$(BUILD)/%: %.c $(CODE) $(DEPS) ../platform.h ../test.h | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(INC) -I.. -o $@ $< $(CODE)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
