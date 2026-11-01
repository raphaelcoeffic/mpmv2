# Host unit tests of src/core/frame.c: framings against the MULTI-Module code and
# against packets captured from its chips.
#   make -C tests/frame        build and run all the tests
TESTS = xn297_mpm sync_prefix esb_capture nrf_v2x2 cc2500 hott a7105 cyrf_dsm cyrf_dsm_bind \
        cyrf_devo cyrf_kyosho3 hs6200_rf2500 frskyl var_len
CODE = $(SRC)/core/frame.c
DEPS = $(SRC)/core/frame.h $(wildcard mpm/*.inc)
INC = -I$(SRC)/core
include ../rules.mk
