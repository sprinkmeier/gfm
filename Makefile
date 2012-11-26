CC=$(CXX)

LDLIBS += $(shell pkg-config --libs openssl)
GIT_TAG=gfm-$(shell git describe --tags --dirty --long)
CXXFLAGS+=-DGIT_TAG='"$(GIT_TAG)"'

default: gfm

clean:
	-rm *.o gfm
	-rm -rf .deps

remake: clean
	$(MAKE)

blob.o::
	git clone . ${GIT_TAG}
	git diff > $(GIT_TAG).diff
	tar --create --file - --remove-files \
		$(GIT_TAG).diff ${GIT_TAG} \
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
