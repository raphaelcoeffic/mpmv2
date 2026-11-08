# Host tests of the EEPROM file without a file system (file_server_ram.c on
# the RAM EEPROM of the LaunchPads and the ZBDongle-P, core/eeprom_ram.c)
#   make -C tests/eeprom_ram   build and run the tests
TESTS = eeprom_ram
CODE = $(SRC)/core/file_server_ram.c $(SRC)/core/eeprom_ram.c $(SRC)/core/log.c
DEPS = $(SRC)/core/file_server.h $(SRC)/core/eeprom.h $(SRC)/core/log.h
INC = -I$(SRC)/core
include ../rules.mk
