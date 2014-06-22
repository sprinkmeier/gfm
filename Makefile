MACH ?= $(shell uname --machine)

# x86_64
OC_OUT = elf64-x86-64
OC_BIN = i386

ifeq ($(MACH),armv6l)
    OC_OUT = elf32-littlearm
    OC_BIN = arm
endif

MDs  =$(wildcard *.md)
HTMLs=$(MDs:.md=.html)
PDFs =$(HTMLs:.html=.pdf)
DOC  =$(HTMLs) $(PDFs)

CC=$(CXX)

LDLIBS += $(shell pkg-config --libs openssl)
GIT_TAG=gfm-$(shell git describe --tags --dirty --long)

CXXFLAGS += -Wall -Wextra -Werror
CXXFLAGS += -O3
#CXXFLAGS += -g

default: gfm doc

doc: $(DOC)

clean:
	-rm *.o git.h foo*
	-rm -rf .deps
	-rm $(DOC)

cleaner: clean
	-rm gfm

remake: cleaner
	$(MAKE)

%.html: %.md
	markdown $^ > $@

%.pdf: %.html
	html2ps < $^ | ps2pdf - > $@

blob.o::
	git gc --aggressive
	git clone . ${GIT_TAG}
	git diff > $(GIT_TAG).diff
	tar --create --file - --remove-files \
		$(GIT_TAG).diff ${GIT_TAG} \
		| xz > gfm.tar.xz
	tar --format=v7 \
		--create --file gfm.tar gfm.tar.xz
	objcopy \
		--input binary \
		--output $(OC_OUT) \
		--binary-architecture $(OC_BIN) \
		gfm.tar $@
	rm gfm.tar gfm.tar.xz

git.h::
	echo '#define GIT_TAG "$(GIT_TAG)"' > $@

gfm.o: git.h

gfm: gfm.o  blob.o

%.o: %.cc
	test -d .deps || mkdir .deps
	$(COMPILE.cc) -MMD $(OUTPUT_OPTION) $<
	@mv  $*.d  .deps/$*.d
	@echo $@: Makefile >> .deps/$*.d


test: gfm doc
	# contains tarball?
	./gfm - | tar --xz --test --file -
	# generate parity
	./gfm foo 10 10 < gfm
	# parity files are tarballs?
	tar --test --verbose --file foo00
	# recover and verify checksum
	./gfm foo | tee foo | md5sum --check foo.md5
	# randomly remove files
	rm $$(ls foo0* | shuf | head)
	# recover again and verify
	./gfm foo > foo_
	cmp foo foo_
	# clean up
	rm foo*

.PHONY: default clean cleaner remake doc test

-include .deps/*.d
