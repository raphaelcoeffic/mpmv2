# Host unit tests of the MULTI-Module core (src/mpm) with a fake port
#   make -C tests/mpm          build and run all the tests
TESTS = core
# PPM: a protocol of the test table
DEFS = -DMPM_PPM_PROTOCOL=127 -DMPM_PPM_SUB_PROTOCOL=2
CODE = $(SRC)/mpm/mpm.c $(SRC)/mpm/mpm_frame.c $(SRC)/mpm/mpm_settings.c $(SRC)/core/log.c
DEPS = $(wildcard $(SRC)/mpm/*.h)
INC = -I$(SRC) -I$(SRC)/core
include ../rules.mk
