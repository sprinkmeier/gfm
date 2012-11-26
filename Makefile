.PHONY: default clean remake

#EXPORT=~/bin/bzr_export.pl
EXPORT=./bzr_export.pl

default: gfm

clean:
	-rm tmp tmp.h gfm

remake: clean default

tmp.h: tmp
	echo 'unsigned char tar[] = {};' > tmp.h
	./tmp tmp.h

tmp: gfm.cc gfa.hh
	if [ -x $(EXPORT) ] ; then $(EXPORT) --verbose --output tmp ; else ln --symbolic --force `which touch` tmp ; fi

gfm: gfm.cc gfa.hh tmp.h
	$(CXX) $(CXXFLAGS) -lssl -Wall -Werror -DREAL_TAR_ARRY $^ -o $@
