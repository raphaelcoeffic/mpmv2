# Host tests of the file system and the EEPROM file on a simulated NOR flash
#   make -C tests/storage          build and run the tests
CC ?= cc
CFLAGS ?= -O1 -Wall -Wno-unused-function
SRC := ../../src
LFS := ../../lib/littlefs
BUILD := build
TESTS := storage
DEFS := -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
CODE := $(SRC)/core/lfs_driver.c $(SRC)/core/eeprom.c $(SRC)/core/file_server.c \
	$(SRC)/core/settings.c $(SRC)/mpm/mpm_settings.c $(SRC)/core/log.c $(LFS)/lfs.c \
	$(LFS)/lfs_util.c
INC := -I$(SRC) -I$(SRC)/core -I$(SRC)/hal -I$(SRC)/drivers -I.. -I$(LFS)

all: $(TESTS:%=$(BUILD)/%)
	@fail=0; for t in $^; do echo "== $$t"; ./$$t > $$t.log 2>&1 || { cat $$t.log; fail=1; }; tail -1 $$t.log; done; exit $$fail

$(BUILD)/%: %.c $(CODE) $(wildcard $(SRC)/core/*.h $(SRC)/hal/*.h $(SRC)/drivers/*.h) \
		../platform.h | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(INC) -o $@ $< $(CODE)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
