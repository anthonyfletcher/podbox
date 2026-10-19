#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#
# Emits one quoted entry per line for the credits reel. A blank line in
# docs/CREDITS becomes an empty entry, which the reel renders as a one-line
# gap -- that is how sections ("For RockPod", "For Rockbox", ...) are spaced
# apart. Blank lines before the first name are the file's own header padding
# and are dropped, so the reel does not open on empty space. Any other line
# that starts in the first column is a name, whatever its first letter; the
# indented banner is not.
my $seen = 0;
while (<STDIN>) {
    if($_ =~ /^\s*$/) {
	print "\"\",\n" if $seen;
    }
    elsif(($_ =~ /^(\S[\S ]*)/) && ($_ !~ /^People/)) {
	print "\"$1\",\n";
	$seen = 1;
    }
}
