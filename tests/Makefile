# All the host tests: make -C tests (clean: make -C tests clean)
DIRS := frame mpm storage

all clean:
	@fail=0; for d in $(DIRS); do $(MAKE) -C $$d $@ || fail=1; done; exit $$fail

.PHONY: all clean
