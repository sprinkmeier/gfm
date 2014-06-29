# gfm: Gallois Field Matrix - based encoder

This utility uses Reed-Solomon coding to provide fault tolerant data storage.

This program is based on two papers,
*H. Peter Anvin, 'The mathematics of RAID-6', December 2004*
and
*James S. Plank, [A Tutorial on Reed-Solomon Coding for Fault-Tolerance in
RAID-like Systems](http://www.cs.utk.edu/~plank/plank/papers/CS-96-332.html)*
and
*[Note: Correction to the 1997 Tutorial on Reed-Solomon Coding](http://web.eecs.utk.edu/~plank/plank/papers/CS-03-504.pdf)*.

The idea is to take a stream of data and store it in a number of files
while calculating and adding in parity data.

These files can then be transported by potentially unreliable means.

Provided sufficient files arrive intact the original data stream can
be fully recovered.

split CriticalData into 10 data files with 5 parity files

    $ gfm crit 10 5 < CriticalData

the created files:

    $ ls
    crit00  crit03  crit06  crit09  crit0c  CriticalData
    crit01  crit04  crit07  crit0a  crit0d  crit.md5
    crit02  crit05  crit08  crit0b  crit0e

The first 10 files contain the original data, slightly mangled and
with some housekeeping data added. These are the 'data' files.
The last 5 files contain the calculated parity data (plus housekeeping).
These are the 'parity' files.

In practice 'data' and 'parity' files are interchangeable when it
comes to data recovery.

The md5 file contains the MD5 checksums of the original data
and the created files:

    $ gfm crit | md5sum --check crit.md5
    crit00: OK
    crit01: OK
    crit02: OK
    crit03: OK
    crit04: OK
    crit05: OK
    crit06: OK
    crit07: OK
    crit08: OK
    crit09: OK
    crit0a: OK
    crit0b: OK
    crit0c: OK
    crit0d: OK
    crit0e: OK
    -: OK

Note that the 'data' files, being slightly rearranged versions
of the raw data, have the same 'compressability' as the original
data. The 'parity' files tend not to compress much as they're made
up of all the data files mixed together. Compress before you gfm!

Now assume that only 10 of these files made it:

    $ ls
    crit01  crit04  crit07  crit09  crit0d  CriticalData
    crit02  crit05  crit08  crit0a  crit0e  crit.md5

To recover the data from the remaining files
(checking MD5 checksums as we go):

    $ gfm crit | tee CriticalData.recovered | md5sum --check crit.md5
    md5sum: crit.0: No such file or directory
    crit00: FAILED open or read
    crit01: OK
    crit02: OK
    md5sum: crit.03: No such file or directory
    crit03: FAILED open or read
    crit04: OK
    crit05: OK
    md5sum: crit.06: No such file or directory
    crit06: FAILED open or read
    crit07: OK
    crit08: OK
    crit09: OK
    crit0a: OK
    md5sum: crit0b: No such file or directory
    crit0b: FAILED open or read
    md5sum: crit0c: No such file or directory
    crit0c: FAILED open or read
    crit0d: OK
    crit0e: OK
    -: OK
    md5sum: WARNING: 5 listed files could not be read

And verify:

    $ diff CriticalData CriticalData.recovered && echo OK
    OK

## Notes

Files, if present, are assumed to be correct. Depending on the
transport mechanism it may be advisable to verify this.

The total number of files (data + parity) must be less than or
equal to 250.

The maximum number of files that can be lost without loss of data is
equal to the number of parity files generated.

The above examples are obviously just a start:

1. tar up the ~/bin directory, encrypt it and split it up with parity:

`$ tar --create --file - --directory ~ bin | \`<br/>
&nbsp;&nbsp;`gpg --encrypt --sign --recipient $USER | gfm mailme 5 5`

1. result:

`$ ls`<br/>
`mailme00  mailme02  mailme04  mailme06  mailme08  mailme.md5`<br/>
`mailme01  mailme03  mailme05  mailme07  mailme09`

1. now email the lot:

`$ for X in * ; do uuencode "$X" "$X"`<br/>
&nbsp;&nbsp;`| mail someone@example.com -s "$X" ; done`

1. recover:

`$ gfm mailme |  gpg > bin.tar`

## Self Contained

The utility contains a copy of its own git repository:

    $ gfm - | tar --xz --extract --file -
    $ cd gfm-*
    $ make test

## Viral

What's the use of data without a recovery tool?

Tarballs have no redundancy, but at least everyone has
tar to extract the data.

    $ tar czf - -C /var/cache/apt/archives/ . | gfm deb.tar.gz 5 5
    tar: ./lock: Cannot open: Permission denied
    tar: Error exit delayed from previous errors
    $ ls
    deb.tar.gz00  deb.tar.gz02  deb.tar.gz04  deb.tar.gz06  deb.tar.gz08  deb.tar.gz.md5
    deb.tar.gz01  deb.tar.gz03  deb.tar.gz05  deb.tar.gz07  deb.tar.gz09

OK, so you have a bunch of data files, but no recovery tool. Not so...

Every file is a nested tarball containing the recovery tool:

    $ tar xvf deb.tar.gz05
    gfm.tar.xz
    tar: Skipping to next header
    tar: Exiting with failure status due to previous errors
    $ tar xf gfm.tar.xz

Don't worry about the error, that's just tar complaining about the
extra 'junk' at the end of the 'tarball'

    $ rm deb*
    $ ls
    gfm-$TAG gfm.tar
    $ cd gfm-*
    $ ls
    gfa.hh  gfm.cc  gpl-3.0.txt  LICENSE  Makefile  README.md

This is all you need to recreate the gfm tool, and more:

    $ make
    [....]
    $ ls
    blob.o  gfa.hh  gfm  gfm.cc  gfm.o  git.h  gpl-3.0.txt
    LICENSE  Makefile  README.html  README.md  README.pdf

Note that this is a clone of the GFM repository:

    $ git status
    # On branch master
    nothing to commit (working directory clean)

## Debugging, Diagnosing ...

**gfm** has a built-in-test mode that is activated by setting the
environment variable **BIT**:

    $ BIT=1 gfm
    BIT ...
    BIT OK!
    [..]

There is also a diagnostic mode that dumps details about the
Gallois Fields, Parity and Recovery matrices:

    $ DMP=1 gfm
    $ ls
    gfm.gfa  gfm.gfm

The generated files don't make much sense without reading the papers first.

Once you've recovered the build environment you can run the usual *make check*:

    $ make check

Check the comments in the *Makefile* to see what it does.
If the test passes *make* will exit with the usual return code
of 0 after cleaning up all the files generated for the test.
If it fails the files will be left behind to allow further testing.

There us also a test script that runs a more extensive test:

    $ ./runtest

This runs a far more extensive version of the *make check* test.
