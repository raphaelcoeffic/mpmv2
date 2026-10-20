# Host tests of the file system and the EEPROM file on a simulated NOR flash
#   make -C tests/storage          build and run the tests
CC ?= cc
CFLAGS ?= -O1 -Wall -Wno-unused-function
SRC := ../../src
LFS := ../../lib/littlefs
BUILD := build
TESTS := storage
DEFS := -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
CODE := $(SRC)/lfs_driver.c $(SRC)/eeprom.c $(SRC)/file_server.c $(LFS)/lfs.c $(LFS)/lfs_util.c

all: $(TESTS:%=$(BUILD)/%)
	@fail=0; for t in $^; do echo "== $$t"; ./$$t > $$t.log 2>&1 || { cat $$t.log; fail=1; }; tail -1 $$t.log; done; exit $$fail

$(BUILD)/%: %.c $(CODE) $(wildcard $(SRC)/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) -I$(SRC) -I$(LFS) -o $@ $< $(CODE)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
