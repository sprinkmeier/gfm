CC=$(CXX)

LDLIBS += $(shell pkg-config --libs openssl)
GIT_TAG=$(shell git describe --tags --dirty --long)

default: gfm

clean:
	-rm *.o gfm
	-rm -rf .deps

remake: clean
	$(MAKE)

blob.o::
	git archive --prefix $(GIT_TAG)/ --format tar HEAD > $(GIT_TAG).tar
	git diff > $(GIT_TAG).diff
	tar --create --file - --remove-files \
		$(GIT_TAG).tar $(GIT_TAG).diff \
		| xz > $*.bin
	objcopy \
		--input binary \
		--output elf64-x86-64 \
		--binary-architecture i386 \
		$*.bin $@
	rm $*.bin

gfm: gfm.o blob.o

%.o: %.cc
	test -d .deps || mkdir .deps
	$(COMPILE.cc) -MMD $(OUTPUT_OPTION) $<
	@sed -e 's|.*:|$*.o:|' < $*.d > .deps/$*.d
	@sed -e 's/.*://' -e 's/\\$$//' < $*.d | fmt -1 | \
	  sed -e 's/^ *//' -e 's/$$/:/' >> .deps/$*.d
	@echo $@: Makefile >> .deps/$*.d
	@rm -f $*.d

.PHONY: default clean remake

-include .deps/*.d
