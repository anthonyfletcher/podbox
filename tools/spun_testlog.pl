#!/usr/bin/perl
#
# spun_testlog.pl -- synthesise a playback-log family for testing Spun.
#
# The development device has no listening history, and the parser's behaviour
# depends on cases that take months to accumulate naturally: rotation
# boundaries, a calendar-year change, a 24-hour week, plays logged against an
# unset clock. This produces all of them on demand.
#
# It writes two things:
#
#   playback_0001.log ... playback_NNNN.log, playback.log
#       The family, in the order the reader streams it: numbered logs oldest
#       first, live log last (see lrd_open_playback / lrd_advance).
#
#   spun_expected.txt
#       What a correct parse must produce. This is the point of the tool --
#       it turns "the numbers look about right" into pass or fail.
#
# The expectations are derived by running a faithful Perl port of the parser's
# own rules (path_to_meta, the listened/skip test, the truncation to
# NAME_MAX_LEN) over the entries as they are emitted -- not by asserting what
# the generator meant. A bug in the naming rules below shows up as a
# disagreement, which is what we want it to do.
#
# IT PLAYS REAL FILES. The track list comes from the device, so every logged
# path names a file the tag database actually knows. That is the whole point:
# an invented library would resolve through filename guesswork every time and
# would never once exercise the database join -- or the artwork cache, which
# keys off the very same strings (see aa_dirname() in metadata/art_cache.c).
# The join failing is silent and total, so it has to be something the test
# data can catch.
#
# Build the list from a mounted device:
#
#   cd /d && find Music -type f \( -iname '*.mp3' -o -iname '*.flac' ... \) \
#     | sed 's|^|/<HDD0>/|' | sort > tracks.txt
#
# The /<HDD0>/ prefix is not decoration. Both targets define HAVE_MULTIVOLUME,
# so runtime paths carry a volume prefix and id3->path keeps it -- check
# .rockbox/.playlist_control on the device for the form it really uses.
#
# BECAUSE the files are real, two of the expected figures are predictions
# rather than certainties, and the difference between them is the test:
#
#   with a working database, artists and albums come from the tags
#   without one, they come from path_to_meta -- which, for a library laid out
#   as <artist>/<album>/<NN Title>.ext, reports the ALBUM folder as the artist
#
# Both are printed. Which one the device shows tells you whether the join
# works. Every other figure is name-independent and must match exactly.
#
# Usage:  perl tools/spun_testlog.pl --tracks FILE [--out DIR]
#         default DIR is ./spun_testlog
#
# Copy the resulting *.log files to <device>:\.rockbox\ by hand. They cannot
# be shipped in the zip: build-rb.cmd excludes playback*.log from robocopy,
# and /XF is symmetric -- excluded files are not copied from the zip either.

use strict;
use warnings;

my $OUT = 'spun_testlog';
my $TRACKFILE;
my $SCROB = 0;
for (my $i = 0; $i < @ARGV; $i++) {
    if    ($ARGV[$i] eq '--out'    && $i + 1 < @ARGV) { $OUT = $ARGV[++$i]; }
    elsif ($ARGV[$i] eq '--tracks' && $i + 1 < @ARGV) { $TRACKFILE = $ARGV[++$i]; }
    elsif ($ARGV[$i] eq '--scrobbler') { $SCROB = 1; }
    else { die "usage: $0 --tracks FILE [--out DIR] [--scrobbler]\n"; }
}
die "usage: $0 --tracks FILE [--out DIR] [--scrobbler]\n" unless defined $TRACKFILE;

# Matches PLAYBACK_LOG_MAX_FILESZ_BYTES in apps-ipod/audio/playback.c. The
# real rotation happens at boot, when the file is ALREADY over the cap, so
# files end up slightly larger than this -- mirrored below.
my $ROTATE_AT   = 511 * 1024;
my $MIN_VALID_TS = 1104537600;      # 2005-01-01 UTC; below this = unset RTC
my $NAME_MAX    = 39;               # NAME_MAX_LEN - 1, the stored key length

# ---------------------------------------------------------------- randomness

# Own LCG rather than Perl's rand: srand/rand are not guaranteed stable across
# Perl versions or platforms, and the whole value of this tool is that the same
# invocation produces the same bytes.
my $SEED_INITIAL = 20260807;
my $SEED = $SEED_INITIAL;
sub rnd { $SEED = ($SEED * 1103515245 + 12345) & 0x7FFFFFFF; return $SEED; }
sub pick { my ($n) = @_; return rnd() % $n; }
sub between { my ($lo, $hi) = @_; return $lo + pick($hi - $lo + 1); }

