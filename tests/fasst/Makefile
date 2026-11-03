# Host unit test of src/core/fasst_frame.c against the TM-10's packets as
# the receiver got them (doc/futaba.md "FASST on the air").
#   make -C tests/fasst        build and run it
TESTS = fasst
CODE = $(SRC)/core/fasst_frame.c
DEPS = $(SRC)/core/fasst_frame.h
INC = -I$(SRC)/core -I$(SRC)/hal
include ../rules.mk
