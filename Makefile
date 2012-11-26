CC=$(CXX)

LDLIBS += $(shell pkg-config --libs openssl)

GIT_TAG=$(shell git describe --tags --dirty --long)

#EXPORT=~/bin/bzr_export.pl
EXPORT=./bzr_export.pl

default: gfm

clean:
	-rm *.o gfm

remake: clean default

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

.PHONY: default clean remake