# ------------------------------------------------------------------ calendar

# Ported from wrapped_core.h's days_from_civil (Howard Hinnant's algorithm),
# so day arithmetic here and in the parser cannot drift apart.
sub days_from_civil {
    my ($y, $m, $d) = @_;
    $y -= ($m <= 2) ? 1 : 0;
    my $era = int(($y >= 0 ? $y : $y - 399) / 400);
    my $yoe = $y - $era * 400;
    my $doy = int((153 * ($m + ($m > 2 ? -3 : 9)) + 2) / 5) + $d - 1;
    my $doe = $yoe * 365 + int($yoe / 4) - int($yoe / 100) + $doy;
    return $era * 146097 + $doe - 719468;
}

sub civil_from_days {
    my ($z) = @_;
    $z += 719468;
    my $era = int(($z >= 0 ? $z : $z - 146096) / 146097);
    my $doe = $z - $era * 146097;
    my $yoe = int(($doe - int($doe/1460) + int($doe/36524) - int($doe/146096)) / 365);
    my $y   = $yoe + $era * 400;
    my $doy = $doe - (365 * $yoe + int($yoe/4) - int($yoe/100));
    my $mp  = int((5 * $doy + 2) / 153);
    my $d   = $doy - int((153 * $mp + 2) / 5) + 1;
    my $m   = $mp + ($mp < 10 ? 3 : -9);
    $y += 1 if $m <= 2;
    return ($y, $m, $d);
}

sub ts_of { my ($day, $h, $mi, $s) = @_; return $day * 86400 + $h * 3600 + $mi * 60 + $s; }
sub year_of_ts { my ($ts) = @_; my ($y) = civil_from_days(int($ts / 86400)); return $y; }

# The achievement engine's absolute week index: unix day 4 is Mon 1970-01-05.
sub week_of_ts { my ($ts) = @_; return int((int($ts / 86400) - 4) / 7); }

# ------------------------------------------------- the parser's naming rules

# Ports of strip_tracknum / strip_tracknum_dot / path_to_meta from
# wrapped_core.h. Kept literal rather than tidied: the value is in matching.

sub strip_tracknum {
    my ($s) = @_;
    return $s unless $s =~ /^(\d+)/;
    my $rest = substr($s, length($1));
    $rest =~ s/^[.\ \-_]+//;
    return $rest ne '' ? $rest : $s;
}

sub strip_tracknum_dot {
    my ($s) = @_;
    return $s unless $s =~ /^(\d+)\./;
    my $rest = substr($s, length($1) + 1);
    $rest =~ s/^[\ _]+//;
    return $rest ne '' ? $rest : $s;
}

sub clip { my ($s) = @_; return length($s) > $NAME_MAX ? substr($s, 0, $NAME_MAX) : $s; }

sub path_to_meta {
    my ($path) = @_;
    my $last = rindex($path, '/');
    my $fname = $last >= 0 ? substr($path, $last + 1) : $path;

    my $stem = length($fname) > 159 ? substr($fname, 0, 159) : $fname;
    my $dot = rindex($stem, '.');
    $stem = substr($stem, 0, $dot) if $dot > 0;

    my ($artist, $title);
    my $sep1 = index($stem, ' - ');
    if ($sep1 >= 0) {
        $artist = clip(substr($stem, 0, $sep1));
        $artist = clip(strip_tracknum_dot($artist));
        my $after1 = substr($stem, $sep1 + 3);
        my $sep2 = index($after1, ' - ');
        my $tsrc = $sep2 >= 0 ? substr($after1, $sep2 + 3) : $after1;
        $title = clip(strip_tracknum($tsrc));
    } else {
        # No "Artist - " in the name: fall back to the parent folder, which
        # for <artist>/<album>/<track> is the ALBUM directory.
        if ($last > 0) {
            my $p = rindex($path, '/', $last - 1);
            my $start = $p >= 0 ? $p + 1 : 0;
            $artist = clip(substr($path, $start, $last - $start));
        } else {
            $artist = '';
        }
        $title = clip(strip_tracknum($stem));
    }

    $artist = '(unknown)' if $artist eq '';
    $title  = $fname      if $title  eq '';
    return ($artist, $title);
}

