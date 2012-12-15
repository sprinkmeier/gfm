CC=$(CXX)

LDLIBS += $(shell pkg-config --libs openssl)
GIT_TAG=gfm-$(shell git describe --tags --dirty --long)

CXXFLAGS += -Wall -Wextra -Werror
CXXFLAGS += -O3

default: gfm

clean:
	-rm *.o git.h
	-rm -rf .deps

cleaner: clean
	-rm gfm

remake: cleaner
	$(MAKE)

blob.o::
	git gc --aggressive
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

git.h::
	echo '#define GIT_TAG "$(GIT_TAG)"' > $@

gfm.o: git.h

gfm: gfm.o  blob.o

%.o: %.cc
	test -d .deps || mkdir .deps
	$(COMPILE.cc) -MMD $(OUTPUT_OPTION) $<
	@mv  $*.d  .deps/$*.d
	@echo $@: Makefile >> .deps/$*.d

.PHONY: default clean cleaner remake

-include .deps/*.d
