#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#
# The plugin system has been removed from this fork. This file builds nothing.
#
# It survives only because tools/root.make:135 `include`s it by exact path, and
# a missing (non-optional) include is a fatal make error. tools/ is outside the
# scope this fork's cleanup is confined to, so the file stays.
#
# The credits.raw rule that used to live here moved to apps/apps.make -- it was
# never plugin-specific, and keeping it here tied it to ENABLEDPLUGINS=yes.
#
# See plugins/README.md for the rest of this directory.

ROCKS :=