# ------------------------------------------------------------- the library

# Real device paths, one per line. Track durations are not in the list and
# reading them would mean parsing tags, so each gets a plausible length
# derived from its own path -- deterministic, and stable for a given track
# across runs, which is what a real log looks like.
my @TRACKS;
{
    open(my $fh, '<', $TRACKFILE) or die "$TRACKFILE: $!\n";
    while (my $line = <$fh>) {
        $line =~ s/\r?\n\z//;
        next if $line eq '';
        my $h = 0;
        $h = ($h * 31 + ord($_)) & 0x7FFFFFFF for split //, $line;
        push @TRACKS, { path => $line, length => (140 + $h % 181) * 1000 };
    }
    close $fh;
    die "$TRACKFILE: no tracks\n" unless @TRACKS;
}

# What the database would say, assuming tags follow the folders: the component
# after "Music" is the artist, the containing folder is the album. An honest
# assumption for a tidy library and stated as one -- a folder whose tags
# disagree moves a play from one predicted bucket to another without making
# anything wrong.
# The title a tagged file would carry: its filename, stripped of extension and
# track number.
sub filename_title {
    my ($path) = @_;
    my $fname = (split m{/}, $path)[-1];
    my $dot = rindex($fname, '.');
    my $stem = $dot > 0 ? substr($fname, 0, $dot) : $fname;
    return clip(strip_tracknum($stem));
}

