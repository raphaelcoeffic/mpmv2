# Host unit test of src/core/tfhss.c: the hop rows against the R334SBS's
# table, packets and CRC syndromes against the bench's Python reference.
#   make -C tests/tfhss        build and run it
TESTS = tfhss
CODE = $(SRC)/core/tfhss.c
DEPS = $(SRC)/core/tfhss.h
INC = -I$(SRC)/core
include ../rules.mk
