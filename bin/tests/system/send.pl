#!/usr/bin/perl
#
# Copyright (C) Internet Systems Consortium, Inc. ("ISC")
# Copyright (C) Internet Software Consortium.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

# $Id: send.pl,v 1.7 2011/03/05 23:52:29 tbox Exp $

#
# Send a file to a given address and port using TCP.  Used for
# configuring the test server in ans.pl.
#

use IO::File;
use IO::Socket;

@ARGV == 2 or die "usage: send.pl host port [file ...]\n";

my $host = shift @ARGV;
my $port = shift @ARGV;

my $sock = IO::Socket::INET->new(PeerAddr => $host, PeerPort => $port,
				 Proto => "tcp",) or die "$!";
while (<>) {
	$sock->syswrite($_, length $_);
}

$sock->close;