sub folder_meta {
    my ($path) = @_;
    my @seg = split m{/}, $path;
    my ($artist, $album) = ('', '');
    for my $i (0 .. $#seg - 1) {
        if ($seg[$i] eq 'Music') { $artist = $seg[$i + 1] // ''; last; }
    }
    $album = $seg[$#seg - 1] // '';
    return (clip($artist), clip($album));
}

# ------------------------------------------------------------------ tallying
#
# Mirrors playback_parse's accounting exactly. Every counter here has a
# counterpart the stage-1 debug screen should print.

my (%artist_plays, %artist_secs, %title_plays, %title_skips, %album_plays);
# The same three, as a working database would key them (see folder_meta).
my (%db_artist, %db_album, %db_title);
my (%day_secs, %day_plays, %week_secs, %year);
my ($lines, $plays, $skips, $taps, $secs, $night, $invalid_ts) = (0) x 7;
my ($ts_min, $ts_max);
my @hour_hist = (0) x 24;

# One entry, already reduced to what both formats have in common. The two
# differ in how they decide "played" and in what a play is worth, so that
# decision is made by the caller and this only does the arithmetic.
sub tally {
    my ($ts, $listened, $skipped, $elapsed, $artist, $album, $title) = @_;

    my $valid_ts = ($ts >= $MIN_VALID_TS);

    $lines++;
    $invalid_ts++ unless $valid_ts;

    my $y = $valid_ts ? year_of_ts($ts) : 0;
    $year{$y} ||= { lines => 0, plays => 0, skips => 0, secs => 0 };
    $year{$y}{lines}++;

    if ($listened) {
        $plays++;
        $secs += $elapsed;
        $year{$y}{plays}++;
        $year{$y}{secs} += $elapsed;

        my $is_night = $valid_ts && int(($ts % 86400) / 3600) < 5;
        $night++ if $is_night;

        $artist_plays{$artist}++;
        $artist_secs{$artist} += $elapsed;
        $title_plays{$title}++;
        $album_plays{$album}++;

        if ($valid_ts) {
            $ts_min = $ts if !defined $ts_min || $ts < $ts_min;
            $ts_max = $ts if !defined $ts_max || $ts > $ts_max;
            $hour_hist[int(($ts % 86400) / 3600)]++;
            my $day = int($ts / 86400);
            $day_plays{$day}++;
            $day_secs{$day} += $elapsed;
            $week_secs{week_of_ts($ts)} += $elapsed;
        }
    }
    elsif ($skipped) {
        $skips++;
        $year{$y}{skips}++;
        $title_skips{$title}++;
        $title_plays{$title} += 0;      # the title exists, with zero plays
    }
    else {
        # Under 5s heard: a browsing tap. Counted as a data line and nothing
        # else -- not a play, not a skip, no title record. The scrobbler
        # format cannot express this, so it never lands here.
        $taps++;
    }
}

# ------------------------------------------------------------- file emission

my @files;                  # [ name, content ] oldest first
my $cur = '';
my $cur_is_live = 0;

sub rotate_if_needed {
    # The scrobbler log is never rotated; the core appends to it forever.
    return if $SCROB;
    # Real rotation happens at boot, when the file is already over the cap --
    # so a rotated file is slightly LARGER than $ROTATE_AT, never smaller.
    if (length($cur) > $ROTATE_AT) {
        push @files, $cur;
        $cur = '';
    }
}

sub emit_header {
    my ($ts) = @_;
    # The scrobbler log carries one preamble, written when the file is
    # created, and nothing per boot. It is emitted up front instead.
    return if $SCROB;
    my ($y, $m, $d) = civil_from_days(int($ts / 86400));
    my $h  = int(($ts % 86400) / 3600);
    my $mi = int(($ts % 3600) / 60);
    # Only the leading '#' matters to the parser -- every '#' line is skipped
    # before any field is looked at. The shape follows allocate_playback_log's.
    $cur .= sprintf("# Started Ver. 4.0-testlog %04d-%02d-%02d Time %02d-%02d-00\n",
                    $y, $m, $d, $h, $mi);
}

sub emit {
    my ($ts, $el_ms, $len_ms, $path) = @_;

    if ($SCROB) {
        # The writer's own rule, integer division and all (playback.c's
        # format_playbacklog): elapsed >= length/100 * SCROBBLER_LISTENED_PCT.
        # It is NOT the reader's rule, so the same play can be a listen in one
        # format and a skip in the other -- a long track heard for five
        # minutes passes the reader's four-minute clause and fails this one.
        my $listened = $el_ms >= int($len_ms / 100) * 50;
        my ($artist, $album) = folder_meta($path);
        my $title = filename_title($path);

        # Tags the file would carry. A track number is written but nothing
        # reads it, and the MusicBrainz field is empty -- as it is for
        # everything this fork logs.
        $cur .= sprintf("%s\t%s\t%s\t%d\t%d\t%s\t%d\t\n",
                        $artist, $album, $title, 0,
                        int($len_ms / 1000), $listened ? 'L' : 'S', $ts);

        # A play is credited with the whole track; a skip with nothing. There
        # is no elapsed time in the file to do better with.
        tally($ts, $listened, !$listened,
              $listened ? int($len_ms / 1000) : 0, $artist, $album, $title);
        return;
    }

    $cur .= sprintf("%d:%d:%d:%s\n", $ts, $el_ms, $len_ms, $path);

    my $listened = ($len_ms == 0) || ($el_ms * 2 >= $len_ms)
                                  || ($el_ms >= 240000);
    my ($artist, $title) = path_to_meta($path);
    my ($fart, $falb) = folder_meta($path);

    # Both name models, so the report can show what a working database join
    # produces against what a broken one does.
    if ($listened) {
        $db_artist{$fart}++;
        $db_album{$falb}++;
        $db_title{$title}++;
    }

    # Without a database there is no album name, so playback_parse buckets the
    # play under the artist instead.
    tally($ts, $listened, !$listened && $el_ms >= 5000,
          int($el_ms / 1000), $artist, $artist, $title);
}

# ----------------------------------------------------------------- the shape

# The Audioscrobbler preamble, written once when the core creates the file
# (allocate_playback_log). Every line of it starts with '#', so a reader that
# skips comments needs to know nothing about it.
if ($SCROB) {
    $cur .= "#AUDIOSCROBBLER/1.1\n#TZ/UNKNOWN\n#CLIENT/Rockbox ipodvideo\n"
          . "#ARTIST\t#ALBUM\t#TITLE\t#TRACKNUM\t#LENGTH\t#RATING\t"
          . "#TIMESTAMP\t#MUSICBRAINZ_TRACKID\n";
}

my $DAY0 = days_from_civil(2025, 3, 1);
my $DAY1 = days_from_civil(2026, 8, 1);

# The week that gets 24h+ of listening, making it a SUPERWEEK (and, with the
# padding below, a comfortable margin over the 1440-minute threshold).
my $SUPER_DAY = days_from_civil(2025, 11, 10);      # a Monday
my %SUPER = map { ($SUPER_DAY + $_) => 1 } (0 .. 6);

# Entries with a year-2000 timestamp, as an unset RTC produces. Emitted on one
# day so they are easy to find in the file.
my $UNSET_RTC_DAY = days_from_civil(2025, 6, 12);

# Listening happens in sessions, not one continuous block from breakfast. The
# spread matters: an evening session that runs past midnight is what puts
# entries in the 00:00-04:59 window the 3am Club card and the night stat are
# built on, and a day's plays landing in one narrow band makes the listening
# clock card look broken.
my @START_HOURS = (7, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 19,
                   20, 21, 21, 22, 22, 23, 23, 0, 1);

for (my $day = $DAY0; $day <= $DAY1; $day++) {

    # Rotation happens at boot, before anything is written to the fresh file --
    # so a rotated family never has a header split across two files.
    rotate_if_needed();
    emit_header(ts_of($day, 7, 30, 0)) if pick(4) != 0;

    my $n;
    if ($SUPER{$day})    { $n = between(88, 105); }  # ~24h+ of audio over the week
    elsif (pick(9) == 0) { $n = 0; }                 # a day off
    else                 { $n = between(8, 34); }

    my $sessions = $n <= 12 ? 1 : ($n <= 40 ? between(1, 2) : between(2, 4));
    my $left = $n;
    my $done_today = 0;

    for (my $s = 0; $s < $sessions && $left > 0; $s++) {
        my $take;
        if ($s == $sessions - 1) {
            $take = $left;
        } else {
            my $max = $left - ($sessions - 1 - $s);   # leave one for each later session
            $max = 1 if $max < 1;
            $take = between(1, $max);
        }
        $left -= $take;

        # Absolute cursor, deliberately NOT wrapped within the day: a session
        # that starts at 23:00 runs into tomorrow's small hours, which is
        # exactly the case worth generating.
        my $cursor = $day * 86400
                   + $START_HOURS[pick(scalar @START_HOURS)] * 3600
                   + pick(3600);

        for (my $i = 0; $i < $take; $i++) {
            my $t = $TRACKS[pick(scalar @TRACKS)];
            my $len = $t->{length};

            $cursor += int($len / 1000) + between(2, 90);

            my $roll = pick(100);
            my $el;
            if ($roll < 8)      { $el = between(600, 4900); }               # browsing tap
            elsif ($roll < 22)  { $el = between(5000, int($len / 2) - 1); } # skip
            else                { $el = between(int($len / 2), $len); }     # play

            # A handful of entries with an unknown length: the parser treats
            # len_ms == 0 as listened regardless of how little was heard.
            $len = 0 if pick(200) == 0;
            $el = $len if $len > 0 && $el > $len;

            if ($day == $UNSET_RTC_DAY && $done_today < 4) {
                # Plays logged before the clock was set. Counted as data lines
                # and as plays, but excluded from every date-derived figure.
                emit(946684800 + $done_today * 300, $el, $len, $t->{path});
            } else {
                emit($cursor, $el, $len, $t->{path});
            }
            $done_today++;
        }
    }
}

# The live log's final line is left unterminated, which is what an interrupted
# flush leaves behind. lrd_line must still return it.
$cur =~ s/\n\z//;

push @files, $cur;

# ---------------------------------------------------------------- write out

mkdir $OUT unless -d $OUT;

my @names;
for (my $i = 0; $i < @files; $i++) {
    my $name = $SCROB ? '.scrobbler.log'
             : ($i == $#files) ? 'playback.log'
                               : sprintf('playback_%04d.log', $i + 1);
    push @names, $name;
    open(my $fh, '>', "$OUT/$name") or die "$OUT/$name: $!\n";
    binmode $fh;
    print $fh $files[$i];
    close $fh;
}

# --------------------------------------------------------------- expectations

sub top_n {
    my ($h, $n) = @_;
    # Ties are broken by name here. The parser's top_n() breaks them by
    # insertion order instead, so a tie at the boundary can legitimately
    # differ -- compare the counts, not the order, if that happens.
    my @k = sort { $h->{$b} <=> $h->{$a} || $a cmp $b } keys %$h;
    return @k > $n ? @k[0 .. $n - 1] : @k;
}

sub longest_streak {
    my @d = sort { $a <=> $b } keys %day_plays;
    my ($best, $run) = (0, 0);
    for (my $i = 0; $i < @d; $i++) {
        $run = ($i > 0 && $d[$i] == $d[$i - 1] + 1) ? $run + 1 : 1;
        $best = $run if $run > $best;
    }
    return $best;
}

sub fmt_day { my ($d) = @_; return sprintf('%04d-%02d-%02d', civil_from_days($d)); }

open(my $x, '>', "$OUT/spun_expected.txt") or die "$OUT/spun_expected.txt: $!\n";

my $ntracks = scalar @TRACKS;
print $x <<"HDR";
Spun test log -- expected parse results
=======================================
Generated by tools/spun_testlog.pl, seed $SEED_INITIAL,
over $ntracks real tracks from $TRACKFILE.
Same seed and same track list reproduces these bytes exactly.

The name-independent figures below are derived by applying playback_parse's
own rules (the listened/skip test, elapsed/1000, the night window) to each
emitted entry. They must match whatever the device reports.

The name-dependent ones come in two versions, and which one the device shows
is the test -- see "Names" below.

Files, in the order the reader streams them
-------------------------------------------
HDR

for (my $i = 0; $i < @names; $i++) {
    my $body = $files[$i];
    my $nl = () = $body =~ /\n/g;
    printf $x "  %-20s %8d bytes  %6d lines\n", $names[$i], length($body), $nl + 1;
}

my $mins = int($secs / 60);
printf $x <<"ALL", $lines, $plays, $skips, $taps, $secs, $mins, int($mins/60), $night, $invalid_ts;

All-time
--------
  data lines (non-#, non-empty, parseable)   %d
  plays      (listened)                      %d
  skips      (not listened, >= 5s heard)     %d
  browsing taps (< 5s heard: counted as
                 a data line and nothing else) %d
  listened seconds                           %d
  listened minutes                           %d
  listened hours                             %d
  night plays (00:00-04:59, valid clock)     %d
  entries with an unset clock (< 2005)       %d
ALL

printf $x "  distinct listening days                    %d\n", scalar keys %day_plays;
printf $x "  longest streak of consecutive days         %d\n", longest_streak();
printf $x "     (assumed definition: consecutive calendar days with at\n";
printf $x "      least one listened play and a valid timestamp)\n";
printf $x "  first play                                 %s (ts %d)\n", fmt_day(int($ts_min/86400)), $ts_min;
printf $x "  last play                                  %s (ts %d)\n", fmt_day(int($ts_max/86400)), $ts_max;

if ($SCROB) {
    printf $x <<"SNAMES", scalar keys %artist_plays, scalar keys %album_plays, scalar keys %title_plays;

Names
-----
The scrobbler format carries tagged names in the file, so there is no database
join to test and no fallback to fall back to. These are simply what the log
says, and the device must match them exactly:

    unique artists   %d
    unique albums    %d
    unique titles    %d

Note what this format cannot do: it records a played/skipped verdict and the
track's full length, with no elapsed time. So "minutes" above is the total
length of the tracks that counted as played -- higher than the playback log's
figure for the same listening -- and every browsing tap is indistinguishable
from a real skip, so taps is 0 and skips absorbs them.

The played/skipped decision is also made by the WRITER here, using its own
rule (elapsed >= length/100 * 50), not by the reader's. The reader's rule has
a four-minute clause the writer's lacks, so a track over eight minutes heard
for five is a play in the playback log and a skip in this one.

That divergence is NOT exercised by this fixture -- no track in it is long
enough for the four-minute clause to apply -- so plays here match the playback
log's exactly. The differences you should see are confined to minutes (higher)
and skips (which absorb the taps). It is not a code path either way: this
reader takes the verdict already in the file.
SNAMES
} else {
print $x <<"NAMES";

Names -- the discriminating figures
-----------------------------------
Every logged path names a file the database knows, so the counts below depend
entirely on whether Spun's database join is working.

  WITH the database (what a correct build should show)
    unique artists   @{[ scalar keys %db_artist ]}
    unique albums    @{[ scalar keys %db_album ]}
    unique titles    @{[ scalar keys %db_title ]}

  WITHOUT it, falling back to path_to_meta (what a broken join shows)
    unique artists   @{[ scalar keys %artist_plays ]}
    unique albums    @{[ scalar keys %album_plays ]}   (keyed by artist)
    unique titles    @{[ scalar keys %title_plays ]}

The first block is a PREDICTION: it assumes each file's tags agree with its
folders -- artist from the component after "Music", album from the containing
folder, title from the filename. A folder whose tags disagree moves a play
between predicted buckets, so treat small differences as a tagging quirk and
a collapse to the second block as a broken join.

The artist figures are the ones to read. This library's filenames are mostly
"NN Title.ext" with no " - ", so path_to_meta falls back to the parent folder
-- which is the ALBUM, not the artist. A build with no database join reports
roughly one "artist" per album.

Also worth checking on the device: the debug screen's own "from db" and "from
path" counters, which say the same thing directly.
NAMES
}

print $x "\nPer calendar year\n-----------------\n";
for my $y (sort keys %year) {
    my $v = $year{$y};
    my $label = $y ? $y : 'unset clock';
    printf $x "  %-12s lines %6d   plays %6d   skips %5d   minutes %7d\n",
        $label, $v->{lines}, $v->{plays}, $v->{skips}, int($v->{secs} / 60);
}

print $x "\nTop 10 artists by plays (path_to_meta names)\n";
print $x "--------------------------------------------\n";
for my $a (top_n(\%artist_plays, 10)) {
    printf $x "  %-42s %5d plays  %6d min\n", $a, $artist_plays{$a}, int($artist_secs{$a} / 60);
}

print $x "\nTop 10 titles by plays\n----------------------\n";
for my $t (top_n(\%title_plays, 10)) {
    printf $x "  %-42s %5d plays  %5d skips\n", $t, $title_plays{$t}, ($title_skips{$t} || 0);
}

print $x "\nTop 10 titles by skips\n----------------------\n";
for my $t (top_n(\%title_skips, 10)) {
    printf $x "  %-42s %5d skips\n", $t, $title_skips{$t};
}

print $x "\nWeeks at SUPERWEEK or above\n---------------------------\n";
print $x "  (week index is ((ts/86400) - 4) / 7, the achievement engine's;\n";
print $x "   tiers: SUPER 1440 min, ULTRA 2880, HYPER 4320, GIGA 5760, OMEGA 7200)\n";
my $any_super = 0;
for my $w (sort { $a <=> $b } keys %week_secs) {
    my $m = int($week_secs{$w} / 60);
    next if $m < 1440;
    my $tier = $m >= 7200 ? 'OMEGA' : $m >= 5760 ? 'GIGA'
             : $m >= 4320 ? 'HYPER' : $m >= 2880 ? 'ULTRA' : 'SUPER';
    printf $x "  week %-8d starting %s   %6d min  %s\n",
        $w, fmt_day($w * 7 + 4), $m, $tier;
    $any_super = 1;
}
print $x "  (none)\n" unless $any_super;

print $x "\nHour histogram (listened plays, valid clock)\n";
print $x "--------------------------------------------\n";
for my $h (0 .. 23) {
    printf $x "  %02d:00  %5d%s\n", $h, $hour_hist[$h], ($h < 5 ? '   <- night' : '');
}

print $x <<'CASES';

Cases this family deliberately contains
---------------------------------------
  rotation boundary         more than one file; the reader must step across
                            without dropping or duplicating a line
  two calendar years        2025 and 2026, for the year picker
  a SUPERWEEK               see the week table above
  night plays               00:00-04:59, for the 3am Club and the night stat
  skips                     >= 5s heard but under half the track
  browsing taps             < 5s heard: must count as neither play nor skip
  unset-clock entries       year-2000 timestamps, below MIN_VALID_TS
  unknown track length      len_ms == 0, which counts as listened regardless
  real device paths         every entry names a file the database knows, so
                            the join is exercised rather than assumed -- and
                            so is the artwork cache, which keys off the same
                            strings
  both naming branches      most filenames carry no " - " and go through the
                            parent-folder fallback; a few hundred do carry
                            one and go through the "Artist - Album - NN Title"
                            path
  unterminated final line   the live log ends without a newline
CASES

close $x;

printf "Wrote %d file(s) to %s/\n", scalar(@names) + 1, $OUT;
printf "  %-20s %8d bytes\n", $names[$_], length($files[$_]) for (0 .. $#names);
printf "  %-20s\n", 'spun_expected.txt';
printf "\n%d data lines: %d plays, %d skips, %d taps, %d minutes listened\n",
    $lines, $plays, $skips, $taps, $mins;
print  "Copy the *.log files to <device>:\\.rockbox\\ by hand.\n";
