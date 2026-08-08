/***************************************************************************
 * Original code from the Spun plugin (Stats_for_iPod)
 * was: apps/plugins/ach_table.h
 * Copyright (C) 2026 Siebe Majoor
 * GNU General Public License (version 2+)
 *
 * The badge definitions. Data only -- which is what earns this a header of
 * its own rather than a place in pv_badges.c.
 *
 * Five of the original 345 rows are gone, because the features behind them
 * are: the achievement watcher's test-toast badges (there is no watcher) and
 * the tamper-detection badge (there is no signing). Nothing else was touched,
 * and the meta rows still top out at 280 against 332 non-meta badges, so the
 * wall is still finishable.
 ****************************************************************************/
#ifndef _PV_BADGES_TABLE_H
#define _PV_BADGES_TABLE_H

static const struct pv_badge pv_badges_table[] = {
    /* ---- LISTENING TIME ---- */
    { "First Note",             "listen for 1 minute",                    PV_AM_MINUTES, 0, 1, PV_TIER_GOLD },
    { "Sound Check",            "listen for 5 minutes",                   PV_AM_MINUTES, 0, 5, PV_TIER_GOLD },
    { "Warming Up",             "listen for 15 minutes",                  PV_AM_MINUTES, 0, 15, PV_TIER_GOLD },
    { "Side A",                 "listen for 30 minutes - flip it over",   PV_AM_MINUTES, 0, 30, PV_TIER_GOLD },
    { "Hour of Power",          "listen for 60 minutes",                  PV_AM_MINUTES, 0, 60, PV_TIER_GOLD },
    { "Double LP",              "listen for 120 minutes",                 PV_AM_MINUTES, 0, 120, PV_TIER_GOLD },
    { "Five Hour Groove",       "listen for 300 minutes",                 PV_AM_MINUTES, 0, 300, PV_TIER_GOLD },
    { "Marathon Session",       "listen for 600 minutes",                 PV_AM_MINUTES, 0, 600, PV_TIER_GOLD },
    { "Kilominute",             "listen for 1,000 minutes",               PV_AM_MINUTES, 0, 1000, PV_TIER_GOLD },
    { "Two Kilominutes",        "listen for 2,000 minutes",               PV_AM_MINUTES, 0, 2000, PV_TIER_GOLD },
    { "Ear Conditioning",       "listen for 3,500 minutes",               PV_AM_MINUTES, 0, 3500, PV_TIER_GOLD },
    { "Audio Devotee",          "listen for 5,000 minutes",               PV_AM_MINUTES, 0, 5000, PV_TIER_GOLD },
    { "Wall of Sound",          "listen for 7,500 minutes",               PV_AM_MINUTES, 0, 7500, PV_TIER_GOLD },
    /* WA4b insert at slot 13 (ACH_INS_9000) - the ONE sanctioned
     * mid-table insert; ach_seen_load() remaps older save files */
    { "It's Over 9000!",        "listen for more than 9,000 minutes",     PV_AM_MINUTES, 0, 9001, PV_TIER_GOLD },
    { "Five Figure Ears",       "listen for 10,000 minutes",              PV_AM_MINUTES, 0, 10000, PV_TIER_GOLD },
    { "Ten Days Solid",         "listen for 15,000 minutes",              PV_AM_MINUTES, 0, 15000, PV_TIER_GOLD },
    { "Audiophile Ascendant",   "listen for 25,000 minutes",              PV_AM_MINUTES, 0, 25000, PV_TIER_ICE },
    { "The Long Groove",        "listen for 40,000 minutes",              PV_AM_MINUTES, 0, 40000, PV_TIER_ICE },
    { "Vinyl Monk",             "listen for 60,000 minutes",              PV_AM_MINUTES, 0, 60000, PV_TIER_VIO },
    { "One Hundred Grand",      "listen for 100,000 minutes",             PV_AM_MINUTES, 0, 100000, PV_TIER_VIO },
    { "Beyond the Charts",      "listen for 150,000 minutes",             PV_AM_MINUTES, 0, 150000, PV_TIER_EMBER },
    { "Halfway to Absurd",      "listen for 250,000 minutes",             PV_AM_MINUTES, 0, 250000, PV_TIER_EMBER },
    { "Touch Grass",            "listen for 400,000 minutes - go outside", PV_AM_MINUTES, PV_BADGE_SECRET, 400000, PV_TIER_EMBER },
    { "The Gladwell Line",      "listen for 600,000 min - 10,000 hours",  PV_AM_MINUTES, PV_BADGE_SECRET, 600000, PV_TIER_OMEGA },
    { "Heat Death of the iPod", "listen for 1,000,000 minutes",           PV_AM_MINUTES, PV_BADGE_SECRET, 1000000, PV_TIER_OMEGA },

    /* ---- PLAYS ---- */
    { "Press Play",             "finish 1 play",                          PV_AM_PLAYS, 0, 1, PV_TIER_GOLD },
    { "Hooked",                 "log 5 plays",                            PV_AM_PLAYS, 0, 5, PV_TIER_GOLD },
    { "EP's Worth",             "log 10 plays",                           PV_AM_PLAYS, 0, 10, PV_TIER_GOLD },
    { "Mixtape",                "log 25 plays",                           PV_AM_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Crate Digger",           "log 50 plays",                           PV_AM_PLAYS, 0, 50, PV_TIER_GOLD },
    { "Century of Songs",       "log 100 plays",                          PV_AM_PLAYS, 0, 100, PV_TIER_GOLD },
    { "Double Century",         "log 200 plays",                          PV_AM_PLAYS, 0, 200, PV_TIER_GOLD },
    { "B-Side Devourer",        "log 350 plays",                          PV_AM_PLAYS, 0, 350, PV_TIER_GOLD },
    { "Five Hundred Spins",     "log 500 plays",                          PV_AM_PLAYS, 0, 500, PV_TIER_GOLD },
    { "Setlist Veteran",        "log 750 plays",                          PV_AM_PLAYS, 0, 750, PV_TIER_GOLD },
    { "The Thousandaire",       "log 1,000 plays",                        PV_AM_PLAYS, 0, 1000, PV_TIER_GOLD },
    { "Fifteen Hundred Deep",   "log 1,500 plays",                        PV_AM_PLAYS, 0, 1500, PV_TIER_GOLD },
    { "Jukebox Hero",           "log 2,500 plays",                        PV_AM_PLAYS, 0, 2500, PV_TIER_GOLD },
    { "Four Thousand Holes",    "log 4,000 plays",                        PV_AM_PLAYS, 0, 4000, PV_TIER_GOLD },
    { "Six Thousand Strong",    "log 6,000 plays",                        PV_AM_PLAYS, 0, 6000, PV_TIER_GOLD },
    { "Myriad",                 "log 10,000 plays",                       PV_AM_PLAYS, 0, 10000, PV_TIER_ICE },
    { "Playback Machine",       "log 15,000 plays",                       PV_AM_PLAYS, 0, 15000, PV_TIER_ICE },
    { "Industrial Playback",    "log 25,000 plays",                       PV_AM_PLAYS, 0, 25000, PV_TIER_VIO },
    { "Spin Doctor Emeritus",   "log 50,000 plays",                       PV_AM_PLAYS, 0, 50000, PV_TIER_VIO },
    { "Playtergeist",           "log 100,000 plays",                      PV_AM_PLAYS, PV_BADGE_SECRET, 100000, PV_TIER_EMBER },
    { "The Algorithm Fears You", "log 250,000 plays",                     PV_AM_PLAYS, PV_BADGE_SECRET, 250000, PV_TIER_OMEGA },

    /* ---- SKIPS ---- */
    { "Next!",                  "skip 1 track",                           PV_AM_SKIPS, 0, 1, PV_TIER_GOLD },
    { "Not Feeling It",         "skip 5 tracks",                          PV_AM_SKIPS, 0, 5, PV_TIER_GOLD },
    { "Channel Surfer",         "skip 10 tracks",                         PV_AM_SKIPS, 0, 10, PV_TIER_GOLD },
    { "Picky",                  "skip 25 tracks",                         PV_AM_SKIPS, 0, 25, PV_TIER_GOLD },
    { "Quality Control",        "skip 50 tracks",                         PV_AM_SKIPS, 0, 50, PV_TIER_GOLD },
    { "The Curator",            "skip 100 tracks",                        PV_AM_SKIPS, 0, 100, PV_TIER_GOLD },
    { "Ruthless",               "skip 250 tracks",                        PV_AM_SKIPS, 0, 250, PV_TIER_GOLD },
    { "Skipper",                "skip 500 tracks",                        PV_AM_SKIPS, 0, 500, PV_TIER_GOLD },
    { "Kiloskip",               "skip 1,000 tracks",                      PV_AM_SKIPS, 0, 1000, PV_TIER_GOLD },
    { "Commitment Issues",      "skip 2,500 tracks",                      PV_AM_SKIPS, PV_BADGE_SECRET, 2500, PV_TIER_ICE },
    { "Why Even Have a Library", "skip 10,000 tracks",                    PV_AM_SKIPS, PV_BADGE_SECRET, 10000, PV_TIER_VIO },
    { "The Grand Refusal",      "skip 100,000 tracks - but why",          PV_AM_SKIPS, PV_BADGE_SECRET, 100000, PV_TIER_EMBER },

    /* ---- LIBRARY DEPTH ---- */
    { "Track One",              "play 1 distinct song",                   PV_AM_UNIQ_TRACKS, 0, 1, PV_TIER_GOLD },
    { "Sampler Platter",        "play 5 distinct songs",                  PV_AM_UNIQ_TRACKS, 0, 5, PV_TIER_GOLD },
    { "Variety Pack",           "play 10 distinct songs",                 PV_AM_UNIQ_TRACKS, 0, 10, PV_TIER_GOLD },
    { "Getting Around",         "play 25 distinct songs",                 PV_AM_UNIQ_TRACKS, 0, 25, PV_TIER_GOLD },
    { "Deep Cuts",              "play 50 distinct songs",                 PV_AM_UNIQ_TRACKS, 0, 50, PV_TIER_GOLD },
    { "Century Collection",     "play 100 distinct songs",                PV_AM_UNIQ_TRACKS, 0, 100, PV_TIER_GOLD },
    { "Shuffle Fodder",         "play 150 distinct songs",                PV_AM_UNIQ_TRACKS, 0, 150, PV_TIER_GOLD },
    { "Crate of Crates",        "play 250 distinct songs",                PV_AM_UNIQ_TRACKS, 0, 250, PV_TIER_GOLD },
    { "The Back Catalog",       "play 400 distinct songs",                PV_AM_UNIQ_TRACKS, 0, 400, PV_TIER_GOLD },
    { "Six Hundred Songs",      "play 600 distinct songs",                PV_AM_UNIQ_TRACKS, 0, 600, PV_TIER_GOLD },
    { "Librarian",              "play 1,000 distinct songs",              PV_AM_UNIQ_TRACKS, 0, 1000, PV_TIER_GOLD },
    { "Discography Diver",      "play 1,500 distinct songs",              PV_AM_UNIQ_TRACKS, 0, 1500, PV_TIER_GOLD },
    { "The Archivist",          "play 2,500 distinct songs",              PV_AM_UNIQ_TRACKS, 0, 2500, PV_TIER_ICE },
    { "Five Thousand Deep",     "play 5,000 distinct songs",              PV_AM_UNIQ_TRACKS, 0, 5000, PV_TIER_VIO },
    { "Infinite Jukebox",       "play 10,000 distinct songs",             PV_AM_UNIQ_TRACKS, 0, 10000, PV_TIER_VIO },
    { "Alexandria, but Audio",  "play 25,000 distinct songs",             PV_AM_UNIQ_TRACKS, PV_BADGE_SECRET, 25000, PV_TIER_EMBER },

    /* ---- ARTISTS MET ---- */
    { "One Band Fan",           "play 1 distinct artist",                 PV_AM_UNIQ_ARTISTS, 0, 1, PV_TIER_GOLD },
    { "Power Trio",             "play 3 distinct artists",                PV_AM_UNIQ_ARTISTS, 0, 3, PV_TIER_GOLD },
    { "Starting Five",          "play 5 distinct artists",                PV_AM_UNIQ_ARTISTS, 0, 5, PV_TIER_GOLD },
    { "Ten Piece Band",         "play 10 distinct artists",               PV_AM_UNIQ_ARTISTS, 0, 10, PV_TIER_GOLD },
    { "Festival Lineup",        "play 20 distinct artists",               PV_AM_UNIQ_ARTISTS, 0, 20, PV_TIER_GOLD },
    { "Genre Tourist",          "play 35 distinct artists",               PV_AM_UNIQ_ARTISTS, 0, 35, PV_TIER_GOLD },
    { "Radio Rotation",         "play 50 distinct artists",               PV_AM_UNIQ_ARTISTS, 0, 50, PV_TIER_GOLD },
    { "Scene Historian",        "play 75 distinct artists",               PV_AM_UNIQ_ARTISTS, 0, 75, PV_TIER_GOLD },
    { "A Hundred Voices",       "play 100 distinct artists",              PV_AM_UNIQ_ARTISTS, 0, 100, PV_TIER_GOLD },
    { "Label Executive",        "play 150 distinct artists",              PV_AM_UNIQ_ARTISTS, 0, 150, PV_TIER_GOLD },
    { "Human Discovery Engine", "play 250 distinct artists",              PV_AM_UNIQ_ARTISTS, 0, 250, PV_TIER_GOLD },
    { "Five Hundred Names",     "play 500 distinct artists",              PV_AM_UNIQ_ARTISTS, 0, 500, PV_TIER_ICE },
    { "Ethnomusicologist",      "play 1,000 distinct artists",            PV_AM_UNIQ_ARTISTS, 0, 1000, PV_TIER_VIO },
    { "Every Band Ever",        "play 2,500 distinct artists",            PV_AM_UNIQ_ARTISTS, PV_BADGE_SECRET, 2500, PV_TIER_EMBER },

    /* ---- ALBUM SHELF ---- */
    { "First Pressing",         "play 1 distinct album",                  PV_AM_UNIQ_ALBUMS, 0, 1, PV_TIER_GOLD },
    { "Short Stack",            "play 3 distinct albums",                 PV_AM_UNIQ_ALBUMS, 0, 3, PV_TIER_GOLD },
    { "Record Shelf",           "play 5 distinct albums",                 PV_AM_UNIQ_ALBUMS, 0, 5, PV_TIER_GOLD },
    { "Ten LPs Deep",           "play 10 distinct albums",                PV_AM_UNIQ_ALBUMS, 0, 10, PV_TIER_GOLD },
    { "Collector",              "play 20 distinct albums",                PV_AM_UNIQ_ALBUMS, 0, 20, PV_TIER_GOLD },
    { "Shelf Two",              "play 35 distinct albums",                PV_AM_UNIQ_ALBUMS, 0, 35, PV_TIER_GOLD },
    { "Fifty Sleeves",          "play 50 distinct albums",                PV_AM_UNIQ_ALBUMS, 0, 50, PV_TIER_GOLD },
    { "Vinyl Hoarder",          "play 75 distinct albums",                PV_AM_UNIQ_ALBUMS, 0, 75, PV_TIER_GOLD },
    { "Hundred Albums",         "play 100 distinct albums",               PV_AM_UNIQ_ALBUMS, 0, 100, PV_TIER_GOLD },
    { "Warehouse Wing",         "play 200 distinct albums",               PV_AM_UNIQ_ALBUMS, 0, 200, PV_TIER_GOLD },
    { "Four Hundred Sleeves",   "play 400 distinct albums",               PV_AM_UNIQ_ALBUMS, 0, 400, PV_TIER_ICE },
    { "The Stacks",             "play 1,000 distinct albums",             PV_AM_UNIQ_ALBUMS, PV_BADGE_SECRET, 1000, PV_TIER_VIO },

    /* ---- NIGHT OWL ---- */
    { "Past Bedtime",           "log 1 play between 00:00 and 04:59",     PV_AM_NIGHT_PLAYS, 0, 1, PV_TIER_GOLD },
    { "Night Shift",            "log 5 night plays (00:00-04:59)",        PV_AM_NIGHT_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Insomniac Intro",        "log 10 night plays (00:00-04:59)",       PV_AM_NIGHT_PLAYS, 0, 10, PV_TIER_GOLD },
    { "3 AM Thoughts",          "log 25 night plays (00:00-04:59)",       PV_AM_NIGHT_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Nocturne",               "log 50 night plays (00:00-04:59)",       PV_AM_NIGHT_PLAYS, 0, 50, PV_TIER_GOLD },
    { "Creature of the Night",  "log 100 night plays (00:00-04:59)",      PV_AM_NIGHT_PLAYS, 0, 100, PV_TIER_GOLD },
    { "Vampire Hours",          "log 250 night plays (00:00-04:59)",      PV_AM_NIGHT_PLAYS, 0, 250, PV_TIER_GOLD },
    { "Sleep Is a Suggestion",  "log 1,000 night plays (00:00-04:59)",    PV_AM_NIGHT_PLAYS, PV_BADGE_SECRET, 1000, PV_TIER_ICE },
    { "The Sun Is a Lie",       "log 5,000 night plays (00:00-04:59)",    PV_AM_NIGHT_PLAYS, PV_BADGE_SECRET, 5000, PV_TIER_VIO },

    /* ---- ROUND THE CLOCK ---- */
    { "Three Hour Tour",        "play in 3 distinct hours of the day",    PV_AM_HOURS_COVERED, 0, 3, PV_TIER_GOLD },
    { "Quarter Day",            "play in 6 distinct hours of the day",    PV_AM_HOURS_COVERED, 0, 6, PV_TIER_GOLD },
    { "Nine to Five",           "play in 9 distinct hours of the day",    PV_AM_HOURS_COVERED, 0, 9, PV_TIER_GOLD },
    { "Half the Clock",         "play in 12 distinct hours of the day",   PV_AM_HOURS_COVERED, 0, 12, PV_TIER_GOLD },
    { "Fifteen Slots",          "play in 15 distinct hours of the day",   PV_AM_HOURS_COVERED, 0, 15, PV_TIER_GOLD },
    { "Mostly Awake",           "play in 18 distinct hours of the day",   PV_AM_HOURS_COVERED, 0, 18, PV_TIER_GOLD },
    { "Almost Always On",       "play in 21 distinct hours of the day",   PV_AM_HOURS_COVERED, 0, 21, PV_TIER_GOLD },
    { "Rock Around the Clock",  "play in all 24 hours of the day",        PV_AM_HOURS_COVERED, 0, 24, PV_TIER_ICE },

    /* ---- ACTIVE DAYS ---- */
    { "Day One",                "listen on 1 day",                        PV_AM_ACTIVE_DAYS, 0, 1, PV_TIER_GOLD },
    { "Three-Day Weekend",      "listen on 3 different days",             PV_AM_ACTIVE_DAYS, 0, 3, PV_TIER_GOLD },
    { "One Week Wonder",        "listen on 7 different days",             PV_AM_ACTIVE_DAYS, 0, 7, PV_TIER_GOLD },
    { "Fortnight",              "listen on 14 different days",            PV_AM_ACTIVE_DAYS, 0, 14, PV_TIER_GOLD },
    { "Three Weeks In",         "listen on 21 different days",            PV_AM_ACTIVE_DAYS, 0, 21, PV_TIER_GOLD },
    { "Month of Music",         "listen on 30 different days",            PV_AM_ACTIVE_DAYS, 0, 30, PV_TIER_GOLD },
    { "Fifty Days In",          "listen on 50 different days",            PV_AM_ACTIVE_DAYS, 0, 50, PV_TIER_GOLD },
    { "Steady Habit",           "listen on 75 different days",            PV_AM_ACTIVE_DAYS, 0, 75, PV_TIER_GOLD },
    { "Hundred Day Club",       "listen on 100 different days",           PV_AM_ACTIVE_DAYS, 0, 100, PV_TIER_GOLD },
    { "Routine, Established",   "listen on 150 different days",           PV_AM_ACTIVE_DAYS, 0, 150, PV_TIER_GOLD },
    { "Two Hundred Days",       "listen on 200 different days",           PV_AM_ACTIVE_DAYS, 0, 200, PV_TIER_GOLD },
    { "Nine Month Stretch",     "listen on 270 different days",           PV_AM_ACTIVE_DAYS, 0, 270, PV_TIER_GOLD },
    { "Year-Rounder",           "listen on 365 different days",           PV_AM_ACTIVE_DAYS, 0, 365, PV_TIER_GOLD },
    { "Five Hundred Days",      "listen on 500 different days",           PV_AM_ACTIVE_DAYS, 0, 500, PV_TIER_ICE },
    { "Two Years of Days",      "listen on 730 different days",           PV_AM_ACTIVE_DAYS, 0, 730, PV_TIER_ICE },
    { "A Thousand Days",        "listen on 1,000 different days",         PV_AM_ACTIVE_DAYS, 0, 1000, PV_TIER_VIO },
    { "Beyond Habit",           "listen on 1,500 different days",         PV_AM_ACTIVE_DAYS, 0, 1500, PV_TIER_VIO },
    { "Decade Pace",            "listen on 2,500 different days",         PV_AM_ACTIVE_DAYS, PV_BADGE_SECRET, 2500, PV_TIER_EMBER },
    { "Ten Years After",        "listen on 3,650 different days",         PV_AM_ACTIVE_DAYS, PV_BADGE_SECRET, 3650, PV_TIER_OMEGA },

    /* ---- STREAKS ---- */
    { "Back for More",          "keep a 2-day listening streak",          PV_AM_STREAK, 0, 2, PV_TIER_GOLD },
    { "Hat Trick",              "keep a 3-day listening streak",          PV_AM_STREAK, 0, 3, PV_TIER_GOLD },
    { "Five Alive",             "keep a 5-day listening streak",          PV_AM_STREAK, 0, 5, PV_TIER_GOLD },
    { "Seven Day Groove",       "keep a 7-day listening streak",          PV_AM_STREAK, 0, 7, PV_TIER_GOLD },
    { "Double Digits",          "keep a 10-day listening streak",         PV_AM_STREAK, 0, 10, PV_TIER_GOLD },
    { "Two Week Streak",        "keep a 14-day listening streak",         PV_AM_STREAK, 0, 14, PV_TIER_GOLD },
    { "Habit Formed",           "keep a 21-day listening streak",         PV_AM_STREAK, 0, 21, PV_TIER_GOLD },
    { "Thirty and Counting",    "keep a 30-day listening streak",         PV_AM_STREAK, 0, 30, PV_TIER_GOLD },
    { "Forty-Five Unbroken",    "keep a 45-day listening streak",         PV_AM_STREAK, 0, 45, PV_TIER_GOLD },
    { "Sixty Deep",             "keep a 60-day listening streak",         PV_AM_STREAK, 0, 60, PV_TIER_GOLD },
    { "Quarterly Report",       "keep a 90-day listening streak",         PV_AM_STREAK, 0, 90, PV_TIER_GOLD },
    { "Four Months Solid",      "keep a 120-day listening streak",        PV_AM_STREAK, 0, 120, PV_TIER_ICE },
    { "Half Year, No Misses",   "keep a 180-day listening streak",        PV_AM_STREAK, 0, 180, PV_TIER_ICE },
    { "The Full Orbit",         "keep a 365-day listening streak",        PV_AM_STREAK, 0, 365, PV_TIER_VIO },
    { "Two Years Unbroken",     "keep a 730-day listening streak",        PV_AM_STREAK, PV_BADGE_SECRET, 730, PV_TIER_EMBER },
    { "Streak Eternal",         "keep a 1,000-day listening streak",      PV_AM_STREAK, PV_BADGE_SECRET, 1000, PV_TIER_EMBER },

    /* ---- ONE TRACK MIND ---- */
    { "Encore",                 "play one song 2 times",                  PV_AM_TRACK_PLAYS, 0, 2, PV_TIER_GOLD },
    { "On Repeat",              "play one song 5 times",                  PV_AM_TRACK_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Certified Earworm",      "play one song 10 times",                 PV_AM_TRACK_PLAYS, 0, 10, PV_TIER_GOLD },
    { "Fifteen Replays",        "play one song 15 times",                 PV_AM_TRACK_PLAYS, 0, 15, PV_TIER_GOLD },
    { "Comfort Song",           "play one song 25 times",                 PV_AM_TRACK_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Ritual",                 "play one song 40 times",                 PV_AM_TRACK_PLAYS, 0, 40, PV_TIER_GOLD },
    { "Anthem Status",          "play one song 60 times",                 PV_AM_TRACK_PLAYS, 0, 60, PV_TIER_GOLD },
    { "One Hundred Replays",    "play one song 100 times",                PV_AM_TRACK_PLAYS, 0, 100, PV_TIER_GOLD },
    { "It's Still Good",        "play one song 150 times",                PV_AM_TRACK_PLAYS, 0, 150, PV_TIER_GOLD },
    { "The Song Owns You",      "play one song 250 times",                PV_AM_TRACK_PLAYS, 0, 250, PV_TIER_GOLD },
    { "Five Hundred Loops",     "play one song 500 times",                PV_AM_TRACK_PLAYS, 0, 500, PV_TIER_ICE },
    { "Monotrack",              "play one song 1,000 times",              PV_AM_TRACK_PLAYS, 0, 1000, PV_TIER_VIO },
    { "Duran Duran Duran Duran", "play one song 2,500 times",             PV_AM_TRACK_PLAYS, PV_BADGE_SECRET, 2500, PV_TIER_EMBER },
    { "The Loop Consumes",      "play one song 10,000 times",             PV_AM_TRACK_PLAYS, PV_BADGE_SECRET, 10000, PV_TIER_OMEGA },

    /* ---- ARTIST DEVOTION ---- */
    { "Casual Fan",             "play one artist 5 times",                PV_AM_ARTIST_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Repeat Customer",        "play one artist 10 times",               PV_AM_ARTIST_PLAYS, 0, 10, PV_TIER_GOLD },
    { "In the Fan Club",        "play one artist 25 times",               PV_AM_ARTIST_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Merch Table Regular",    "play one artist 50 times",               PV_AM_ARTIST_PLAYS, 0, 50, PV_TIER_GOLD },
    { "Front Row",              "play one artist 75 times",               PV_AM_ARTIST_PLAYS, 0, 75, PV_TIER_GOLD },
    { "Superfan",               "play one artist 100 times",              PV_AM_ARTIST_PLAYS, 0, 100, PV_TIER_GOLD },
    { "Street Team",            "play one artist 150 times",              PV_AM_ARTIST_PLAYS, 0, 150, PV_TIER_GOLD },
    { "Deep Discography",       "play one artist 250 times",              PV_AM_ARTIST_PLAYS, 0, 250, PV_TIER_GOLD },
    { "Certified Stan",         "play one artist 400 times",              PV_AM_ARTIST_PLAYS, 0, 400, PV_TIER_GOLD },
    { "Beyond Stan",            "play one artist 600 times",              PV_AM_ARTIST_PLAYS, 0, 600, PV_TIER_GOLD },
    { "Parasocial",             "play one artist 1,000 times",            PV_AM_ARTIST_PLAYS, 0, 1000, PV_TIER_GOLD },
    { "Their Biggest Fan",      "play one artist 2,500 times",            PV_AM_ARTIST_PLAYS, 0, 2500, PV_TIER_ICE },
    { "Groupie Emeritus",       "play one artist 5,000 times",            PV_AM_ARTIST_PLAYS, 0, 5000, PV_TIER_VIO },
    { "You ARE the Band Now",   "play one artist 20,000 times",           PV_AM_ARTIST_PLAYS, PV_BADGE_SECRET, 20000, PV_TIER_EMBER },

    /* ---- ALBUM DEVOTION ---- */
    { "Front to Back",          "play one album 5 times",                 PV_AM_ALBUM_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Repeat Pressing",        "play one album 10 times",                PV_AM_ALBUM_PLAYS, 0, 10, PV_TIER_GOLD },
    { "Liner Notes Memorized",  "play one album 25 times",                PV_AM_ALBUM_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Album of the Month",     "play one album 50 times",                PV_AM_ALBUM_PLAYS, 0, 50, PV_TIER_GOLD },
    { "Album of the Year",      "play one album 100 times",               PV_AM_ALBUM_PLAYS, 0, 100, PV_TIER_GOLD },
    { "Concept Album Convert",  "play one album 150 times",               PV_AM_ALBUM_PLAYS, 0, 150, PV_TIER_GOLD },
    { "Deluxe Edition",         "play one album 250 times",               PV_AM_ALBUM_PLAYS, 0, 250, PV_TIER_GOLD },
    { "Anniversary Reissue",    "play one album 400 times",               PV_AM_ALBUM_PLAYS, 0, 400, PV_TIER_GOLD },
    { "Box Set Devotee",        "play one album 750 times",               PV_AM_ALBUM_PLAYS, 0, 750, PV_TIER_GOLD },
    { "The Only Album",         "play one album 1,500 times",             PV_AM_ALBUM_PLAYS, 0, 1500, PV_TIER_ICE },
    { "Dark Side of the Loop",  "play one album 5,000 times",             PV_AM_ALBUM_PLAYS, PV_BADGE_SECRET, 5000, PV_TIER_EMBER },

    /* ---- LOYALTY ---- */
    { "No Skips",               "play a never-skipped song 3 times",      PV_AM_LOYAL_PLAYS, 0, 3, PV_TIER_GOLD },
    { "True to You",            "play a never-skipped song 5 times",      PV_AM_LOYAL_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Ride or Die",            "play a never-skipped song 10 times",     PV_AM_LOYAL_PLAYS, 0, 10, PV_TIER_GOLD },
    { "Twenty, No Doubts",      "play a never-skipped song 20 times",     PV_AM_LOYAL_PLAYS, 0, 20, PV_TIER_GOLD },
    { "Unskippable",            "play a never-skipped song 35 times",     PV_AM_LOYAL_PLAYS, 0, 35, PV_TIER_GOLD },
    { "Fifty and Faithful",     "play a never-skipped song 50 times",     PV_AM_LOYAL_PLAYS, 0, 50, PV_TIER_GOLD },
    { "Undying Loyalty",        "play a never-skipped song 100 times",    PV_AM_LOYAL_PLAYS, 0, 100, PV_TIER_GOLD },
    { "Two Hundred, Zero Skips", "play a never-skipped song 200 times",   PV_AM_LOYAL_PLAYS, 0, 200, PV_TIER_GOLD },
    { "Til Battery Do Us Part", "play a never-skipped song 500 times",    PV_AM_LOYAL_PLAYS, PV_BADGE_SECRET, 500, PV_TIER_VIO },

    /* ---- SKIP TARGET ---- */
    { "It's Not You",           "skip the same song 3 times",             PV_AM_TRACK_SKIPS, PV_BADGE_SECRET, 3, PV_TIER_GOLD },
    { "It's Definitely You",    "skip the same song 10 times",            PV_AM_TRACK_SKIPS, PV_BADGE_SECRET, 10, PV_TIER_GOLD },
    { "Why Is It Still Here",   "skip the same song 25 times",            PV_AM_TRACK_SKIPS, PV_BADGE_SECRET, 25, PV_TIER_GOLD },
    { "Delete It Already",      "skip the same song 50 times",            PV_AM_TRACK_SKIPS, PV_BADGE_SECRET, 50, PV_TIER_ICE },
    { "Nemesis Track",          "skip the same song 100 times",           PV_AM_TRACK_SKIPS, PV_BADGE_SECRET, 100, PV_TIER_ICE },

    /* ---- BIG DAYS ---- */
    { "Light Rotation",         "log 5 plays in one day",                 PV_AM_DAY_PLAYS, 0, 5, PV_TIER_GOLD },
    { "Ten a Day",              "log 10 plays in one day",                PV_AM_DAY_PLAYS, 0, 10, PV_TIER_GOLD },
    { "Busy Ears",              "log 25 plays in one day",                PV_AM_DAY_PLAYS, 0, 25, PV_TIER_GOLD },
    { "Power Listener",         "log 40 plays in one day",                PV_AM_DAY_PLAYS, 0, 40, PV_TIER_GOLD },
    { "All-Day Rotation",       "log 60 plays in one day",                PV_AM_DAY_PLAYS, 0, 60, PV_TIER_GOLD },
    { "Ninety in a Day",        "log 90 plays in one day",                PV_AM_DAY_PLAYS, 0, 90, PV_TIER_GOLD },
    { "Track Gatling",          "log 120 plays in one day",               PV_AM_DAY_PLAYS, 0, 120, PV_TIER_GOLD },
    { "Human Radio Station",    "log 160 plays in one day",               PV_AM_DAY_PLAYS, 0, 160, PV_TIER_GOLD },
    { "Playquake",              "log 220 plays in one day",               PV_AM_DAY_PLAYS, 0, 220, PV_TIER_ICE },
    { "Three Hundred Special",  "log 300 plays in one day",               PV_AM_DAY_PLAYS, 0, 300, PV_TIER_ICE },
    { "The Day the Music Won",  "log 500 plays in one day",               PV_AM_DAY_PLAYS, PV_BADGE_SECRET, 500, PV_TIER_VIO },

    /* ---- MARATHON DAYS ---- */
    { "Half Hour Day",          "listen 30 minutes in one day",           PV_AM_DAY_MINS, 0, 30, PV_TIER_GOLD },
    { "Hour Day",               "listen 60 minutes in one day",           PV_AM_DAY_MINS, 0, 60, PV_TIER_GOLD },
    { "Two Hours Deep",         "listen 120 minutes in one day",          PV_AM_DAY_MINS, 0, 120, PV_TIER_GOLD },
    { "Four Hour Block",        "listen 240 minutes in one day",          PV_AM_DAY_MINS, 0, 240, PV_TIER_GOLD },
    { "Six Hour Session",       "listen 360 minutes in one day",          PV_AM_DAY_MINS, 0, 360, PV_TIER_GOLD },
    { "The Full Workday",       "listen 480 minutes in one day",          PV_AM_DAY_MINS, 0, 480, PV_TIER_GOLD },
    { "Ten Hour Odyssey",       "listen 600 minutes in one day",          PV_AM_DAY_MINS, 0, 600, PV_TIER_GOLD },
    { "Half the Day",           "listen 720 minutes in one day",          PV_AM_DAY_MINS, 0, 720, PV_TIER_GOLD },
    { "Nine Hundred Minutes",   "listen 900 minutes in one day",          PV_AM_DAY_MINS, 0, 900, PV_TIER_ICE },
    { "Eighteen Hour Day",      "listen 1,080 minutes in one day",        PV_AM_DAY_MINS, 0, 1080, PV_TIER_ICE },
    { "Twenty Hour Bender",     "listen 1,200 minutes in one day",        PV_AM_DAY_MINS, 0, 1200, PV_TIER_VIO },
    { "Midnight to Midnight",   "listen 1,440 min in one day - all of it", PV_AM_DAY_MINS, PV_BADGE_SECRET, 1440, PV_TIER_OMEGA },

    /* ---- OBSESSION DAYS ---- */
    { "Triple Take",            "play one song 3 times in one day",       PV_AM_TRACK_DAY, 0, 3, PV_TIER_GOLD },
    { "One More Time",          "play one song 5 times in one day",       PV_AM_TRACK_DAY, 0, 5, PV_TIER_GOLD },
    { "Eight Is Enough",        "play one song 8 times in one day",       PV_AM_TRACK_DAY, 0, 8, PV_TIER_GOLD },
    { "Twelve Rounds",          "play one song 12 times in one day",      PV_AM_TRACK_DAY, PV_BADGE_SECRET, 12, PV_TIER_GOLD },
    { "Song Hostage Situation", "play one song 20 times in one day",      PV_AM_TRACK_DAY, PV_BADGE_SECRET, 20, PV_TIER_ICE },
    { "The Wall Is Melting",    "play one song 35 times in one day",      PV_AM_TRACK_DAY, PV_BADGE_SECRET, 35, PV_TIER_VIO },
    { "Sixty Times Today",      "play one song 60 times in one day",      PV_AM_TRACK_DAY, PV_BADGE_SECRET, 60, PV_TIER_VIO },
    { "One Song, One Day, 100x", "play one song 100 times in one day",    PV_AM_TRACK_DAY, PV_BADGE_SECRET, 100, PV_TIER_EMBER },

    /* ---- SKIP SPREE ---- */
    { "Rapid Fire",             "skip 3 tracks in a row",                 PV_AM_SKIP_RUN, PV_BADGE_SECRET, 3, PV_TIER_GOLD },
    { "Nothing Sounds Right",   "skip 5 tracks in a row",                 PV_AM_SKIP_RUN, PV_BADGE_SECRET, 5, PV_TIER_GOLD },
    { "Decision Paralysis",     "skip 10 tracks in a row",                PV_AM_SKIP_RUN, PV_BADGE_SECRET, 10, PV_TIER_GOLD },
    { "Scorched Playlist",      "skip 20 tracks in a row",                PV_AM_SKIP_RUN, PV_BADGE_SECRET, 20, PV_TIER_GOLD },
    { "The Great Purge",        "skip 40 tracks in a row",                PV_AM_SKIP_RUN, PV_BADGE_SECRET, 40, PV_TIER_ICE },
    { "Eighty and Nothing",     "skip 80 tracks in a row - just stop",    PV_AM_SKIP_RUN, PV_BADGE_SECRET, 80, PV_TIER_VIO },

    /* ---- FLOW STATE ---- */
    { "Letting It Ride",        "play 5 tracks without skipping",         PV_AM_PLAY_RUN, 0, 5, PV_TIER_GOLD },
    { "Trust the Queue",        "play 10 tracks without skipping",        PV_AM_PLAY_RUN, 0, 10, PV_TIER_GOLD },
    { "Twenty, No Touching",    "play 20 tracks without skipping",        PV_AM_PLAY_RUN, 0, 20, PV_TIER_GOLD },
    { "Zen Shuffle",            "play 35 tracks without skipping",        PV_AM_PLAY_RUN, 0, 35, PV_TIER_GOLD },
    { "Fifty Straight",         "play 50 tracks without skipping",        PV_AM_PLAY_RUN, 0, 50, PV_TIER_GOLD },
    { "Deep Flow",              "play 75 tracks without skipping",        PV_AM_PLAY_RUN, 0, 75, PV_TIER_GOLD },
    { "Hundred Track Trance",   "play 100 tracks without skipping",       PV_AM_PLAY_RUN, 0, 100, PV_TIER_GOLD },
    { "The Long Haul",          "play 150 tracks without skipping",       PV_AM_PLAY_RUN, 0, 150, PV_TIER_GOLD },
    { "Playlist Pacifist",      "play 250 tracks without skipping",       PV_AM_PLAY_RUN, 0, 250, PV_TIER_ICE },
    { "Five Hundred Serene",    "play 500 tracks without skipping",       PV_AM_PLAY_RUN, 0, 500, PV_TIER_ICE },
    { "The Skip Button Rusts",  "play 1,000 tracks without skipping",     PV_AM_PLAY_RUN, PV_BADGE_SECRET, 1000, PV_TIER_VIO },

    /* ---- BIG WEEKS ---- */
    { "Weekly Warmup",          "listen 60 minutes in one week",          PV_AM_WEEK_MINS, 0, 60, PV_TIER_GOLD },
    { "Three Hour Week",        "listen 180 minutes in one week",         PV_AM_WEEK_MINS, 0, 180, PV_TIER_GOLD },
    { "An Hour a Day",          "listen 420 minutes in one week",         PV_AM_WEEK_MINS, 0, 420, PV_TIER_GOLD },
    { "Two a Day",              "listen 840 minutes in one week",         PV_AM_WEEK_MINS, 0, 840, PV_TIER_GOLD },
    { "SUPERWEEK",              "listen 1,440 minutes in one week",       PV_AM_WEEK_MINS, 0, 1440, PV_TIER_GOLD },
    { "Beyond Super",           "listen 2,016 minutes in one week",       PV_AM_WEEK_MINS, 0, 2016, PV_TIER_GOLD },
    { "ULTRAWEEK",              "listen 2,880 minutes in one week",       PV_AM_WEEK_MINS, 0, 2880, PV_TIER_ICE },
    { "HYPERWEEK",              "listen 4,320 minutes in one week",       PV_AM_WEEK_MINS, 0, 4320, PV_TIER_VIO },
    { "GIGAWEEK",               "listen 5,760 minutes in one week",       PV_AM_WEEK_MINS, 0, 5760, PV_TIER_EMBER },
    { "OMEGAWEEK",              "listen 7,200 minutes in one week",       PV_AM_WEEK_MINS, 0, 7200, PV_TIER_OMEGA },
    { "Sleepless in Stereo",    "listen 8,640 minutes in one week",       PV_AM_WEEK_MINS, PV_BADGE_SECRET, 8640, PV_TIER_OMEGA },
    { "THE ETERNAL WEEK",       "listen 10,080 minutes in one week - all", PV_AM_WEEK_MINS, PV_BADGE_SECRET, 10080, PV_TIER_OMEGA },

    /* ---- SUPERWEEKS ---- */
    { "First Superweek",        "log 1 superweek (1,440+ min)",           PV_AM_SUPERWEEKS, 0, 1, PV_TIER_GOLD },
    { "Super Again",            "log 2 superweeks (1,440+ min)",          PV_AM_SUPERWEEKS, 0, 2, PV_TIER_GOLD },
    { "Super Trilogy",          "log 3 superweeks (1,440+ min)",          PV_AM_SUPERWEEKS, 0, 3, PV_TIER_GOLD },
    { "Five Supers",            "log 5 superweeks (1,440+ min)",          PV_AM_SUPERWEEKS, 0, 5, PV_TIER_GOLD },
    { "Super Eight",            "log 8 superweeks (1,440+ min)",          PV_AM_SUPERWEEKS, 0, 8, PV_TIER_GOLD },
    { "Twelve Weeks of Power",  "log 12 superweeks (1,440+ min)",         PV_AM_SUPERWEEKS, 0, 12, PV_TIER_GOLD },
    { "Twenty Supers",          "log 20 superweeks (1,440+ min)",         PV_AM_SUPERWEEKS, 0, 20, PV_TIER_GOLD },
    { "Thirty Supers",          "log 30 superweeks (1,440+ min)",         PV_AM_SUPERWEEKS, 0, 30, PV_TIER_ICE },
    { "A Super Year",           "log 52 superweeks (1,440+ min)",         PV_AM_SUPERWEEKS, 0, 52, PV_TIER_VIO },
    { "Two Years, All Super",   "log 104 superweeks (1,440+ min)",        PV_AM_SUPERWEEKS, PV_BADGE_SECRET, 104, PV_TIER_EMBER },

    /* ---- ULTRAWEEKS ---- */
    { "First Ultraweek",        "log 1 ultraweek (2,880+ min)",           PV_AM_ULTRAWEEKS, 0, 1, PV_TIER_GOLD },
    { "Ultra Again",            "log 2 ultraweeks (2,880+ min)",          PV_AM_ULTRAWEEKS, 0, 2, PV_TIER_GOLD },
    { "Ultra Trilogy",          "log 3 ultraweeks (2,880+ min)",          PV_AM_ULTRAWEEKS, 0, 3, PV_TIER_GOLD },
    { "Penta Ultra",            "log 5 ultraweeks (2,880+ min)",          PV_AM_ULTRAWEEKS, 0, 5, PV_TIER_GOLD },
    { "Deca Ultra",             "log 10 ultraweeks (2,880+ min)",         PV_AM_ULTRAWEEKS, 0, 10, PV_TIER_ICE },
    { "Twenty Ultras",          "log 20 ultraweeks (2,880+ min)",         PV_AM_ULTRAWEEKS, 0, 20, PV_TIER_VIO },
    { "The Ultra Year",         "log 52 ultraweeks (2,880+ min)",         PV_AM_ULTRAWEEKS, PV_BADGE_SECRET, 52, PV_TIER_EMBER },

    /* ---- CONSISTENCY ---- */
    { "One Good Week",          "log 1 active week (30+ min)",            PV_AM_ACTIVE_WEEKS, 0, 1, PV_TIER_GOLD },
    { "Week Two",               "log 2 active weeks (30+ min)",           PV_AM_ACTIVE_WEEKS, 0, 2, PV_TIER_GOLD },
    { "One Month Steady",       "log 4 active weeks (30+ min)",           PV_AM_ACTIVE_WEEKS, 0, 4, PV_TIER_GOLD },
    { "Two Months Running",     "log 8 active weeks (30+ min)",           PV_AM_ACTIVE_WEEKS, 0, 8, PV_TIER_GOLD },
    { "A Season of Sound",      "log 13 active weeks (30+ min)",          PV_AM_ACTIVE_WEEKS, 0, 13, PV_TIER_GOLD },
    { "Half Year Regular",      "log 26 active weeks (30+ min)",          PV_AM_ACTIVE_WEEKS, 0, 26, PV_TIER_GOLD },
    { "Three Quarters In",      "log 39 active weeks (30+ min)",          PV_AM_ACTIVE_WEEKS, 0, 39, PV_TIER_GOLD },
    { "The Whole Year",         "log 52 active weeks (30+ min)",          PV_AM_ACTIVE_WEEKS, 0, 52, PV_TIER_GOLD },
    { "Eighteen Months",        "log 78 active weeks (30+ min)",          PV_AM_ACTIVE_WEEKS, 0, 78, PV_TIER_GOLD },
    { "Two Year Regular",       "log 104 active weeks (30+ min)",         PV_AM_ACTIVE_WEEKS, 0, 104, PV_TIER_GOLD },
    { "Three Year Fixture",     "log 156 active weeks (30+ min)",         PV_AM_ACTIVE_WEEKS, 0, 156, PV_TIER_ICE },
    { "Five Year Plan",         "log 260 active weeks (30+ min)",         PV_AM_ACTIVE_WEEKS, 0, 260, PV_TIER_VIO },
    { "Decade Subscriber",      "log 520 active weeks (30+ min)",         PV_AM_ACTIVE_WEEKS, PV_BADGE_SECRET, 520, PV_TIER_EMBER },

    /* ---- PERFECT MONTHS ---- */
    { "Perfect Month",          "1 month with every single day active",   PV_AM_PERFECT_MONTHS, 0, 1, PV_TIER_GOLD },
    { "Perfect Pair",           "2 months with every single day active",  PV_AM_PERFECT_MONTHS, 0, 2, PV_TIER_GOLD },
    { "Perfect Quarter",        "3 months with every single day active",  PV_AM_PERFECT_MONTHS, 0, 3, PV_TIER_GOLD },
    { "Four Flawless",          "4 months with every single day active",  PV_AM_PERFECT_MONTHS, 0, 4, PV_TIER_GOLD },
    { "Half Year of Halos",     "6 months with every single day active",  PV_AM_PERFECT_MONTHS, 0, 6, PV_TIER_ICE },
    { "Nine Perfect Months",    "9 months with every single day active",  PV_AM_PERFECT_MONTHS, 0, 9, PV_TIER_ICE },
    { "The Perfect Year",       "12 months with every single day active", PV_AM_PERFECT_MONTHS, 0, 12, PV_TIER_VIO },
    { "Eighteen Immaculate",    "18 months with every single day active", PV_AM_PERFECT_MONTHS, 0, 18, PV_TIER_EMBER },
    { "Two Years Untouched",    "24 months with every single day active", PV_AM_PERFECT_MONTHS, 0, 24, PV_TIER_EMBER },
    { "Four Years, No Rest",    "48 months with every single day active", PV_AM_PERFECT_MONTHS, PV_BADGE_SECRET, 48, PV_TIER_OMEGA },

    /* ---- RED LETTER DAYS ---- */
    { "Auld Lang Sync",         "play music during 00:00-00:59 on jan 1", PV_AM_F_NEWYEAR,   PV_BADGE_SECRET, 1, PV_TIER_ICE },
    { "Last Christmas",         "play music on december 25",              PV_AM_F_XMAS,      PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "Spooky Scary Playback",  "play music on october 31",               PV_AM_F_HALLOWEEN, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "Unlucky Groove",         "play music on a friday the 13th",        PV_AM_F_FRIDAY13,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "Stroke of Midnight",     "log a play in a day's first minute",     PV_AM_F_MIDNIGHT,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "Bonus Level",            "play music on february 29",              PV_AM_F_LEAPDAY,   PV_BADGE_SECRET, 1, PV_TIER_ICE },

    /* ---- ??? ---- */

    /* ---- LISTENING TYPES (secret; profile re-typed after every play
     *      once past 100 plays, so types passed through still count) ---- */
    { "The Free Spirit",        "be typed as the free spirit",            PV_AM_TYPE0,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Night Scout",        "be typed as the night scout",            PV_AM_TYPE1,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Adventurer",         "be typed as the adventurer",             PV_AM_TYPE2,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Midnight Wanderer",  "be typed as the midnight wanderer",      PV_AM_TYPE3,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Channel Hopper",     "be typed as the channel hopper",         PV_AM_TYPE4,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Midnight Zapper",    "be typed as the midnight zapper",        PV_AM_TYPE5,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Comfort Curator",    "be typed as the comfort curator",        PV_AM_TYPE6,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Night Ritualist",    "be typed as the night ritualist",        PV_AM_TYPE7,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Catalog Sifter",     "be typed as the catalog sifter",         PV_AM_TYPE8,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Night Miner",        "be typed as the night miner",            PV_AM_TYPE9,  PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Completionist",      "be typed as the completionist",          PV_AM_TYPE10, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Deep Diver",         "be typed as the deep diver",             PV_AM_TYPE11, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Picky Superfan",     "be typed as the picky superfan",         PV_AM_TYPE12, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Restless Fan",       "be typed as the restless fan",           PV_AM_TYPE13, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The True Believer",      "be typed as the true believer",          PV_AM_TYPE14, PV_BADGE_SECRET, 1, PV_TIER_GOLD },
    { "The Midnight Devotee",   "be typed as the midnight devotee",       PV_AM_TYPE15, PV_BADGE_SECRET, 1, PV_TIER_GOLD },

    /* ---- TYPE COLLECTOR ---- */
    { "Two-Faced",              "collect 2 listening types",              PV_AM_TYPES_COLLECTED, 0, 2, PV_TIER_GOLD },
    { "Phase Shifter",          "collect 4 listening types",              PV_AM_TYPES_COLLECTED, 0, 4, PV_TIER_GOLD },
    { "Mood Ring",              "collect 8 listening types",              PV_AM_TYPES_COLLECTED, 0, 8, PV_TIER_GOLD },
    { "Identity Crisis",        "collect 12 listening types",             PV_AM_TYPES_COLLECTED, 0, 12, PV_TIER_ICE },
    { "All Of Them At Once",    "collect all 16 listening types",         PV_AM_TYPES_COLLECTED, 0, 16, PV_TIER_VIO },

#ifdef ACH_PERSONAL
    /* one personal badge, nobody else's business - see ach_personal.h
       (untracked; public builds simply do not have this row) */
    ACH_PERSONAL_ROW
#endif

    /* ---- META ---- */
    { "Achievement Achieved",   "unlock 10 other achievements",           PV_AM_META, 0, 10, PV_TIER_GOLD },
    { "Plaque Collector",       "unlock 25 other achievements",           PV_AM_META, 0, 25, PV_TIER_GOLD },
    { "Fifty Trophies",         "unlock 50 other achievements",           PV_AM_META, 0, 50, PV_TIER_GOLD },
    { "Wall of Fame",           "unlock 100 other achievements",          PV_AM_META, 0, 100, PV_TIER_GOLD },
    { "Cabinet Overflow",       "unlock 150 other achievements",          PV_AM_META, 0, 150, PV_TIER_GOLD },
    { "Two Hundred Medals",     "unlock 200 other achievements",          PV_AM_META, 0, 200, PV_TIER_ICE },
    { "Completionist Rising",   "unlock 250 other achievements",          PV_AM_META, 0, 250, PV_TIER_VIO },
    { "All This For Music",     "unlock 280 other achievements",          PV_AM_META, 0, 280, PV_TIER_EMBER },

    /* ---- Saved progress keys on TABLE ORDER: a badge is a bit at its own
     * index, so inserting a row anywhere but the end silently reassigns
     * every unlock after it. Append below this line, never insert above,
     * and if a row ever must move, bump the save format's magic and remap
     * on load rather than hoping nobody notices. ---- */

};

#endif /* _PV_BADGES_TABLE_H */
