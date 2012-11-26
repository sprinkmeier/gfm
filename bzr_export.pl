#! /usr/bin/perl -w
    eval 'exec /usr/bin/perl -S $0 ${1+"$@"}'
        if 0; #$running_under_some_shell

use strict;
use warnings;
use Carp;
use Data::Dumper;
use Getopt::Long;
# dumper, getopt

sub getInfo($);
sub trim($);
sub runOrDie(@);

my $target   = ".";
my $revision;
my $output;
my $verbose  = 0;
my $result = GetOptions ("revision=i" => \$revision,
			 "output=s"   => \$output,
			 "target=s"   => \$target,
			 "verbose+"   => \$verbose);

my @info = getInfo($target);

if($verbose){ print Dumper('info:', \@info);}

$revision = $revision || $info[0];

#(my $rootPath) = ($info{'Repository Root'} =~ /file:\/\/(.*)/);
#(my $root)   = ($info{'Repository Root'} =~ /.*\/(.*)/);
chomp(my $rootPath = `cd "$target" ; pwd`);
my ($root) = ($rootPath =~ m,.*/(.*),);
my $stub     = "$root-$revision";
chomp(my $tmp=`mktemp -t -d "$stub.XXXXXXXXXX"`);
my $tarball  = "$tmp/${stub}.tar.bz2";
my $dump     = "$tmp/${stub}_tar_bz2";

print("Dumping revision $revision to $tmp\n");

runOrDie('bzr',
	'export',
	'--revision', 
		$revision,
	"$tmp/$stub");


if (0 and defined($rootPath))
{
    print "$rootPath\n";

    runOrDie('sh', 
	     '-c', 
	     "svnadmin dump \"$rootPath\" > \"$tmp/$stub/$root.svndump\"");
}

runOrDie('tar',
	'--create',
	'--bzip2',
	'--file', 
		$tarball, 
	'--directory', 
		$tmp, 
	$stub);

open(my $tar, '<', $tarball ) or die "unable to open tarball: $!";
open(my $src, '>', "$dump.c") or die "unable to open dump.c : $!";
open(my $hdr, '>', "$dump.h") or die "unable to open dump.h : $!";

print $src '#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "' . "${stub}_tar_bz2.h" . '"

void writeInfo(FILE * f, int h)
{
    unsigned i;
    const char * fmt = h ? "\t\"%s\",\n" : "%s\n";
    for (i = 0; i < sizeof(info)/sizeof(*info); i++)
    {
	fprintf(f, fmt, info[i]);
    }
}

void writeHeader(FILE * f)
{
    unsigned i, n = sizeof(tar)/sizeof(*tar);
    fprintf(f,"%s\nconst char * info[%zu] = {\n", HEADER_HEAD, sizeof(info)/sizeof(*info));
    writeInfo(f, 1);
    fprintf(f,"};\n\n\nunsigned char tar[%u] = {", n);
    for (i = 0; i < n; i++)
    {
	fprintf(f, "%s0x%02x,", ((i % 16) ? "" : "\n\t"), (tar[i] & 0xFF));
    }
    fprintf(f,"\n};\n#endif\n");
}

int main(int argc, char ** argv)
{
    if (argc != 2)
    {
	printf("%s [-[t|h]|file.info|file.tar.bz2|file.h]\n", argv[0]);
	exit(1);
    }
    FILE * f;
    // info, tar, h
    int mode = 0;
    char * arg = argv[1];
    if (!strcmp(arg, "-"))
    {
	f = stdout;
    }
    else if (!strcmp(arg, "-t"))
    {
	f = stdout;
	mode = 1;
    }
    else if (!strcmp(arg, "-h"))
    {
	f = stdout;
	mode = 2;
    }
    else
    {
	f = fopen(arg, "w");
	assert(f);
	size_t len = strlen(arg);
	if ((len > 8) && !strcmp(arg+len-8, ".tar.bz2"))
	{
	    mode = 1;
	}
	else if  ((len > 2) && !strcmp(arg+len-2, ".h"))
	{
	    mode = 2;
	}
    }
    switch (mode)
    {
	case 0:
	writeInfo(f, 0);
	break;
	case 1:
	{
	    size_t rc = fwrite(tar, 1, sizeof(tar), f);
	    assert(rc == sizeof(tar));
	} break;
      default:
	writeHeader(f);
    }
	
    fclose(f);

    return 0;
}
';

my $def = "_INCL_${stub}_";
$def =~ tr/a-zA-Z0-9/_/c;
print $hdr "#define HEADER_HEAD \"#ifndef $def\\n#define $def\\n\\n#define REVISION $revision\"\n\n";
print $hdr 'char * info[] = {';
foreach my $info (@info)
{
    $info =~ tr/a-zA-Z0-9:\/\.\(\)\-+,%@ //cd;
    print $hdr "\n\t\"$info\",";
}
print $hdr '};

unsigned char tar[] = {';

	my $cnt = 0;
	while(sysread($tar,my $c,1))
	{
		printf $hdr "%s0x%02x", ($cnt ? ($cnt % 16 ? "," : ",\n\t") : "\n\t"), ord($c);
		$cnt++;
	}


print $hdr "};\n/*end $def */\n";

close($tar);
close($src);
close($hdr);

$output = $output || $stub;
runOrDie('gcc', '-Wall', '-Werror', $dump . '.c', '-o', $output);
unless ($output =~ /^.?.?\//)
{
    $output = './' . $output;
}
runOrDie($output, $output . '.tar.bz2');
exit(0);

sub getInfo($)
{
	my $targ = shift;
	chomp(my @info=`bzr revno "$targ" ; bzr info "$targ"`);
	if($verbose) {print Dumper(\@info)};

	return @info;
}


sub runOrDie(@)
{
	my @args = @_;
	if($verbose) {print Dumper('exec:', \@args)};
	system(@args) == 0 or die "system @args failed: $?";
}

sub trim($)
{
	my $str = shift;
	$str =~ s/^\s+//;
	$str =~ s/\s+$//;
	return $str;
}
