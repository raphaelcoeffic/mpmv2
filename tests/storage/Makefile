# Host tests of the file system and the EEPROM file on a simulated NOR flash
#   make -C tests/storage          build and run the tests
LFS = ../../lib/littlefs
TESTS = storage
DEFS = -DLFS_NO_MALLOC -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR
CODE = $(SRC)/core/lfs_driver.c $(SRC)/core/eeprom.c $(SRC)/core/file_server.c \
	$(SRC)/core/settings.c $(SRC)/mpm/mpm_settings.c $(SRC)/core/log.c $(LFS)/lfs.c \
	$(LFS)/lfs_util.c
DEPS = $(wildcard $(SRC)/core/*.h $(SRC)/hal/*.h $(SRC)/drivers/*.h)
INC = -I$(SRC) -I$(SRC)/core -I$(SRC)/hal -I$(SRC)/drivers -I$(LFS)
include ../rules.mk
