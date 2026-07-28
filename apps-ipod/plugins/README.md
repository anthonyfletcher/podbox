# `apps-ipod/plugins/` — contains no plugin, and no plugin code

The plugin system was removed from this fork. The `rb->` API struct, the `.rock`
loader, `plugin_crt0.c`, the plugin lib and every plugin source are gone. What is
left here is here because something **outside** `apps-ipod/` names these exact
paths, and this fork's cleanup work is confined to `apps-ipod/`.

Do not "tidy up" this directory without reading what each file does first.

## Still live

### `plugin.lds` — **not scaffolding**

`lib/rbcodec/codecs/codecs.make` sets:

```make
CODEC_LDS := $(APPSDIR)/plugins/plugin.lds
```

Codecs and plugins shared one linker script upstream. The plugins went away; the
codecs did not. This script still links every `.codec` in the build. Delete or
move it and codec linking breaks — and it breaks **late**, at package time, not
at compile time.

### `credits.pl`

Generates `credits.raw` from `docs/CREDITS` for the core credits screen
(`viewers/credits.c`). 

## Inert, but required to exist

`plugins.make` and `bitmaps/pluginbitmaps.make` are included by
`tools/root.make` at exact paths under `$(APPSDIR)`. A missing `include` (as
opposed to `-include`) is a fatal make error. Neither file builds anything.

## Inert, and no longer reached

`viewers.config`, `CATEGORIES` and `rockbox-fonts.config` are copies of files
`tools/buildzip.pl` still needs — but buildzip opens them under `$ROOT/apps/`,
the unbuilt upstream mirror, **not** `$APPSDIR`. Only `tagnavi.config` and
`lang/Invalid*.talk` follow `$APPSDIR`. So `make zip` reads
`apps/plugins/viewers.config` and `apps/plugins/CATEGORIES` (both with `or die`,
so a missing one fails the zip) and never looks in this directory.
