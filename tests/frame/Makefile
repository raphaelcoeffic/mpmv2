# Host unit tests of src/frame.c: framings against the MULTI-Module code and
# against packets captured from its chips.
#   make -C tests/frame        build and run all the tests
CC ?= cc
CFLAGS ?= -O1 -Wall -Wno-unused-function
SRC := ../../src
BUILD := build
TESTS := xn297_mpm sync_prefix esb_capture nrf_v2x2 cc2500 hott a7105 cyrf_dsm cyrf_dsm_bind \
         cyrf_devo cyrf_kyosho3 hs6200_rf2500 frskyl var_len

all: $(TESTS:%=$(BUILD)/%)
	@fail=0; for t in $^; do echo "== $$t"; ./$$t > $$t.log 2>&1 || { cat $$t.log; fail=1; }; tail -1 $$t.log; done; exit $$fail

$(BUILD)/%: %.c $(SRC)/frame.c $(SRC)/frame.h ../platform.h $(wildcard mpm/*.inc) | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC) -I.. -o $@ $< $(SRC)/frame.c

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
