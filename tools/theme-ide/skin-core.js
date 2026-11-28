/* PodBox skin core: the skin reader and pixel renderer that Theme Lens and the
   web simulator share. A classic script, not a module, so it also loads from a
   file:// page; everything it declares is a global.

   Parts, in order:
     - tag tables, the firmware's own lists, the scanner and the model build
     - fonts (.fnt), bitmaps (.bmp) and tag evaluation
     - argument specs, which the linter in build() reads
     - the pixel surface, %Cl's filter chain and colour parsing
     - renderSkin(), one skin file for one state */
"use strict";

/* ============================================================
   Tag tables.
   Mirrors lib/skin_parser/tag_table.c (upstream) and
   apps-ipod/skin/custom_tags.c (this fork's additions).
   [params, description, argNames]
   ============================================================ */

const UPSTREAM = {
  ac:["","Align the rest of the line to the centre"],
  al:["","Align the rest of the line left"],
  aL:["","Align left, flipping to right in RTL languages"],
  ar:["","Align the rest of the line right"],
  aR:["","Align right, flipping to left in RTL languages"],
  ax:["","Align following the language's reading direction"],
  if:["TS[ITS]|D","Compare a tag against a value",["tag","operator","value","limit"]],
  and:["T*","True only if every argument is true"],
  or:["T*","True if any argument is true"],

  fb:["","Bitrate of the current file"],
  fc:["","Codec of the current file"],
  ff:["","Sample frequency in Hz"],
  fk:["","Sample frequency in kHz"],
  fm:["","File name with extension"],
  fn:["","File name without extension"],
  fp:["","Full file path"],
  fs:["","File size in kB"],
  fv:["","Set when the file is variable bitrate"],
  d:["I","Directory name, n levels above the file",["levels"]],
  ft:["F|[is]","Text read from a file, following a key",["file","key"]],
  Fb:["","Next track: bitrate"], Fc:["","Next track: codec"],
  Ff:["","Next track: frequency in Hz"], Fk:["","Next track: frequency in kHz"],
  Fm:["","Next track: file name with extension"], Fn:["","Next track: file name"],
  Fp:["","Next track: full path"], Fs:["","Next track: file size"],
  Fv:["","Next track: set when variable bitrate"],
  D:["I","Next track: directory name n levels up",["levels"]],

  xl:["SF|[IP][IP]I","Preload a bitmap so it can be drawn later",["id","file","x","y","frames"]],
  xd:["S|[IT]I","Draw a preloaded bitmap",["id","frame"]],
  X:["f","Set the backdrop image for the screen",["file"]],
  x:["SF|II","Load and immediately draw a bitmap",["id","file","x","y"]],
  x9:["S","Draw a nine-segment stretched bitmap",["id"]],

  Vd:["S","Show a conditional viewport declared by %Vl",["label"]],
  VI:["S","Show a list viewport declared by %Vi",["label"]],
  Vp:["IC","Draw a custom list in this viewport",["mode","code"]],
  Vf:["s","Set this viewport's foreground colour",["colour"]],
  Vb:["s","Set this viewport's background colour",["colour"]],
  Vs:["S|s","Set the text style (invert, colour, gradient…)",["style","colour"]],
  Vg:["SS|s","Set up a gradient for the list selector",["start","end","text"]],
  Vy:["|i","Set the box a line of text sits in — bare means the viewport's own height",["height"]],
  Vt:["s|iiii","Drop shadow behind this viewport's text — '-' turns it off",["colour","x","y","blur","opacity"]],
  VB:["","Draw this viewport onto the backdrop layer"],
  Vl:["S[IP][IP][ip][ip]i","Declare a conditional viewport — hidden until %Vd names it",["label","x","y","width","height","font"]],
  Vi:["s[IP][IP][ip][ip]i","Declare a list (UI) viewport, shown by %VI",["label","x","y","width","height","font"]],
  V:["[IP][IP][ip][ip]i","Declare a viewport — always drawn",["x","y","width","height","font"]],

  bl:["?[iP][iP][iP][iP]|s*","Battery level, or a battery bar with coordinates",["x","y","width","height"]],
  bv:["","Battery voltage"], bt:["","Estimated battery time left"],
  bs:["","Time left on the sleep timer"], bsp:["","Sleep timer left, 0–100 of the length it started from"], bc:["","Set while charging"],
  bp:["","Set while a charger is connected"], bu:["","Set while USB is connected"],

  cc:["","Set when the player has a real-time clock"],
  cd:["","Day of month, zero padded"], ce:["","Day of month, space padded"],
  cf:["","Set when the clock is configured for 12-hour time"],
  cH:["","Hour, 24h, zero padded"], ck:["","Hour, 24h"],
  cI:["","Hour, 12h, zero padded"], cl:["","Hour, 12h"],
  cm:["","Month number"], cM:["","Minute"], cS:["","Second"],
  cy:["","Year, 2 digits"], cY:["","Year, 4 digits"],
  cP:["","AM/PM, uppercase"], cp:["","am/pm, lowercase"],
  ca:["","Weekday name"], cb:["","Month name"],
  cu:["","Day of week, 1 = Monday"], cw:["","Day of week, 0 = Sunday"],

  ia:["","Artist"], ic:["","Composer"], id:["","Album"], iA:["","Album artist"],
  iG:["","Grouping"], ig:["","Genre"], ik:["","Disc number"], in:["","Track number"],
  it:["","Track title"], iv:["","ID3 version"], iy:["","Year"], iC:["","Comment"],
  Ia:["","Next track: artist"], Ic:["","Next track: composer"], Id:["","Next track: album"],
  IA:["","Next track: album artist"], IG:["","Next track: grouping"], Ig:["","Next track: genre"],
  Ik:["","Next track: disc number"], In:["","Next track: track number"],
  It:["","Next track: title"], Iv:["","Next track: ID3 version"],
  Iy:["","Next track: year"], IC:["","Next track: comment"],

  Sp:["","Playback pitch"], Ss:["","Playback speed"],
  lh:["","Flickers while the disk is active"],

  mh:["","Set while the hold switch is on"],
  mr:["","Set while the remote hold switch is on"],
  mm:["","Repeat mode"], mp:["","Playback status"],
  mv:["|D","Set briefly after the volume buttons are used",["timeout"]],

  pm:["","Peak meter"], pL:["?[iP][iP][iP][iP]|s*","Left channel peak meter",["x","y","width","height"]],
  pR:["?[iP][iP][iP][iP]|s*","Right channel peak meter",["x","y","width","height"]],
  pf:["","Full-width progress bar"],
  pb:["?[iP][iP][iP][iP]|s*","Track progress bar",["x","y","width","height"]],
  pv:["?[iP][iP][iP][iP]|s*","Volume bar",["x","y","width","height"]],
  px:["","Percentage played through the track"],
  pc:["","Time elapsed"], pr:["","Time remaining"], pt:["","Track length"],
  prb:["","Time left in the playing audiobook, at Spoken Word Speed; empty for anything else"],
  pbp:["","How much of the playing audiobook is heard, 0–100; empty for anything else"],
  Pt:["","Next track: length"],
  pS:["|D","Set for n seconds at the start of a track",["seconds"]],
  pE:["|D","Set for the last n seconds of a track",["seconds"]],
  pp:["","Position in the playlist"], pe:["","Number of entries in the playlist"],
  pP:["?[iP][iP][iP][iP]|s*","Playlist progress bar",["x","y","width","height"]],
  pn:["","Playlist name"], ps:["","Set while shuffle is on"],
  pX:["?[iP][iP][iP][iP]|s*","Playlist elapsed bar",["x","y","width","height"]],

  rp:["","Play count from the database"], rr:["","Rating from the database"],
  ra:["","Auto score from the database"],
  rg:["","Replaygain value in use"], xf:["","Set while crossfade is enabled"],

  tp:["","Set when the player has a tuner"], tt:["","Set when the station is tuned"],
  tm:["","Set while in scan mode"], ts:["","Set when receiving stereo"],
  ta:["","Lowest tunable frequency"], tb:["","Highest tunable frequency"],
  tf:["","Current frequency"],
  tr:["?[iP][iP][iP][iP]|s*","Signal strength bar",["x","y","width","height"]],
  tl:["","Minimum signal strength"], th:["","Maximum signal strength"],
  Ti:["","Preset number"], Tn:["","Preset name"], Tf:["","Preset frequency"],
  Tc:["","Number of presets"],
  tx:["","Set when RDS is available"], ty:["","RDS station name"], tz:["","RDS text"],

  s:["","Scroll this line if it is too wide"],
  t:["D|d","How long this subline stays on screen",["seconds"]],

  we:["","Re-enable the theme's own drawing"],
  wd:["","Disable the built-in status bar"],
  wi:["","Draw the built-in status bar here"],

  Fl:["IF|I","Load a font under an id that %V and %Vl can select",["id","file","glyphs"]],
  /* This fork overrides %Cl with three more arguments — a filter chain, a
     corner radius, and the label a %Cd calls it by — and %Cd with a label plus
     an optional window. A skin may declare several %Cl; what costs memory is
     the art SIZE, not the tag, because slots are claimed by dimension and
     shared within and across skins. */
  Cl:["[iP][iP][iP][iP]|sssis","Claim an album art slot, set its box, and optionally filter, round and name it",
      ["x","y","width","height","halign","valign","filters","radius","label"]],
  Cd:["|s[ip][ip][ip][ip]si","Draw an album art — the one named, or the nearest %Cl above it — optionally through a window onto it, or as its reflection: %Cd(name,-,-,-,-,reflect,strength) draws the bottom rows upside down from the top of this viewport, fading from strength per cent (default 35)",
      ["label","window x","window y","window width","window height","reflect","strength"]],
  C:["","Set when album art was found for this track"],

  Lt:["","Title of the current list"],
  LT:["|IS","Text of a list row",["offset","nowrap"]],
  LR:["","Row number within a multi-column list"],
  LC:["","Column number within a multi-column list"],
  LN:["","Index of the row being drawn"],
  Li:["","Icon of the list title"],
  LI:["|IS","Icon of a list row",["offset","nowrap"]],
  Lb:["Sii|si","Draw list rows using the viewports under this label, at this row size",["label","width","height","tile","art height"]],
  Ld:["","True while the list on screen draws art rows"],
  LD:["","The selected podcast episode's description; a space on a list of episodes where it has none; empty on any other list and on one whose episodes have no notes"],
  Lm:["|IS","A row of a list that is selecting: m ticked, u not; empty for a row that cannot be ticked and on any other list. A skin that uses it gets no [x] in the row text",["offset","nowrap"]],
  LH:["|S","The list's header: bare, its kind (album, on an album's tracks) or empty; with a field, that field: name, artist, year, tracks, length, discs, or art (the cover, filling the viewport)",["field"]],
  Lc:["","Set while drawing the selected row"],
  Lk:["|IS","Kind of a list row: 0 plain, 1 menu, 2 toggle, 3 setting, 4 action, 5 command, 6 container, 7 track, 8 file, 9 option, 10 custom, 11 custom menu",["offset","nowrap"]],
  Lp:["|IS","Number of a row among the list's tracks, or its disc's on an album of several, counted from the end on a podcast's newest-first lists; empty on other rows",["offset","nowrap"]],
  Ls:["|IS","Disc of a track row on an album whose tracks are on more than one; empty on other rows and lists",["offset","nowrap"]],
  Lv:["|IS","Current value of a setting row",["offset","nowrap"]],
  Lr:["|IS","Where a number setting sits in its range, or how far through a book or podcast episode, 0-100; empty on other rows",["offset","nowrap"]],
  LP:["|IS","True on the row that is playing",["offset","nowrap"]],
  LB:["?[iP][iP][iP][iP]|s*","Set when the list needs a scrollbar",["x","y","width","height"]],

  QT:["","Name of the top quickscreen setting"], Qt:["","Value of the top quickscreen setting"],
  QR:["","Name of the right quickscreen setting"], Qr:["","Value of the right quickscreen setting"],
  QB:["","Name of the bottom quickscreen setting"], Qb:["","Value of the bottom quickscreen setting"],
  QL:["","Name of the left quickscreen setting"], Ql:["","Value of the left quickscreen setting"],

  St:["[Sip]|[ip][ip][ip]s*","Value of a Rockbox setting, by name",["setting"]],
  Sx:["S","A string translated into the current language",["string"]],
  Sr:["","Set when the language reads right to left"],

  Tl:["|[SD]D","Set for n seconds after a button press or wheel turn (default 10)",["seconds"]],
  T:["[Sip][ip][ip][ip][Sip]|S*","Declare a touch region",["x","y","width","height","action"]],
  Tp:["","Set when the player has a touchscreen"],

  cs:["","Which screen is currently showing"],

  Rp:["","Set when the player can record"], Rr:["","Set while recording"],
  Rf:["","Recording frequency"], Re:["","Recording encoder"], Rb:["","Recording bitrate"],
  Rm:["","Set when recording in mono"], Rs:["","Recording seconds"],
  Rn:["","Recording minutes"], Rh:["","Recording hours"],

  vs:["SSi|I","Set a skin variable",["name","operator","value","max"]],
  vg:["S","Read a skin variable",["name"]],
  vl:["S|D","Set while a skin variable's timeout is running",["name","seconds"]],

  ss:["IiT|s","A slice of another tag's text",["start","length","tag","ellipsis"]],
  /* This fork overrides %dr with an opacity and a corner radius. */
  dr:["[IP][IP][ip][ip]|ssii","Draw a filled rectangle, optionally tinted and rounded",
      ["x","y","width","height","colour","colour2","opacity","radius"]],
};

/* This fork's tags — apps-ipod/skin/custom_tags.c */
const CUSTOM = {
  lb:["","Set while the database or album-art cache is building"],
  lw:["","Set while the firmware's generic 'working' flag is on"],
  la:["","Spinner frame index — a full turn takes about a second"],
  Sb:["i|siiisii","Draw a spectrum analyser filling this viewport",["bars","align","radius","gap","peak","peakcolour","segment","segmentgap"]],
  La:["|Isis","Album art for a list row",["offset","nowrap","radius","filters"]],
  tw:["T|i","Pixel width of text — pass a font id when measuring outside its viewport",["text","fontid"]],
  Vw:["","Width of the current viewport, in pixels"],
  Vh:["","Height of the current viewport, in pixels"],
  sel:["T[ITS]*","Pick a value by matching a subject against keys",["subject","key/value…"]],
  wr:["I[ITS]","The nth word-wrapped line of text, wrapped to this viewport",["line","text"]],
  ma:["[IT]S[IT]","Integer arithmetic: a op b",["a","operator","b"]],
  sl:["[ITS]","Number of characters in text",["text"]],
  sf:["[ITS][ITS]","Index of a substring, or -1 when absent",["haystack","needle"]],
  pd:["I[ITS]","Pad or truncate text to exactly n characters",["width","text"]],
  trm:["[ITS]S","Text with the given characters stripped from both ends",["text","characters"]],
  wt:["T|ST","Draw wrapped, aligned text filling this viewport",["text","align","fallback"]],
};

/* Enumerated *arguments*, keyed "tag:index". Values transcribed from the
   parsers in apps-ipod/skin/skin_parser.c. */
const ARG_ENUMS = {
  /* parse_albumart_load: only the first character is read, case-insensitive,
     and anything unrecognised leaves the default in place. */
  "Cl:4": { firstChar: true, dflt: "centre", vals: { l: "left", c: "centre", r: "right" } },
  "Cl:5": { firstChar: true, dflt: "centre", vals: { t: "top", c: "centre", b: "bottom" } },

  /* parse_viewporttextstyle */
  "Vs:0": { vals: {
    invert: "swap foreground and background for the line",
    color: "colour the text — the next argument is the colour",
    colour: "colour the text — the next argument is the colour",
    clear: "back to the viewport's own style",
    gradient: "gradient over n lines — the next argument is n",
  } },

  /* parse_skinvar */
  "vs:1": { vals: {
    set: "assign the value", inc: "add to the variable",
    dec: "subtract from the variable", touch: "set while the region is touched",
  } },

  "Lb:3": { vals: { tile: "lay rows out as a grid instead of full-width rows" } },
  "Sb:1": { vals: { center: "grow the bars from the middle, not up from the bottom",
                    radiate: "open out from the middle: bass at the centre, treble at both edges, one stereo channel per half" } },
  "ma:1": { vals: { "+": "add", "-": "subtract", "*": "multiply",
                    "/": "divide, truncated (÷0 gives 0)", "%": "remainder (mod 0 gives 0)" } },
  "if:1": { vals: { "=": "equal to", "==": "equal to", "!=": "not equal to",
                    ">": "greater than", "<": "less than",
                    ">=": "greater than or equal to", "<=": "less than or equal to" } },
  "sf:1": { }, /* plain string needle */
  "LT:1": { vals: { nowrap: "stop the offset wrapping round the ends of the list" } },
  "LI:1": { vals: { nowrap: "stop the offset wrapping round the ends of the list" } },
  "La:1": { vals: { nowrap: "stop the offset wrapping round the ends of the list" } },
  "wt:1": { align2: true },
};

/* ---------- %Cl's filter chain ----------
   apps-ipod/draw/img_filter.c. Names join with '+'; an amount is written
   straight after the name — no brackets, no spaces — and only '-' can
   introduce one, because '+' is already the separator. A misspelt name is a
   parse error, so it fails when the theme loads rather than drawing nothing. */
const IMG_FILTERS = {
  invert:     { amt: "none",                          what: "complement every channel" },
  brightness: { amt: "required", min: -100, max: 100, what: "toward white, or toward black when negative" },
  lighter:    { amt: "adaptive", min: 0,   max: 100,  what: "brighten — with no amount, to a fixed mean brightness, so it adapts to the artwork" },
  darker:     { amt: "adaptive", min: 0,   max: 100,  what: "darken — with no amount, to a fixed mean brightness, so it adapts to the artwork" },
  scrim:      { amt: "none",                          what: "darken until the text over it reads, adapting to the artwork and the palette's text colour; scrim:rrggbb names that colour" },
  contrast:   { amt: "required", min: -100, max: 100, what: "away from mid-grey, or toward it when negative" },
  reduce:     { amt: "default",  min: 2,   max: 256, def: 4,   what: "posterise to n levels per channel" },
  bw:         { amt: "default",  min: 0,   max: 100, def: 100, what: "toward greyscale" },
  saturate:   { amt: "required", min: -100, max: 100, what: "saturate; saturate-100 is the same as bw" },
  hue:        { amt: "required", min: 0,   max: 359,  what: "rotate the hue, in degrees" },
  dither:     { amt: "none",                          what: "ordered 8×8 Bayer" },
  pixellate:  { amt: "default",  min: 2,   max: 64,  def: 8,   what: "block average; the amount is the block edge in pixels" },
  blur:       { amt: "default",  min: 1,   max: 16,  def: 4,   what: "box blur; the amount is the window on a decimated copy" },
};
const IMG_FILTER_MAX_CHAIN = 8;

/* Mirrors img_filter_compile(). Returns the steps and, if it would not compile
   on the device, the message it would fail with. */
function parseFilterChain(spec) {
  const out = { steps: [], error: null };
  const text = String(spec === undefined || spec === null ? "" : spec).trim();
  if (!text || text === "-") return out;

  let p = 0;
  while (p < text.length) {
    const from = p;
    while (p < text.length && /[A-Za-z]/.test(text[p])) p++;
    const name = text.slice(from, p);
    if (!name) { out.error = "expected a filter name"; return out; }

    let negative = false, digits = "";
    if (text[p] === "-") { negative = true; p++; }
    while (p < text.length && /[0-9]/.test(text[p])) digits += text[p++];
    if (negative && !digits) { out.error = '"' + name + '": expected an amount after the minus'; return out; }

    /* `scrim:rrggbb` names the text colour, as img_filter_compile() reads it. */
    let textLum = null;
    if (text[p] === ":") {
      const hex = text.slice(p + 1, p + 7);
      if (!/^[0-9A-Fa-f]{6}$/.test(hex)) { out.error = '"' + name + '": expected rrggbb after the colon'; return out; }
      const n = parseInt(hex, 16);
      textLum = ((n >> 16) * 77 + ((n >> 8) & 255) * 150 + (n & 255) * 29) >> 8;
      p += 7;
    }

    if (text[p] === "+") {
      p++;
      if (p >= text.length) { out.error = "the chain ends with a '+'"; return out; }
    } else if (p < text.length) {
      out.error = '"' + name + '": unexpected ' + JSON.stringify(text.slice(p)) +
                  " after the filter — the chain joins with '+' and takes no spaces";
      return out;
    }

    const def = IMG_FILTERS[name];
    if (!def) { out.error = '"' + name + '" is not a filter'; return out; }
    if (textLum !== null && name !== "scrim") { out.error = '"' + name + '" takes no colour'; return out; }

    let amount = digits === "" ? null : (negative ? -parseInt(digits, 10) : parseInt(digits, 10));
    let adaptive = false;
    if (def.amt === "none" && amount !== null) { out.error = '"' + name + '" takes no amount'; return out; }
    if (def.amt === "required" && amount === null) { out.error = '"' + name + '" needs an amount'; return out; }
    if (def.amt === "default" && amount === null) amount = def.def;
    if (def.amt === "adaptive" && amount === null) adaptive = true;
    if (!adaptive && def.amt !== "none" && (amount < def.min || amount > def.max)) {
      out.error = '"' + name + '" takes ' + def.min + "…" + def.max + ", not " + amount;
      return out;
    }
    if (out.steps.length === IMG_FILTER_MAX_CHAIN) {
      out.error = "more than " + IMG_FILTER_MAX_CHAIN + " filters in one chain";
      return out;
    }
    out.steps.push({ name, amount, adaptive, textLum });
  }
  return out;
}

/* ---------- what each argument is ----------
   The name alone says "label" or "offset"; this says what the parser does with
   it, which is usually the part that catches you out. Keyed "tag:index".
   Transcribed from .claude/skills/skin-debugging/references/skin-tag-reference.md and the parsers it
   cites. Anything not listed here falls back to the argument's name. */
const ARG_HELP = {
  /* viewports */
  "V:0": "x. Negative counts back from the right edge; a percentage is thousandths of the screen width.",
  "V:1": "y. Negative counts back from the bottom edge.",
  "V:2": "width. '-' fills the rest of the screen from x.",
  "V:3": "height. '-' fills the rest of the screen from y.",
  "V:4": "font id — 0 is sysfixed, 1 the user's UI font, 2 and up are ids %Fl has loaded.",
  "Vl:0": "label. %Vd draws this viewport by name; %Lb hands every viewport sharing the label to the list as one row layout.",
  "Vl:1": "x — inside the row, not on the screen, when a %Lb names this label.",
  "Vl:2": "y — inside the row, not on the screen, when a %Lb names this label.",
  "Vl:3": "width. '-' runs to the right edge.",
  "Vl:4": "height. '-' runs to the bottom edge.",
  "Vl:5": "font id.",
  "Vi:0": "label, or '-' for the default list area. %VI switches the list to it.",
  "Vi:1": "x.", "Vi:2": "y.",
  "Vi:3": "width. '-' runs to the right edge.",
  "Vi:4": "height. '-' runs to the bottom edge.",
  "Vi:5": "font id.",
  "Vd:0": "the %Vl label to draw now. It acts by side effect and returns no text, so it cannot be a %sel value.",
  "VI:0": "the %Vi label the list should draw in.",
  "Vs:0": "style: invert, color, gradient or clear.",
  "Vs:1": "the colour for 'color', or the number of lines for 'gradient'.",
  "Vg:0": "gradient start colour.", "Vg:1": "gradient end colour.", "Vg:2": "text colour over the gradient.",
  "Vf:0": "foreground colour, RRGGBB, or !RRGGBB to fix it against dynamic colours. On a viewport's own declaration line this becomes the viewport's starting colour; below it, it is a render-time instruction.",
  "Vb:0": "background colour, RRGGBB.",
  "Vy:0": "the height of the line box: what a line of text is centred in, and what %Vs(invert) fills. Bare or '-' means the viewport's own height. Left off entirely it is the font height, so a viewport taller than its font draws text at the top and fills only part of it. It does not change the step between lines.",
  "Vt:0": "shadow colour, RRGGBB, or '-' to switch the shadow off from here down.",
  "Vt:1": "horizontal offset in pixels, -8 to 8. Default 1.",
  "Vt:2": "vertical offset in pixels, -8 to 8. Default 1.",
  "Vt:3": "blur radius in pixels, 0 to 3. Default 2; 0 gives a hard edge.",
  "Vt:4": "opacity, 0 invisible to 15 solid. Default 15. The shadow is clipped to the viewport but not to the line box, so leave the line room (%Vy) for the offset plus the blur.",
  "Vp:0": "playlist entry offset.", "Vp:1": "the skin code drawn for each entry.",

  /* fonts and images */
  "Fl:0": "font id, 2 or above — 0 and 1 are the built-in and UI fonts.",
  "Fl:1": "the .fnt file. The name must end in .fnt: a .font name fails the check and the whole skin is discarded, with nothing on screen to say so.",
  "Fl:2": "how many glyphs to keep cached.",
  "xl:0": "label, used by %xd.",
  "xl:1": "the .bmp file. The literal __list_icons__ preloads the user's configured iconset instead.",
  "xl:2": "x.", "xl:3": "y.",
  "xl:4": "number of frames stacked in the file — %xd then picks one.",
  "xd:0": "the %xl label. With a single two-character argument the second character picks the subimage: %xd(Aa) is subimage 'a' of image A.",
  "xd:1": "frame number, 1-based.",
  "x:0": "label.", "x:1": "the .bmp file.", "x:2": "x.", "x:3": "y.",
  "X:0": "the backdrop image for the whole screen.",
  "x9:0": "the %xl label, drawn stretched as nine segments — corners fixed, edges and middle stretched.",

  /* album art */
  "Cl:0": "x of the art box.", "Cl:1": "y of the art box.",
  "Cl:2": "maximum width — the art is fitted inside, keeping its aspect.",
  "Cl:3": "maximum height.",
  "Cl:4": "horizontal alignment inside the box. Only the first character is read.",
  "Cl:5": "vertical alignment inside the box. Only the first character is read.",
  "Cl:6": "filter chain, this fork's addition. Names join with '+', an amount is written straight after the name, and there are no spaces anywhere. A misspelt name fails the skin at load rather than drawing unfiltered.",

  /* drawing */
  "dr:0": "x.", "dr:1": "y.",
  "dr:2": "width. '-' runs to the viewport's right edge.",
  "dr:3": "height. '-' runs to the viewport's bottom edge.",
  "dr:4": "fill colour, RRGGBB. '-' means the viewport foreground as it stood when the parser reached this line — write the colour out instead.",
  "dr:5": "gradient end colour. '-' means no gradient, and a gradient loses to both an opacity and a radius.",
  "dr:6": "opacity, 0 (invisible) to 15 (opaque). This fork's addition; absent means opaque, so every %dr written before it behaves as it always did.",
  "dr:7": "corner radius in pixels, 0–32. This fork's addition.",

  /* lists */
  "Lb:0": "the %Vl label whose viewports draw one row. Every %Vl sharing it is redrawn once per visible row.",
  "Lb:1": "row width — the grid cell width when tiling.",
  "Lb:2": "row height. This is the step between rows, which a row viewport shorter or taller than it does not show on its own.",
  "Lb:3": "'tile' lays the rows out as a grid instead of full-width rows.",
  "Lb:4": "row height to use when the list draws art. It has to be at least the row height above: the two are compared to decide whether the list draws art rows at all, so a smaller one leaves it drawing plain rows with nothing to say why. Equal is fine, and is what makes one row's cover meet the next with no gap. Omit it and the 'database art row height' setting answers instead; write the tile argument as '-' to reach it.",
  "LT:0": "row offset from the one being drawn; 0 is that row.",
  "LT:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "Lk:0": "row offset; 0 is the row being drawn.",
  "Lk:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "Lp:0": "row offset; 0 is the row being drawn.",
  "Lp:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "Ls:0": "row offset; 0 is the row being drawn.",
  "Ls:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "Lv:0": "row offset; 0 is the row being drawn.",
  "Lv:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "Lr:0": "row offset; 0 is the row being drawn.",
  "Lr:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "LP:0": "row offset; 0 is the row being drawn.",
  "LP:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "LI:0": "row offset; 0 is the row being drawn.",
  "LI:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "La:0": "row offset; 0 is the row being drawn.",
  "La:1": "'nowrap' stops the offset wrapping round the ends of the list.",
  "La:2": "corner radius in pixels. Write the wrap argument as '-' to reach it.",
  "La:3": "filter chain for the cover, joined by '+' (bw, invert, brightness25, contrast40, saturate50, hue180, reduce4, dither, pixellate8). No blur. The chain is this row config's and reaches nothing else.",

  /* widgets */
  "Sb:0": "number of bands, 1–8. Outside that it is clamped, not refused. With 'radiate' it counts the bands on each side, so the meter is twice as many columns wide.",
  "Sb:1": "'center' grows the bars from the middle rather than up from the bottom; 'radiate' also opens the layout out from the centre, bass in the middle to treble at both edges, with the left half fed from the left channel.",
  "Sb:2": "corner radius, fitted to each bar — there is no upper limit, so a short bar rounds less than a tall one.",
  "Sb:3": "gap between bars in pixels. 0 butts them into a solid block.",
  "Sb:4": "thickness in pixels of the falling peak cap each bar carries. Left out, or 0, there are no caps. The cap rides the bar up, holds for half a second, then floats down until the bar catches it. A centred or radiating bar gets a cap at each end.",
  "Sb:5": "colour of the peak caps. Left out they take the viewport foreground, the same as the bars. Follows the dynamic-colour palette like any other skin colour; '!rrggbb' pins it.",
  "Sb:6": "segment height in pixels. Left out, or 0, the bars are continuous. Each bar shows whole segments counted up from the baseline (out from the middle when centred), so its top always lands on a segment boundary, and the caps snap to the same grid.",
  "Sb:7": "pixels between segments; 1 if left out. Nothing is drawn in the gaps, so what is behind the viewport shows through.",

  /* strings and arithmetic */
  "tw:0": "the text to measure.",
  "tw:1": "font id to measure in. Without it the measurement is made in the current viewport's font, which is the wrong one whenever you are choosing a layout before that viewport is in effect.",
  "wr:0": "line number, 0-based. Text that already fits lands wholly on line 0, so lines 1 and up come back empty.",
  "wr:1": "the text, wrapped to the current viewport's width in its font.",
  "wt:0": "the text, wrapped to the viewport and ellipsised if it is taller than fits -- or, with %s on the same line in a .wps, scrolled up through it on the player.",
  "wt:1": "alignment: vertical t/c/b then horizontal l/c/r. Default tl.",
  "wt:2": "drawn instead when the text is empty — the filename for a track with no title, say.",
  "sel:0": "the subject, evaluated once and compared against each key as text.",
  "ma:0": "left operand — a number or a tag.",
  "ma:1": "operator. Written in a string argument, so a literal % has to be doubled: %ma(%cS,%%,4).",
  "ma:2": "right operand. Dividing or taking a remainder by zero gives 0.",
  "sl:0": "the text, counted in characters rather than bytes.",
  "sf:0": "the text to search.", "sf:1": "the text to find. The result is a 0-based index, or -1.",
  "pd:0": "column width — the text is padded with trailing spaces or truncated to exactly this many characters.",
  "pd:1": "the text.",
  "trm:0": "the text.",
  "trm:1": "every character to strip from the start and the end; the middle is left alone.",
  "ss:0": "start, a literal integer — it cannot be computed.",
  "ss:1": "length, a literal integer.",
  "ss:2": "the tag to slice.", "ss:3": "ellipsis to append when it was truncated.",

  /* settings, files, variables */
  "St:0": "the setting's config-file name, as it appears in config.cfg.",
  "Sx:0": "the phrase's english.lang source string. It resolves through langs/overrides.cfg, so it keeps matching a renamed phrase — the row reading 'Preferences' is %Sx(Settings).",
  "ft:0": "the file to read.",
  "ft:1": "the key. Whitespace around the key, and between it and its value, is now trimmed — %ft(prefs, mode:) and %ft(prefs, mode: ) behave identically.",
  "d:0": "how many directories up from the file, 1 being the one it is in.",
  "vs:0": "variable name.",
  "vs:1": "what to do: set, inc, dec, or touch.",
  "vs:2": "the value — a literal integer, not a tag, so a computed value cannot be stored.",
  "vs:3": "the value it wraps at.",
  "vg:0": "variable name.",
  "vl:0": "variable name.", "vl:1": "timeout in seconds.",
  "if:0": "the tag to test. A tag yielding text compares as 'non-empty = 1', which silently makes %?if(%in,<,10) true for every track — compare %sl(%in) instead.",
  "if:1": "comparison operator.", "if:2": "the value to compare against.",
  "t:0": "how long this subline stays on screen, in seconds.",
  "mv:0": "how long after the volume buttons are used this stays set, in seconds.",
  "pS:0": "seconds from the start of the track.", "pE:0": "seconds before the end of the track.",
  "T:4": "the action fired when the region is touched.",
};

/* Trailing options accepted by every bar-style tag (BAR_PARAMS "…|s*"). */
const BAR_OPTIONS = {
  invert: "fill from the opposite end",
  nofill: "draw the border but no fill",
  noborder: "draw the fill but no border",
  nobar: "draw neither — just the slider or image",
  slider: "use a bitmap slider; the next argument names the image",
  image: "use a bitmap for the fill; the next argument names the image",
  backdrop: "use a bitmap behind the bar; the next argument names the image",
  vertical: "grow vertically", horizontal: "grow horizontally",
  notouch: "do not accept touches",
  setting: "bind to a setting; the next argument names it",
  soffset: "offset within that setting",
  hold: "peak meter bars (%pL, %pR): fall back slowly and mark the held peak; an optional number after it is the mark's thickness in pixels",
};

/* Every tag using BAR_PARAMS — x,y,w,h then any number of option words. */
const BAR_PARAM_TAGS = new Set(["bl", "pb", "pv", "pP", "pX", "St", "tr", "pL", "pR", "LB"]);

/* Arguments that are colours rather than values. */
const COLOUR_ARGS = new Set(["Vf:0", "Vb:0", "Vg:0", "Vg:1", "Vg:2", "dr:4", "dr:5", "Vs:1"]);

/* Enumerations, so %?tag<a|b|c> can be read back in words. */
const ENUMS = {
  mp: ["stopped","playing","paused","fast forward","rewind"],
  mm: ["repeat off","repeat all","repeat one","shuffle","repeat A-B"],
  rr: ["0","1","2","3","4","5","6","7","8","9","10"],
  cs: ["unknown","main menu","WPS","recording","FM radio","playlist viewer","settings",
       "file browser","database browser","plugin browser","quickscreen","(reserved)",
       "option select","playlist browser","plugin","context menu","system screen",
       "time/date screen","bookmarks list","shortcuts menu","track info","USB screen",
       "album covers","text viewer","image viewer","folder select","album charts",
       "document browser","image browser","database search","lyrics"],
};

/* Generated by extract-lists.py from the firmware sources.
   Re-run it rather than editing this by hand:
     python .build/theme-lens/extract-lists.py --embed skin-core.js
   67 lists, 383 rows. Entries with f:1 are hand-written fixtures — the
   browsers build their lists from the disk, so there is nothing in the
   source to read; only their icon ids come from icon.h. */
const FIRMWARE_LISTS = [{"s":1,"n":"Main menu","t":"Rockbox","ti":31,"f":0,"r":["Resume Playback|25","Continue Reading|6","Music|0","Album Covers|1","Artist Portraits|1","Random Album|2","Queue|2","Playlists|2","Documents|6","Images|6","Playback Report|0","Files|29","Shortcuts|10","Settings|20","System|24"],"k":[4,4,4,4,4,4,4,4,4,4,4,4,4,4,4],"v":[null,null,null,null,null,null,null,null,null,null,null,null,null,null,null]},{"s":5,"n":"Playlist viewer","t":"Current Playlist","ti":2,"f":1,"r":["01 Nightcall|0","02 Odd Look|0","03 Protovision|12","04 Rampage|0","05 Blizzard|13"]},{"s":6,"n":"Accessories","t":"Accessories","ti":-1,"f":0,"r":["Line Out|-1","Identify As|-1","Accessory Browsing|-1","Accessory Artwork|-1"],"k":[2,3,3,3],"v":["lineout_active","iap_identity","iap_browse_size","car_artwork"]},{"s":6,"n":"Activity Indicators","t":"Activity Indicators","ti":-1,"f":0,"r":["Show Disk Activity|-1","Show Background Tasks|-1","Show Working|-1"],"k":[2,2,2],"v":["show_disk_activity","show_background_tasks","show_working"]},{"s":6,"n":"Albums By","t":"Albums By","ti":-1,"f":0,"r":["All Albums|-1","Artist|-1","Album Artist|-1","Composer|-1","Other Lists|-1"],"k":[4,4,4,4,3],"v":[null,null,null,null,"database_sort_albums_by"]},{"s":6,"n":"Appearance","t":"Appearance","ti":4,"f":0,"r":["Load Theme|8","Font|6","Skins|4","Interface Colours|26","Interface Elements|26","Now Playing Screen|-1","Carousel|-1","Dialogs|26","Scrolling|-1","Viewers|-1","Peak Meter|-1","Menus|-1","Forget My Changes|-1"],"k":[4,4,1,1,1,1,1,1,1,1,1,1,4],"v":[null,null,null,null,null,null,null,null,null,null,null,null,null]},{"s":6,"n":"Art Cache","t":"Art Cache","ti":-1,"f":0,"r":["Album Art Source|-1","Fast Build|-1","Missing Album Artwork|-1","Missing Artist Portraits|-1"],"k":[3,2,4,4],"v":["art_cache_album_source","art_cache_fast_build",null,null]},{"s":6,"n":"Automatic Resume","t":"Automatic Resume","ti":-1,"f":0,"r":["Automatic Resume|-1","Resume on Automatic Track Change|-1"],"k":[2,3],"v":["autoresume_enable","autoresume_automatic"]},{"s":6,"n":"Backlight","t":"Backlight","ti":-1,"f":0,"r":["First Button Press|-1","Exemptions|-1"],"k":[2,1],"v":["bl_filter_first_keypress",null]},{"s":6,"n":"Battery & Power","t":"Battery & Power","ti":24,"f":0,"r":["Brightness|-1","Backlight|-1","Idle Poweroff|-1","Sleep Timer|-1","Storage|-1","Charge During USB Connection|-1"],"k":[3,1,3,1,1,3],"v":["brightness",null,null,null,null,"usb_charging"]},{"s":6,"n":"Bookmarking","t":"Bookmarking","ti":10,"f":0,"r":["Bookmark on Stop|-1","Update on Stop|-1","Load Last Bookmark|-1","Maintain a List of Recent Bookmarks?|-1"],"k":[3,2,3,3],"v":["autocreatebookmark","autoupdatebookmark","autoloadbookmark","usemrb"]},{"s":6,"n":"Carousel","t":"Carousel","ti":-1,"f":0,"r":["Sort|-1","On Album Select|-1","Scroll Speed|-1","Random Spin Length|-1","Appearance|-1"],"k":[1,3,3,3,1],"v":[null,"album_covers_on_select","album_covers_scroll_speed","album_covers_random_spin",null]},{"s":6,"n":"Crossfade","t":"Crossfade","ti":-1,"f":0,"r":["Enable Crossfade|-1","Fade-In Delay|-1","Fade-In Duration|-1","Fade-Out Delay|-1","Fade-Out Duration|-1","Fade-Out Mode|-1"],"k":[3,3,3,3,3,3],"v":["crossfade","crossfade_fade_in_delay","crossfade_fade_in_duration","crossfade_fade_out_delay","crossfade_fade_out_duration","crossfade_fade_out_mixmode"]},{"s":6,"n":"Crossfeed","t":"Crossfeed","ti":-1,"f":0,"r":["Crossfeed|-1"],"k":[3],"v":["crossfeed"]},{"s":6,"n":"Database","t":"Database","ti":-1,"f":0,"r":["Scan on Startup|-1","Scan on Eject|-1","Select Directories to Scan|-1","Year From Folder Name|-1","Export Modifications|-1","Import Modifications|-1"],"k":[2,2,4,2,4,4],"v":["tagcache_scan_on_startup","tagcache_scan_on_eject",null,"year_from_folder",null,null]},{"s":6,"n":"Debug Logs","t":"Debug Logs","ti":-1,"f":0,"r":["Database Log|-1","Album Art Log|-1","Playback Error Log|-1","Theme Log|-1","Background Task Log|-1","Switch Off After|-1","Turn All Off|-1"],"k":[2,2,2,2,2,3,4],"v":["debug_log_tagcache","debug_log_artcache","debug_log_playback","debug_log_skin","debug_log_tasks","debug_log_expiry",null]},{"s":6,"n":"Dialogs","t":"Dialogs","ti":26,"f":0,"r":["Box Border Width|-1","Box Margin|-1","Box Shadow|-1","Box Shadow Colour|-1","Button Border Width|-1","Button Corner Radius|-1","Dialog Colour Mode|-1","Box Text|-1","Box Background|-1","Box Border|-1","Button Text|-1","Button Background|-1","Button Border|-1","Selected Button Text|-1","Selected Button Background|-1","Selected Button Border|-1"],"k":[3,3,3,4,3,3,3,4,4,4,4,4,4,4,4,4],"v":["dialog_box_border_width","dialog_box_margin","dialog_box_shadow",null,"dialog_btn_border_width","dialog_btn_border_radius","dialog_colors",null,null,null,null,null,null,null,null,null]},{"s":6,"n":"Equalizer","t":"Equalizer","ti":30,"f":0,"r":["Enable EQ|-1","Edit EQ|30","EQ Presets|-1"],"k":[2,4,4],"v":["eq_enabled",null,null]},{"s":6,"n":"Exemptions","t":"Exemptions","ti":-1,"f":0,"r":["Enabled|-1","Settings|-1"],"k":[2,4],"v":["bl_selective_actions",null]},{"s":6,"n":"Fast-Forward/Rewind","t":"Fast-Forward/Rewind","ti":-1,"f":0,"r":["Rewind Across Tracks|-1","Rewind Before Resume|-1","Rewind on Pause|-1"],"k":[2,3,3],"v":["rewind_across_tracks",null,null]},{"s":6,"n":"Files","t":"Files","ti":29,"f":0,"r":["Sort|-1","Show Files|-1","Show Filename Extensions|-1","Follow Playlist|-1","Show Path|-1","Directory Cache|-1","Start File Browser at /|29"],"k":[1,3,3,2,3,2,4],"v":[null,"dirfilter","show_filename_ext","browse_current","show_path_in_browser","dircache",null]},{"s":6,"n":"Haas Surround","t":"Haas Surround","ti":-1,"f":0,"r":["Haas Surround|-1","Side Only|-1"],"k":[3,2],"v":[null,"surround_method2"]},{"s":6,"n":"Interface Colours","t":"Interface Colours","ti":26,"f":0,"r":["Dynamic Colours|-1","Dynamic Colours Background|-1","Foreground Colour|-1","Background Colour|-1","Line Selector Colours|-1","Line Separator Colour|-1","Reset Colours|-1"],"k":[2,3,4,4,1,4,4],"v":["dynamic_colors","dynamic_colors_background",null,null,null,null,null]},{"s":6,"n":"Interface Elements","t":"Interface Elements","ti":26,"f":0,"r":["Art Rows|-1","Show Icons|-1","Status Bar|-1","Scroll Bar|-1","Battery Display|-1","Volume Display|-1","Activity Indicators|-1","Line Selector Type|-1","Line Separator|-1"],"k":[1,2,3,1,3,3,1,3,3],"v":[null,"playlist_viewer_icons","statusbar",null,"battery_display","volume_type",null,"cursor_style",null]},{"s":6,"n":"Keyclick","t":"Keyclick","ti":-1,"f":0,"r":["Headphone Keyclick|-1","Speaker Keyclick|-1","Keyclick Repeats|-1"],"k":[3,2,2],"v":["keyclick","keyclick_hardware","keyclick_repeats"]},{"s":6,"n":"Language & Text","t":"Language & Text","ti":7,"f":0,"r":["Language|7","Voice|22","Default Codepage|-1"],"k":[4,1,3],"v":[null,null,"default_codepage"]},{"s":6,"n":"Library","t":"Library","ti":2,"f":0,"r":["Files|29","Music|-1","Carousel|-1","Database|-1","Art Cache|-1","Playlist Engine|2","Viewers|-1","Maintenance|-1"],"k":[1,1,1,1,1,1,1,1],"v":[null,null,null,null,null,null,null,null]},{"s":6,"n":"Limits","t":"Limits","ti":-1,"f":0,"r":["Max Entries in File Browser|-1","Max Playlist Size|-1","Glyphs to Cache|-1"],"k":[3,3,3],"v":["max_files_in_dir",null,"glyphs_to_cache"]},{"s":6,"n":"Line Selector Colours","t":"Line Selector Colours","ti":-1,"f":0,"r":["Primary Colour|-1","Secondary Colour|-1","Text Colour|-1"],"k":[4,4,4],"v":[null,null,null]},{"s":6,"n":"Lyrics Viewer","t":"Lyrics Viewer","ti":-1,"f":0,"r":["Colour Mode|-1","Alignment|-1","Line Spacing|-1","Previous Line|-1","Next Line|-1","Animation|-1","Highlight Sung Words|-1","Keep Backlight On|-1","Font|6","Use UI Font|6"],"k":[3,3,3,3,3,3,2,2,4,4],"v":["lyric_colour_mode","lyric_align","lyric_line_spacing","lyric_prev_opacity","lyric_next_opacity","lyric_anim","lyric_highlight","lyric_backlight",null,null]},{"s":6,"n":"Maintenance","t":"Maintenance","ti":-1,"f":0,"r":["Update Sound Analysis|-1","Rebuild Playback Report|-1","Rebuild Sound Analysis|-1"],"k":[4,4,4],"v":[null,null,null]},{"s":6,"n":"Manage Settings","t":"Manage Settings","ti":8,"f":0,"r":["Settings Mode|-1","Browse .cfg Files|-1","Reset Settings|-1","Save .cfg File|-1","Save Sound Settings|-1","Save Theme Settings|-1"],"k":[3,4,4,4,4,4],"v":["settings_mode",null,null,null,null,null]},{"s":6,"n":"Menus","t":"Menus","ti":-1,"f":0,"r":["Edit Main Menu|-1","Edit Music Menu|-1","Context Menus|-1"],"k":[4,4,1],"v":[null,null,null]},{"s":6,"n":"Music","t":"Music","ti":-1,"f":0,"r":["Sort|-1","Show Year in Album Lists|-1","Featured Artists|-1","Segregate Audiobooks|-1","Search|-1","Quiz Questions|-1","Appearance|-1"],"k":[1,3,2,2,1,1,1],"v":[null,"album_show_year","featured_artists","segregate_audiobooks",null,null,null]},{"s":6,"n":"Now Playing Screen","t":"Now Playing Screen","ti":25,"f":0,"r":["Default Browser|-1","Select Action|-1","Appearance|-1"],"k":[3,3,1],"v":["browser_default","wps_select_action",null]},{"s":6,"n":"Pause on Unplug","t":"Pause on Unplug","ti":-1,"f":0,"r":["Pause on Unplug|-1","Disable resume on startup if phones unplugged|-1"],"k":[3,2],"v":["unplug_mode","unplug_autoresume"]},{"s":6,"n":"Peak Meter","t":"Peak Meter","ti":-1,"f":0,"r":["Peak Release|-1","Peak Hold Time|-1","Scale|-1","Minimum of Range|-1","Maximum of Range|-1"],"k":[3,3,4,4,4],"v":[null,null,null,null,null]},{"s":6,"n":"Playback Report","t":"Playback Report","ti":-1,"f":0,"r":["Artwork|-1","Top Rows|-1","Achievement Order|-1","Ranked By|-1"],"k":[2,3,3,3],"v":["spun_artwork","spun_top_count","spun_badge_order","spun_rank_by"]},{"s":6,"n":"Playback Settings","t":"Playback Settings","ti":25,"f":0,"r":["Shuffle|-1","Repeat|-1","Play Selected First|-1","Fast-Forward/Rewind|-1","Anti-Skip Buffer|-1","Fade on Stop/Pause|-1","Single Mode|-1","Crossfade|-1","Replaygain|-1","Track Skip Beep|-1","Auto-Change Directory|-1","Cuesheet Support|-1","Chapter Marks|-1","Pause on Unplug|-1","Prevent Track Skipping|-1","Frequency|-1","Logging|-1","Playlists|2","Bookmarking|10","Automatic Resume|-1","Now Playing Screen|25"],"k":[2,3,2,1,3,2,3,1,1,3,3,2,2,1,2,3,3,1,1,1,1],"v":["playlist_shuffle","repeat_mode","play_selected",null,null,"fade_on_stop","single_mode",null,null,"beep","next_folder","cuesheet","chapter_marks",null,"prevent_skip",null,"playback_log",null,null,null,null]},{"s":6,"n":"Playlist Engine","t":"Playlist Engine","ti":2,"f":0,"r":["Enabled|-1","Analysis Depth|-1","Playlist Length|-1","Track Playlist|-1","Mood Playlist|-1","Play Selected First|-1","Continue Playing|-1","Library Sound|-1"],"k":[2,3,3,3,3,2,2,4],"v":["playlist_engine","analysis_depth","mix_length","track_playlist","mood_playlist","mix_starts_with_selected","continue_playing",null]},{"s":6,"n":"Playlist Viewer Settings","t":"Playlist Viewer Settings","ti":2,"f":0,"r":["Show Icons|-1","Show Indices|-1","Track Display|-1"],"k":[2,2,3],"v":["playlist_viewer_icons","playlist_viewer_indices","playlist_viewer_track_display"]},{"s":6,"n":"Playlists","t":"Playlists","ti":2,"f":0,"r":["Sort Playlists|-1","Playlist Viewer Settings|2","Recursively Insert Directories|-1","Queue|2"],"k":[3,1,3,1],"v":["sort_playlists",null,"recursive_dir_insert",null]},{"s":6,"n":"Queue","t":"Queue","ti":2,"f":0,"r":["Warn When Replacing Queue|-1","Party Mode|-1","Keep Current Track When Replacing Playlist|-1","Show Shuffled Adding Options|-1"],"k":[2,2,2,2],"v":["warnon_erase_dynplaylist","party_mode","keep_current_track_on_replace_playlist","show_shuffled_adding_options"]},{"s":6,"n":"Quiz Questions","t":"Quiz Questions","ti":-1,"f":0,"r":["Name the Track|-1","Name the Artist|-1","Name the Album|-1","Name the Year|-1"],"k":[2,2,2,2],"v":["quiz_title","quiz_artist","quiz_album","quiz_year"]},{"s":6,"n":"Scroll Bar","t":"Scroll Bar","ti":-1,"f":0,"r":["Scroll Bar|-1","Scroll Bar Width|-1"],"k":[3,3],"v":["scrollbar",null]},{"s":6,"n":"Scrolling","t":"Scrolling","ti":-1,"f":0,"r":["Enabled|-1","Scroll Speed|-1","Scroll Start Delay|-1","Scroll Step Size|-1","Bidirectional Scroll Limit|-1","Screen Scrolls Out of View|-1","Disable Main Menu Scrolling|-1","Screen Scroll Step Size|-1","Paged Scrolling|-1","List Wraparound|-1","List Order|-1"],"k":[2,3,3,3,3,2,2,3,2,2,3],"v":["scrolling_enabled","scroll_speed",null,"scroll_step","bidir_limit","offset_out_of_view","disable_mainmenu_scrolling","screen_scroll_step","scroll_paginated","list_wraparound","list_order"]},{"s":6,"n":"Search","t":"Search","ti":-1,"f":0,"r":["Maximum Results|-1","Minimum Letters|-1","Result Order|-1"],"k":[3,3,3],"v":["db_search_max_rows","db_search_min_letters","db_search_order"]},{"s":6,"n":"Settings","t":"Settings","ti":20,"f":0,"r":["Search|-1","Sound Settings|0","Playback Settings|25","Library|2","Appearance|4","Battery & Power|24","System|24","Settings Mode|-1","Changed Settings|-1"],"k":[4,1,1,1,1,1,1,3,4],"v":[null,null,null,null,null,null,null,"settings_mode",null]},{"s":6,"n":"Skins","t":"Skins","ti":4,"f":0,"r":["While Playing Screen|4","Base Skin|4","Clear Backdrop|-1"],"k":[4,4,4],"v":[null,null,null]},{"s":6,"n":"Sleep Timer","t":"Sleep Timer","ti":-1,"f":0,"r":["Default Sleep Timer Duration|-1","Start Sleep Timer on Boot|-1","Restart Sleep Timer on Keypress|-1"],"k":[3,2,2],"v":["sleeptimer_duration","sleeptimer_on_startup","keypress_restarts_sleeptimer"]},{"s":6,"n":"Sort","t":"Sort","ti":-1,"f":0,"r":["Albums By|-1"],"k":[1],"v":[null]},{"s":6,"n":"Sound Settings","t":"Sound Settings","ti":0,"f":0,"r":["Maximum Volume Limit|-1","Bass|-1","Bass Cutoff|-1","Treble|-1","Treble Cutoff|-1","Balance|-1","Channel Configuration|-1","Stereo Width|-1","Crossfeed|-1","Equalizer|30","Dithering|-1","Haas Surround|-1","Perceptual Bass Enhancement|-1","Auditory Fatigue Reduction|-1","Compressor|-1"],"k":[3,3,3,3,3,3,3,3,1,1,2,1,1,3,1],"v":[null,"bass","bass_cutoff","treble","treble_cutoff","balance","channel_config","stereo_width",null,null,"dithering_enabled",null,null,"afr_enabled",null]},{"s":6,"n":"Startup/Shutdown","t":"Startup/Shutdown","ti":24,"f":0,"r":["Show Shutdown Message|-1","Start Screen|-1","Shut Down|-1","Reboot|-1"],"k":[2,3,4,4],"v":["show_shutdown_message","start_in_screen",null,null]},{"s":6,"n":"Storage","t":"Storage","ti":-1,"f":0,"r":["Disk Spindown|-1"],"k":[3],"v":["disk_spindown"]},{"s":6,"n":"System","t":"System","ti":24,"f":0,"r":["USB|-1","Accessories|-1","Quick Screen|-1","Keyclick|-1","Limits|-1","Volume Adjustment Mode|-1","Time & Date Settings|-1","Startup/Shutdown|24","Language & Text|7","Manage Settings|8","Debug Logs|-1","Show Debug Menu|-1"],"k":[1,1,2,1,1,3,4,1,1,1,1,2],"v":[null,null,"shortcuts_replaces_qs",null,null,"volume_adjust_mode",null,null,null,null,null,"show_debug_menu"]},{"s":6,"n":"Text Viewer","t":"Text Viewer","ti":-1,"f":0,"r":["Colour Mode|-1","Margin|-1","Line Spacing|-1","Page Number|-1","Read Aloud|-1","Font|6","Use UI Font|6"],"k":[3,2,3,2,3,4,4],"v":["text_viewer_colour_mode","text_viewer_margin","text_viewer_line_spacing","text_viewer_page_number","text_viewer_read_aloud",null,null]},{"s":6,"n":"USB","t":"USB","ti":-1,"f":0,"r":["USB Mode|-1","USB HID|-1","USB Keypad Mode|-1"],"k":[3,2,3],"v":["usb_mode","usb_hid","usb_keypad_mode"]},{"s":6,"n":"Viewers","t":"Viewers","ti":-1,"f":0,"r":["Text Viewer|-1","Lyrics Viewer|-1","Playback Report|-1"],"k":[1,1,1],"v":[null,null,null]},{"s":6,"n":"Voice","t":"Voice","ti":22,"f":0,"r":["Speak Aloud|-1","Voice|-1","Speech Rate|-1","Voice Directories|-1","Use Directory .talk Clips|-1","Voice Filenames|-1","Use File .talk Clips|-1","Say File Type|-1","Say List Titles|-1","Announce Track Change|-1","Announce Battery Level|-1","Voice Prompt Volume|-1"],"k":[2,3,3,3,2,3,2,2,2,3,2,3],"v":["talk_menu","talk_voice","talk_speech_rate","talk_dir","talk_dir_clip","talk_file","talk_file_clip","talk_filetype","talk_list_title","talk_track_change","talk_battery_level","talk_mixer_amp"]},{"s":7,"n":"File browser","t":"Files","ti":29,"f":1,"r":["Albums|1","Playlists|1","01 Nightcall.flac|0","02 Odd Look.flac|0","Nightcall.m3u8|2","liner-notes.txt|33","cover.jpg|34","themify_2.cfg|8","22-Literata.fnt|6","resume.bmark|10"]},{"s":8,"n":"Database — albums","t":"Album","ti":0,"f":1,"r":["OutRun|0","Kavinsky EP|0","Nightcall EP|0","Reborn|0","Drive (Original Motion Picture Soundtrack)|0"]},{"s":8,"n":"Database — top","t":"Music","ti":0,"f":1,"r":["Artist|0","Album Artist|0","Album|0","Genre|0","Composer|0","Year|0","Track|0","Search by...|0","Recently Added|0","User Rating|0"]},{"s":15,"n":"Context menu","t":"Context Menu","ti":19,"f":1,"r":["Playlist|2","Add to Shortcuts|10","Open With|9","Delete|-1","Rename|-1","Set As Backdrop|4","Properties|-1"]},{"s":16,"n":"System","t":"System","ti":24,"f":0,"r":["About|-1","System Info|-1","Credits|-1","Background Tasks|-1","License|-1","Debug|-1"],"k":[4,4,4,4,4,4],"v":[null,null,null,null,null,null]},{"s":19,"n":"Shortcuts","t":"Shortcuts","ti":10,"f":1,"r":["Music|1","Shuffle Songs|2","Sleep Timer|17","Brightness|17"]},{"s":22,"n":"Album covers","t":"Album Covers","ti":1,"f":1,"r":["OutRun — Kavinsky|0","Reborn — Kavinsky|0","Drive — Various Artists|0","Random Access Memories — Daft Punk|0"]},{"s":25,"n":"Folder select","t":"Select Folder","ti":1,"f":1,"r":["Music|1","Podcasts|1","Audiobooks|1","Recordings|1"]}];
/* Each setting a row above changes: t b(ool), c(hoice) or i(nteger);
   o the words for its values, or min/max/step and u(nit); d the value
   a new player starts with; sc a choice that is a number when its
   word is one, as %Lr has it; w the words and topics Search also
   finds it by. */
const FIRMWARE_SETTINGS = {"lineout_active":{"t":"b","o":["No","Yes"],"d":1,"n":"lineout","w":"line out dock system"},"iap_identity":{"t":"c","o":["This Player","iPod Classic","iPhone 4"],"d":0,"n":"identify as","w":"model ipod iphone car dock accessory usb product system"},"iap_browse_size":{"t":"c","o":["Off","5000","10000","20000","40000"],"d":2,"n":"accessory browsing","sc":1,"w":"accessory receiver car library browse usb iap system"},"car_artwork":{"t":"c","o":["Off","As Album Art","Prefer Embedded","Prefer Image File","Prefer Cache"],"d":4,"n":"car artwork","w":"car cover art artwork iap accessory system artwork"},"show_disk_activity":{"t":"b","o":["No","Yes"],"d":0,"n":"show disk activity","w":"busy spinner appearance"},"show_background_tasks":{"t":"b","o":["No","Yes"],"d":1,"n":"show background tasks","w":"busy spinner appearance database"},"show_working":{"t":"b","o":["No","Yes"],"d":1,"n":"show working","w":"busy spinner appearance"},"database_sort_albums_by":{"t":"c","o":["Name","Year (oldest first)","Year (newest first)","Artist + Name","Artist + Year (oldest first)","Artist + Year (newest first)"],"d":0,"n":"database sort albums by","w":"sorting albums year artist library"},"art_cache_album_source":{"t":"c","o":["Image Files Only","Image Files, Then Embedded","Embedded, Then Image Files","Embedded Only"],"d":1,"n":"art cache album source","w":"thumbnail cache embedded cover folder artwork"},"art_cache_fast_build":{"t":"b","o":["No","Yes"],"d":0,"n":"art cache fast build","w":"thumbnail cache artwork"},"autoresume_enable":{"t":"b","o":["No","Yes"],"d":0,"n":"autoresume enable","w":"resume position playback"},"autoresume_automatic":{"t":"c","o":["No","Always","In Custom Directories Only"],"d":0,"n":"autoresume next track","w":"resume playback"},"bl_filter_first_keypress":{"t":"b","o":["Also Acts","Only Lights Screen"],"d":1,"n":"backlight filters first keypress","w":"backlight first button battery"},"brightness":{"t":"i","min":16,"max":142,"step":1,"u":"","d":28,"n":"brightness","w":"screen bright battery appearance"},"usb_charging":{"t":"c","o":["No","Yes","Force"],"d":0,"n":"usb charging","w":"charge charging usb battery usb system"},"autocreatebookmark":{"t":"c","o":["No","Yes","Ask","Yes - Recent Only","Ask - Recent Only"],"d":0,"n":"autocreate bookmarks","w":"bookmark on stop playback"},"autoupdatebookmark":{"t":"b","o":["No","Yes"],"d":0,"n":"autoupdate bookmarks","w":"bookmark playback"},"autoloadbookmark":{"t":"c","o":["No","Yes","Ask"],"d":0,"n":"autoload bookmarks","w":"bookmark load playback"},"usemrb":{"t":"c","o":["No","Yes","One per Playlist","One per Track"],"d":0,"n":"use most-recent-bookmarks","w":"bookmark recent playback"},"album_covers_on_select":{"t":"c","o":["Show tracks","Play album"],"d":0,"n":"album covers on select","w":"carousel cover flow library appearance"},"album_covers_scroll_speed":{"t":"i","min":100,"max":400,"step":25,"u":"%","d":175,"n":"album covers scroll speed","w":"carousel speed appearance"},"album_covers_random_spin":{"t":"c","o":["Short","Medium","Long"],"d":1,"n":"album covers random spin","w":"carousel shuffle play hold library"},"crossfade":{"t":"c","o":["Off","Automatic Track Change Only","Manual Track Skip Only","Shuffle","Shuffle or Manual Track Skip","Always"],"d":0,"n":"crossfade","w":"overlap gapless playback"},"crossfade_fade_in_delay":{"t":"i","min":0,"max":7,"step":1,"u":"s","d":0,"n":"crossfade fade in delay","w":"crossfade playback"},"crossfade_fade_in_duration":{"t":"i","min":0,"max":15,"step":1,"u":"s","d":2,"n":"crossfade fade in duration","w":"crossfade playback"},"crossfade_fade_out_delay":{"t":"i","min":0,"max":7,"step":1,"u":"s","d":0,"n":"crossfade fade out delay","w":"crossfade playback"},"crossfade_fade_out_duration":{"t":"i","min":0,"max":15,"step":1,"u":"s","d":2,"n":"crossfade fade out duration","w":"crossfade playback"},"crossfade_fade_out_mixmode":{"t":"c","o":["Crossfade","Mix"],"d":0,"n":"crossfade fade out mode","w":"crossfade mix playback"},"crossfeed":{"t":"c","o":["Off","Simple (Meier)","Custom"],"d":0,"n":"crossfeed","w":"headphone meier sound"},"tagcache_scan_on_startup":{"t":"b","o":["No","Yes"],"d":0,"n":"tagcache_scan_on_startup","w":"database scan boot library database"},"tagcache_scan_on_eject":{"t":"b","o":["No","Yes"],"d":1,"n":"tagcache_scan_on_eject","w":"database scan usb eject library database"},"year_from_folder":{"t":"b","o":["No","Yes"],"d":0,"n":"year from folder","w":"album release date folders library database"},"debug_log_tagcache":{"t":"b","o":["No","Yes"],"d":0,"n":"debug log tagcache","w":"log debug scan library database system"},"debug_log_artcache":{"t":"b","o":["No","Yes"],"d":0,"n":"debug log artcache","w":"log debug thumbnail cover artwork system"},"debug_log_playback":{"t":"b","o":["No","Yes"],"d":0,"n":"debug log playback","w":"log debug skip codec error track system playback"},"debug_log_skin":{"t":"b","o":["No","Yes"],"d":0,"n":"debug log theme","w":"log debug theme skin font bitmap memory system appearance"},"debug_log_tasks":{"t":"b","o":["No","Yes"],"d":0,"n":"debug log tasks","w":"log debug background task database art scan system"},"debug_log_expiry":{"t":"c","o":["1 hour","6 hours","24 hours","7 days","Never"],"d":2,"n":"debug log expiry","sc":1,"w":"log debug switch off expire hours system"},"dialog_box_border_width":{"t":"i","min":0,"max":10,"step":1,"u":"px","d":2,"n":"dialog box border width","w":"dialog box appearance theme"},"dialog_box_margin":{"t":"i","min":0,"max":40,"step":1,"u":"px","d":10,"n":"dialog box margin","w":"dialog box appearance theme"},"dialog_box_shadow":{"t":"i","min":0,"max":16,"step":1,"u":"px","d":4,"n":"dialog box shadow","w":"dialog box drop shadow appearance theme"},"dialog_btn_border_width":{"t":"i","min":0,"max":10,"step":1,"u":"px","d":2,"n":"dialog button border width","w":"dialog button appearance theme"},"dialog_btn_border_radius":{"t":"i","min":0,"max":20,"step":1,"u":"px","d":0,"n":"dialog button border radius","w":"dialog button corner rounded appearance theme"},"dialog_colors":{"t":"c","o":["Off","On","Auto"],"d":2,"n":"dialog colours","w":"dialog colors box appearance"},"eq_enabled":{"t":"b","o":["No","Yes"],"d":0,"n":"eq enabled","w":"eq equalizer equaliser sound"},"bl_selective_actions":{"t":"b","o":["No","Yes"],"d":0,"n":"No Backlight On Selected Actions","w":"backlight exemptions selective play seek skip volume battery"},"rewind_across_tracks":{"t":"b","o":["No","Yes"],"d":0,"n":"rewind across tracks","w":"rewind previous playback"},"dirfilter":{"t":"c","o":["All","Supported","Music","Playlists"],"d":1,"n":"show files","w":"filter hide library"},"show_filename_ext":{"t":"c","o":["Off","On","Only Unknown Types","Only When Viewing All Types"],"d":3,"n":"show filename exts","w":"extension library"},"browse_current":{"t":"b","o":["No","Yes"],"d":0,"n":"follow playlist","w":"library"},"show_path_in_browser":{"t":"c","o":["Off","Current Directory Only","Full Path"],"d":1,"n":"show path in browser","w":"path title library appearance"},"dircache":{"t":"b","o":["No","Yes"],"d":1,"n":"dircache","w":"directory cache speed library system"},"surround_method2":{"t":"b","o":["No","Yes"],"d":0,"n":"side only","w":"haas surround sound"},"dynamic_colors":{"t":"b","o":["No","Yes"],"d":1,"n":"dynamic colors","w":"dynamic colours album appearance artwork"},"dynamic_colors_background":{"t":"c","o":["Auto","Light","Dark"],"d":0,"n":"dynamic colors background","w":"dynamic colours light dark appearance artwork"},"playlist_viewer_icons":{"t":"b","o":["No","Yes"],"d":1,"n":"playlist viewer icons","w":"playlist viewer playlist appearance"},"statusbar":{"t":"c","o":["Off","Top","Bottom"],"d":0,"n":"statusbar","w":"status bar appearance"},"battery_display":{"t":"c","o":["Graphic","Numeric"],"d":0,"n":"battery display","w":"status bar battery appearance battery"},"volume_type":{"t":"c","o":["Graphic","Numeric"],"d":0,"n":"volume display","w":"status bar volume appearance"},"cursor_style":{"t":"c","o":["Pointer","Bar (Inverse)","Bar (Solid Colour)","Bar (Gradient Colour)"],"d":3,"n":"selector type","w":"line selector cursor highlight appearance"},"keyclick":{"t":"c","o":["Off","Weak","Moderate","Strong"],"d":0,"n":"keyclick","w":"keyclick click system"},"keyclick_hardware":{"t":"b","o":["No","Yes"],"d":1,"n":"hardware keyclick","w":"keyclick click speaker system"},"keyclick_repeats":{"t":"b","o":["No","Yes"],"d":0,"n":"keyclick repeats","w":"keyclick click system"},"default_codepage":{"t":"c","o":["Latin1 (ISO-8859-1)","Greek (ISO-8859-7)","Hebrew (ISO-8859-8)","Cyrillic (CP1251)","Thai (ISO-8859-11)","Arabic (CP1256)","Turkish (ISO-8859-9)","Latin Extended (ISO-8859-2)","Central European (CP1250)","Western European (CP1252)","Japanese (SJIS)","Simp. Chinese (GB2312)","Korean (KSX1001)","Trad. Chinese (BIG5)","Unicode (UTF-8)"],"d":14,"n":"default codepage","w":"codepage character set encoding system"},"max_files_in_dir":{"t":"i","min":1000,"max":100000,"step":1000,"u":"","d":5000,"n":"max files in dir","w":"limit maximum entries library system"},"glyphs_to_cache":{"t":"i","min":50,"max":65540,"step":10,"u":"","d":250,"n":"glyphs","w":"glyph cache font limit system appearance"},"lyric_colour_mode":{"t":"c","o":["Theme","Theme Inverted","Black on White","White on Black"],"d":0,"n":"lyric colour mode","w":"lyrics colors appearance"},"lyric_align":{"t":"c","o":["Left","Centre","Right"],"d":1,"n":"lyric align","w":"lyrics alignment appearance"},"lyric_line_spacing":{"t":"i","min":0,"max":10,"step":1,"u":"","d":2,"n":"lyric line spacing","w":"lyrics leading appearance"},"lyric_prev_opacity":{"t":"i","min":0,"max":100,"step":5,"u":"%","d":30,"n":"lyric previous opacity","w":"lyrics fade appearance"},"lyric_next_opacity":{"t":"i","min":0,"max":100,"step":5,"u":"%","d":55,"n":"lyric next opacity","w":"lyrics fade appearance"},"lyric_anim":{"t":"c","o":["Off","Fast","Normal","Slow"],"d":2,"n":"lyric animation","w":"lyrics scroll appearance"},"lyric_highlight":{"t":"b","o":["No","Yes"],"d":1,"n":"lyric highlight","w":"lyrics words appearance"},"lyric_backlight":{"t":"b","o":["No","Yes"],"d":1,"n":"lyric backlight","w":"lyrics backlight appearance battery"},"settings_mode":{"t":"c","o":["Standard","Everything"],"d":0,"n":"settings mode","w":"basic advanced system"},"album_show_year":{"t":"c","o":["No","Before","After"],"d":0,"n":"album show year","w":"albums release date library"},"featured_artists":{"t":"b","o":["No","Yes"],"d":0,"n":"featured artists","w":"guest feat credits library"},"segregate_audiobooks":{"t":"b","o":["No","Yes"],"d":0,"n":"segregate audiobooks","w":"audiobook books spoken word podcast library"},"browser_default":{"t":"c","o":["Files","Music","Playlists"],"d":1,"n":"default browser","w":"browser default library appearance"},"wps_select_action":{"t":"c","o":["Previous Screen","Database","Coverflow","Files","Lyrics"],"d":0,"n":"wps select action","w":"select action appearance"},"unplug_mode":{"t":"c","o":["Off","Pause","Pause and Resume"],"d":2,"n":"pause on headphone unplug","w":"headphone unplug playback"},"unplug_autoresume":{"t":"b","o":["No","Yes"],"d":0,"n":"disable autoresume if phones not present","w":"headphone resume playback"},"spun_artwork":{"t":"b","o":["No","Yes"],"d":1,"n":"spun artwork","w":"spun playback report statistics cards album art sleeves memory artwork library"},"spun_top_count":{"t":"i","min":5,"max":25,"step":5,"u":"","d":10,"n":"spun top count","w":"spun playback report statistics top ten artists songs albums how many rows library"},"spun_badge_order":{"t":"c","o":["As Listed","Earned First","Newest First","Closest First"],"d":0,"n":"spun badge order","w":"spun playback report statistics achievements badges sort order earned date progress library"},"spun_rank_by":{"t":"c","o":["Plays","Minutes"],"d":0,"n":"spun rank by","w":"spun playback report statistics top artists songs albums ranked plays minutes time listened library"},"playlist_shuffle":{"t":"b","o":["No","Yes"],"d":0,"n":"shuffle","w":"random playback"},"repeat_mode":{"t":"c","o":["Off","All","One","Shuffle","A-B"],"d":0,"n":"repeat","w":"playback"},"play_selected":{"t":"b","o":["No","Yes"],"d":1,"n":"play selected","w":"playback"},"fade_on_stop":{"t":"b","o":["No","Yes"],"d":1,"n":"volume fade","w":"fade stop pause sound playback"},"single_mode":{"t":"c","o":["Off","Track","Album","Album Artist","Artist","Composer","Work","Genre","Playlist"],"d":0,"n":"single mode","w":"stop after playback"},"beep":{"t":"c","o":["Off","Weak","Moderate","Strong"],"d":0,"n":"beep","w":"skip beep playback"},"next_folder":{"t":"c","o":["No","Next Folder","Random Folder","Next in Start Directory","Random in Start Directory"],"d":0,"n":"folder navigation","w":"auto change directory next folder random constrain start playback"},"cuesheet":{"t":"b","o":["No","Yes"],"d":0,"n":"cuesheet support","w":"cue sheet playback"},"chapter_marks":{"t":"b","o":["No","Yes"],"d":0,"n":"chapter marks","w":"chapter audiobook book m4b playback"},"prevent_skip":{"t":"b","o":["No","Yes"],"d":0,"n":"prevent track skip","w":"lock kiosk playback"},"playback_log":{"t":"c","o":["Off","On","Last.fm Logger"],"d":1,"n":"play log","w":"scrobble last.fm logging playback"},"playlist_engine":{"t":"b","o":["No","Yes"],"d":0,"n":"playlist engine","w":"similar genius sound analysis mix library playlist"},"analysis_depth":{"t":"c","o":["Thorough","Quick"],"d":0,"n":"analysis depth","w":"scan speed accuracy quick thorough library playlist"},"mix_length":{"t":"i","min":5,"max":100,"step":5,"u":"","d":40,"n":"generated playlist length","w":"tracks how many size mood similar library playlist"},"track_playlist":{"t":"c","o":["Predictable","Weekly","Variable"],"d":0,"n":"track playlist","w":"similar random varied repeatable library playlist"},"mood_playlist":{"t":"c","o":["Predictable","Weekly","Variable"],"d":1,"n":"mood playlist","w":"mood journey random varied repeatable library playlist"},"mix_starts_with_selected":{"t":"b","o":["No","Yes"],"d":1,"n":"mix starts with selected","w":"similar wind down first seed library playlist"},"continue_playing":{"t":"b","o":["No","Yes"],"d":0,"n":"continue playing","w":"keep going endless extend never stop library playlist"},"playlist_viewer_indices":{"t":"b","o":["No","Yes"],"d":0,"n":"playlist viewer indices","w":"playlist viewer numbers playlist appearance"},"playlist_viewer_track_display":{"t":"c","o":["Track Name Only","Full Path","Title & Album from ID3 Tags","Title from ID3 Tags"],"d":0,"n":"playlist viewer track display","w":"playlist viewer playlist appearance"},"sort_playlists":{"t":"c","o":["Alphabetical","By Date","By Newest Date"],"d":0,"n":"sort playlists","w":"playlist library"},"recursive_dir_insert":{"t":"c","o":["Off","On","Ask"],"d":1,"n":"recursive directory insert","w":"subfolder insert playlist"},"warnon_erase_dynplaylist":{"t":"b","o":["No","Yes"],"d":1,"n":"warn when erasing dynamic playlist","w":"warn erase playlist"},"party_mode":{"t":"b","o":["No","Yes"],"d":0,"n":"party mode","w":"queue playback"},"keep_current_track_on_replace_playlist":{"t":"b","o":["No","Yes"],"d":1,"n":"keep current track when replacing playlist","w":"playlist"},"show_shuffled_adding_options":{"t":"b","o":["No","Yes"],"d":1,"n":"show shuffled adding options","w":"shuffle add playlist"},"quiz_title":{"t":"b","o":["No","Yes"],"d":1,"n":"quiz title","w":"music quiz game questions track library"},"quiz_artist":{"t":"b","o":["No","Yes"],"d":1,"n":"quiz artist","w":"music quiz game questions library"},"quiz_album":{"t":"b","o":["No","Yes"],"d":1,"n":"quiz album","w":"music quiz game questions library"},"quiz_year":{"t":"b","o":["No","Yes"],"d":0,"n":"quiz year","w":"music quiz game questions date library"},"scrollbar":{"t":"c","o":["Off","Left","Right"],"d":0,"n":"scrollbar","w":"scroll bar appearance"},"scrolling_enabled":{"t":"b","o":["No","Yes"],"d":1,"n":"scrolling enabled","w":"scrolling text ellipsis cut truncate scrolling appearance"},"scroll_speed":{"t":"i","min":0,"max":17,"step":1,"u":"","d":14,"n":"scroll speed","w":"scrolling text scrolling appearance"},"scroll_step":{"t":"i","min":1,"max":320,"step":1,"u":"px","d":1,"n":"scroll step","w":"scrolling text scrolling appearance"},"bidir_limit":{"t":"i","min":0,"max":200,"step":25,"u":"%","d":50,"n":"bidir limit","w":"bidirectional scrolling bounce scrolling appearance"},"offset_out_of_view":{"t":"b","o":["No","Yes"],"d":0,"n":"Screen Scrolls Out Of View","w":"scrolling offscreen edge scrolling appearance"},"disable_mainmenu_scrolling":{"t":"b","o":["No","Yes"],"d":0,"n":"Disable main menu scrolling","w":"scrolling main menu root scrolling appearance"},"screen_scroll_step":{"t":"i","min":1,"max":320,"step":1,"u":"px","d":16,"n":"screen scroll step","w":"scrolling screen scrolling appearance"},"scroll_paginated":{"t":"b","o":["No","Yes"],"d":0,"n":"scroll paginated","w":"paged scrolling list scrolling appearance"},"list_wraparound":{"t":"b","o":["No","Yes"],"d":1,"n":"list wraparound","w":"wrap list scrolling appearance"},"list_order":{"t":"c","o":["Descending","Ascending"],"d":1,"n":"list order","w":"list ascending descending scrolling appearance"},"db_search_max_rows":{"t":"c","o":["25","50","75","100","125","150","175","200"],"d":1,"n":"search max rows","sc":1,"w":"search find results library"},"db_search_min_letters":{"t":"c","o":["1","2","3"],"d":0,"n":"search min letters","sc":1,"w":"search find letters library"},"db_search_order":{"t":"c","o":["Tracks, Albums, Artists","Tracks, Artists, Albums","Albums, Tracks, Artists","Albums, Artists, Tracks","Artists, Albums, Tracks","Artists, Tracks, Albums"],"d":0,"n":"search order","w":"search find order library"},"sleeptimer_duration":{"t":"i","min":5,"max":300,"step":5,"u":"min","d":30,"n":"sleeptimer duration","w":"sleep timer battery system"},"sleeptimer_on_startup":{"t":"b","o":["No","Yes"],"d":0,"n":"sleeptimer on startup","w":"sleep timer boot battery system"},"keypress_restarts_sleeptimer":{"t":"b","o":["No","Yes"],"d":0,"n":"keypress restarts sleeptimer","w":"sleep timer battery system"},"bass":{"t":"i","u":"dB","step":1,"min":-12,"max":12,"d":0,"n":"bass","w":"base low end sound"},"bass_cutoff":{"t":"i","u":"","step":1,"min":1,"max":4,"d":1,"n":"bass cutoff","w":"base corner frequency sound"},"treble":{"t":"i","u":"dB","step":1,"min":-12,"max":12,"d":0,"n":"treble","w":"high sound"},"treble_cutoff":{"t":"i","u":"","step":1,"min":1,"max":4,"d":1,"n":"treble cutoff","w":"corner frequency sound"},"balance":{"t":"i","u":"%","step":1,"min":-100,"max":100,"d":0,"n":"balance","w":"left right sound"},"channel_config":{"t":"c","o":["Stereo","Mono","Custom","Mono Left","Mono Right","Karaoke","Swap Left & Right"],"d":0,"n":"channels","w":"mono stereo karaoke swap sound"},"stereo_width":{"t":"i","u":"%","step":5,"min":0,"max":250,"d":100,"n":"stereo_width","w":"stereo width sound"},"dithering_enabled":{"t":"b","o":["No","Yes"],"d":0,"n":"dithering enabled","w":"dither noise sound"},"afr_enabled":{"t":"c","o":["Off","Weak","Moderate","Strong"],"d":0,"n":"afr enabled","w":"fatigue harsh listening sound"},"show_shutdown_message":{"t":"b","o":["No","Yes"],"d":1,"n":"show shutdown message","w":"shutdown splash system"},"start_in_screen":{"t":"c","o":["Previous Screen","Main Menu","Files","Music","Resume Playback","Settings","Recent Bookmarks"],"d":1,"n":"start in screen","w":"start screen boot system appearance"},"disk_spindown":{"t":"i","min":3,"max":254,"step":1,"u":"s","d":5,"n":"disk spindown","w":"disk drive spin idle battery system"},"shortcuts_replaces_qs":{"t":"b","o":["On","Shortcuts instead"],"d":0,"n":"shortcuts instead of quickscreen","w":"quickscreen shortcuts system appearance"},"volume_adjust_mode":{"t":"c","o":["Direct","Perceptual"],"d":0,"n":"volume adjustment mode","w":"perceptual steps sound system"},"show_debug_menu":{"t":"b","o":["No","Yes"],"d":0,"n":"show debug menu","w":"debug system"},"text_viewer_colour_mode":{"t":"c","o":["Theme","Theme Inverted","Black on White","White on Black"],"d":3,"n":"text viewer colour mode","w":"text viewer colors appearance"},"text_viewer_margin":{"t":"b","o":["No","Yes"],"d":1,"n":"text viewer margin","w":"text viewer appearance"},"text_viewer_line_spacing":{"t":"i","min":0,"max":8,"step":1,"u":"","d":0,"n":"text viewer line spacing","w":"text viewer leading appearance"},"text_viewer_page_number":{"t":"b","o":["No","Yes"],"d":0,"n":"text viewer page number","w":"text viewer appearance"},"text_viewer_read_aloud":{"t":"c","o":["Off","Each Page","Continuously"],"d":1,"n":"text viewer read aloud","w":"text viewer speak read book voice"},"usb_mode":{"t":"c","o":["Mass Storage","Charge Only","Drive Only"],"d":0,"n":"usb mode","w":"usb mass storage usb system"},"usb_hid":{"t":"b","o":["No","Yes"],"d":0,"n":"usb hid","w":"usb hid keyboard remote usb system"},"usb_keypad_mode":{"t":"c","o":["Multimedia","Presentation","Browser","Mouse"],"d":0,"n":"usb keypad mode","w":"usb hid keypad usb system"},"talk_menu":{"t":"b","o":["No","Yes"],"d":0,"n":"talk menu","w":"voice speak aloud menus voice"},"talk_voice":{"t":"c","o":["Built-in","Voice File for Menus","Voice File Only"],"d":0,"n":"talk voice","w":"voice built-in synthesiser file recorded menus names voice"},"talk_speech_rate":{"t":"i","min":50,"max":150,"step":10,"u":"%","d":100,"n":"speech rate","w":"voice speech speed rate voice"},"talk_dir":{"t":"c","o":["Off","Numbers","Spell"],"d":2,"n":"talk dir","w":"voice directories voice"},"talk_dir_clip":{"t":"b","o":["No","Yes"],"d":0,"n":"talk dir clip","w":"voice clips voice"},"talk_file":{"t":"c","o":["Off","Numbers","Spell"],"d":2,"n":"talk file","w":"voice filenames voice"},"talk_file_clip":{"t":"b","o":["No","Yes"],"d":0,"n":"talk file clip","w":"voice clips voice"},"talk_filetype":{"t":"b","o":["No","Yes"],"d":0,"n":"talk filetype","w":"voice file type voice"},"talk_list_title":{"t":"b","o":["No","Yes"],"d":0,"n":"say list titles","w":"voice speak list menu title heading voice"},"talk_track_change":{"t":"c","o":["Off","Over the Music","Pause the Music"],"d":0,"n":"announce track change","w":"voice speak track title artist song pause voice"},"talk_battery_level":{"t":"b","o":["No","Yes"],"d":0,"n":"Announce Battery Level","w":"voice speak battery level voice"},"talk_mixer_amp":{"t":"i","min":0,"max":100,"step":5,"u":"%","d":100,"n":"talk mixer level","w":"voice volume voice"}};

const LCD_W = 320, LCD_H = 240;
const ESCAPABLE = "%(,);#<|>";

/* Tags flagged NOBREAK in tag_table.c. Any one of them on a line sets
   info->no_line_break, and skin_render.c then skips line_number++ — so the
   next line draws on the same row. Note %Vs is NOT in this set. */
const NOBREAK_TAGS = new Set(["xl","X","x","Vp","Vf","Vb","Vg","VB","Vy","Vt","we","wd","wi","Fl","Cl","T"]);

/* Tags handled by do_non_text_tags() in skin_render.c: they draw themselves at
   their own coordinates, or just configure, and never append text to the line
   buffer. Only text written into that buffer is placed by line_number, so only
   text can be overdrawn by a suppressed line break. */
const NON_TEXT_ALWAYS = new Set([...NOBREAK_TAGS,
  "V","Vl","Vi","Vd","VI","Vs","Lb","vs","ac","al","ar","aL","aR","ax","s","t","Tl",
  "pm","dr","xd","x9","Cd","La","Sb","wt","LB","pL","pR"]);

/* These are bars — and so draw themselves — only when given coordinates.
   Bare (%bl, %pb) they resolve to text. */
const BAR_TAGS = new Set(["bl","pb","pv","pP","pX","St","tr"]);

const producesText = n =>
  !NON_TEXT_ALWAYS.has(n.name) && !(BAR_TAGS.has(n.name) && n.args.length > 0);

/* Row tags: these resolve a listitem, which only exists while list rows are
   being drawn — i.e. inside a viewport group named by %Lb. %Lt/%Li/%LB are
   list-level, not row-level, and are fine anywhere. */
const ROW_TAGS = new Set(["LT","LI","La","Lc","LN","LR","LC"]);

/* enum current_activity's last; the Theme IDE's build counts it */
const ACTIVITY_MAX = (typeof window !== "undefined" && window.FIRMWARE_ACTIVITY_MAX) || 30;

function lookup(name) {
  if (Object.prototype.hasOwnProperty.call(CUSTOM, name)) return { def: CUSTOM[name], custom: true };
  if (Object.prototype.hasOwnProperty.call(UPSTREAM, name)) return { def: UPSTREAM[name], custom: false };
  return null;
}

/* find_tag() order: custom names first at full length, then upstream 3, 2, 1 chars. */
function matchTagName(text, i) {
  for (let len = 3; len >= 1; len--) {
    const cand = text.substr(i, len);
    if (cand.length < len) continue;
    if (Object.prototype.hasOwnProperty.call(CUSTOM, cand)) return cand;
  }
  for (let len = 3; len >= 1; len--) {
    const cand = text.substr(i, len);
    if (cand.length < len) continue;
    if (Object.prototype.hasOwnProperty.call(UPSTREAM, cand)) return cand;
  }
  return null;
}

/* ============================================================
   Scanner — produces a structural tree and flat colour ranges
   over the same offsets, so multi-line conditionals stay intact.
   ============================================================ */

function scan(text) {
  const ranges = [];
  const issues = [];
  let i = 0;

  const paint = (s, e, cls) => { if (e > s) ranges.push({ s, e, cls }); };

  function readName(at) {
    const name = matchTagName(text, at);
    if (name) return name;
    let n = 0;
    while (n < 3 && at + n < text.length && /[A-Za-z]/.test(text[at + n])) n++;
    return n ? text.substr(at, n) : null;
  }

  /* Skipping whole %constructs, the way skip_tag() does in the C scanner.
     Counting < and > instead would break on the comparison operators in
     %if(%pe, >=, 10) — the '>' there is an argument, not a branch delimiter. */
  function skipBalanced(j, open, close) {
    let d = 0;
    while (j < text.length) {
      const c = text[j];
      if (c === "%") { j = skipConstruct(j); continue; }
      if (c === open) { d++; j++; continue; }
      if (c === close) { d--; j++; if (d === 0) return j; continue; }
      j++;
    }
    return j;
  }
  function skipConstruct(j) {
    if (text[j] !== "%") return j + 1;
    if (ESCAPABLE.includes(text[j + 1] || "")) return j + 2;
    let k = j + 1;
    const isCond = text[k] === "?";
    if (isCond) k++;
    const nm = matchTagName(text, k);
    k += nm ? nm.length : 1;
    if (text[k] === "(") k = skipBalanced(k, "(", ")");
    if (isCond && text[k] === "<") k = skipBalanced(k, "<", ">");
    return k;
  }

  /* Reads (a, b, c) with nesting, returning raw arg strings + their offsets. */
  function readArgs(at) {
    if (text[at] !== "(") return null;
    const open = at;
    let j = at + 1, start = j, depth = 1;
    const args = [];
    while (j < text.length && depth > 0) {
      const c = text[j];
      if (c === "%") { j = skipConstruct(j); continue; }
      if (c === "(") { depth++; j++; continue; }
      if (c === ")") { depth--; if (!depth) break; j++; continue; }
      if (c === "," && depth === 1) {
        args.push({ raw: text.slice(start, j), s: start, e: j });
        start = j + 1;
      }
      j++;
    }
    if (depth !== 0) {
      issues.push({ sev: "err", off: open, msg: "Unclosed ( — the argument list never ends." });
      args.push({ raw: text.slice(start, text.length), s: start, e: text.length });
      return { args, end: text.length, open };
    }
    args.push({ raw: text.slice(start, j), s: start, e: j });
    if (args.length === 1 && args[0].raw === "") args.length = 0;
    return { args, end: j + 1, open };
  }

  function classifyArg(raw) {
    const t = raw.trim();
    if (t === "" || t === "-") return "punct";
    if (/^-?\d+(\.\d+)?%?$/.test(t)) return "num";
    if (t.startsWith("%")) return "code";
    return "str";
  }

  function paintArgs(args) {
    for (const a of args) {
      const kind = classifyArg(a.raw);
      /* Keep the parsed nodes on the argument — the inspector and the linter
         both need to see inside nested code, not just colour it. */
      if (kind === "code") { const save = i; i = a.s; a.nodes = scanRun(a.e); i = save; }
      else paint(a.s, a.e, kind === "num" ? "t-num" : kind === "punct" ? "t-punct" : "t-str");
    }
  }

  /* nameAt = offset of the tag name. For a plain tag that is just past '%';
     for a conditional's subject the '%?' has already been consumed. */
  function parseTag(nameAt, isCondSubject) {
    const start = isCondSubject ? nameAt - 2 : nameAt - 1;   /* '%' or '%?' */
    const paintFrom = isCondSubject ? nameAt : start;
    const name = readName(nameAt);
    if (!name) { paint(nameAt - 1, nameAt, "t-punct"); i = nameAt; return { type: "text", text: "%", s: start, e: i }; }
    const info = lookup(name);
    let cls = info ? (info.custom ? "t-custom" : "t-tag") : "t-bad";
    if (!info) issues.push({ sev: "err", off: start, msg: "Unknown tag %" + name + " — not in the upstream table or this fork's custom tags." });
    if (name === "V" || name === "Vl" || name === "Vi") cls = "t-vpdecl";
    i = nameAt + name.length;

    let args = [], hasArgList = false;
    if (text[i] === "(") {
      const r = readArgs(i);
      paint(paintFrom, i, cls);
      paint(r.open, r.open + 1, "t-punct");
      paintArgs(r.args);
      if (r.end - 1 < text.length) paint(r.end - 1, r.end, "t-punct");
      args = r.args;
      hasArgList = true;
      i = r.end;
    } else {
      paint(paintFrom, i, cls);
    }
    /* hasArgList separates "%V" from "%V()" — the parser rejects the two
       differently, and args is [] for both. */
    return { type: "tag", name, args, hasArgList, info, custom: !!(info && info.custom), s: start, e: i, isCondSubject: !!isCondSubject };
  }

  function parseConditional() {
    const start = i;                       /* at '%' of "%?" */
    paint(start, start + 2, "t-cond");
    i += 2;
    const subject = parseTag(i, true);
    const branches = [];
    if (text[i] !== "<") {
      issues.push({ sev: "err", off: i, msg: "Conditional %?" + (subject.name || "") + " is missing its < … > branch list." });
      return { type: "cond", subject, branches, s: start, e: i };
    }
    paint(i, i + 1, "t-cond");
    i++;
    let guard = 0;
    while (i < text.length && guard++ < 100000) {
      const bs = i;
      const kids = scanRun(null, true);
      branches.push({ nodes: kids, s: bs, e: i });
      if (text[i] === "|") { paint(i, i + 1, "t-cond"); i++; continue; }
      if (text[i] === ">") { paint(i, i + 1, "t-cond"); i++; break; }
      if (i >= text.length) {
        issues.push({ sev: "err", off: start, msg: "Conditional %?" + (subject.name || "") + " is never closed with >." });
        break;
      }
    }
    return { type: "cond", subject, branches, s: start, e: i };
  }

  /* Scans until `limit` (offset) or, in conditional mode, until | or > */
  function scanRun(limit, inCond) {
    const out = [];
    while (i < text.length) {
      if (limit !== null && limit !== undefined && i >= limit) break;
      const c = text[i];
      if (inCond && (c === "|" || c === ">")) break;

      if (c === "#") {
        const s = i;
        while (i < text.length && text[i] !== "\n") i++;
        paint(s, i, "t-cmt");
        out.push({ type: "comment", text: text.slice(s, i), s, e: i });
        continue;
      }
      if (c === "%" && text[i + 1] === "?") { out.push(parseConditional()); continue; }
      if (c === "%" && i + 1 >= text.length) { paint(i, i + 1, "t-punct"); i++; continue; }
      if (c === "%" && ESCAPABLE.includes(text[i + 1] || "")) {
        const s = i; i += 2;
        paint(s, i, "t-punct");
        out.push({ type: "text", text: text[s + 1], s, e: i, escaped: true });
        continue;
      }
      if (c === "%") { out.push(parseTag(i + 1, false)); continue; }
      if (c === ";") { const s = i; i++; paint(s, i, "t-cond"); out.push({ type: "subline", s, e: i }); continue; }

      const s = i;
      while (i < text.length) {
        const ch = text[i];
        if (ch === "%" || ch === "#" || ch === ";" || ch === "\n") break;
        if (inCond && (ch === "|" || ch === ">")) break;
        i++;
      }
      if (i === s) { i++; continue; }
      const chunk = text.slice(s, i);
      /* Spaces between tags are text -- "%cM %cP" draws "5:37 PM" -- so only
         a line holding nothing but spaces is dropped. */
      const wholeLine = (s === 0 || text[s - 1] === "\n") && (i >= text.length || text[i] === "\n");
      if (chunk.trim() !== "" || !wholeLine) out.push({ type: "text", text: chunk, s, e: i });
      if (i < text.length && text[i] === "\n") { if (limit === null || limit === undefined) { if (!inCond) { i++; out.push({ type: "nl", s: i - 1, e: i }); } else i++; } }
    }
    return out;
  }

  const nodes = scanRun(null, false);
  return { nodes, ranges, issues };
}

/* ============================================================
   Geometry — mirrors parse_viewport() in apps-ipod/skin/skin_parser.c
   ============================================================ */

function numOf(raw) {
  const t = (raw || "").trim();
  if (t === "" || t === "-") return { def: true };
  const m = /^(-?\d+(?:\.\d+)?)(%)?$/.exec(t);
  if (!m) return { bad: true, raw: t };
  const v = parseFloat(m[1]);
  if (m[2]) return { permille: Math.round(v * 10) };
  return { n: Math.round(v) };
}

function resolveVp(args, labelled) {
  const a = labelled ? args.slice(1) : args;
  const g = { x: 0, y: 0, w: LCD_W, h: LCD_H, font: null, fontDefault: true, bad: [] };
  const px = numOf(a[0] ? a[0].raw : ""), py = numOf(a[1] ? a[1].raw : "");
  const pw = numOf(a[2] ? a[2].raw : ""), ph = numOf(a[3] ? a[3].raw : "");
  const pf = numOf(a[4] ? a[4].raw : "");

  if (!px.def && !px.bad) g.x = px.permille !== undefined ? Math.trunc(px.permille * LCD_W / 1000)
                              : (px.n < 0 ? px.n + LCD_W : px.n);
  if (!py.def && !py.bad) g.y = py.permille !== undefined ? Math.trunc(py.permille * LCD_H / 1000)
                              : (py.n < 0 ? py.n + LCD_H : py.n);

  if (pw.def || pw.bad) g.w = LCD_W - g.x;
  else g.w = pw.permille !== undefined ? Math.trunc(pw.permille * LCD_W / 1000)
           : (pw.n < 0 ? (pw.n + LCD_W) - g.x : pw.n);

  if (ph.def || ph.bad) g.h = LCD_H - g.y;
  else g.h = ph.permille !== undefined ? Math.trunc(ph.permille * LCD_H / 1000)
           : (ph.n < 0 ? (ph.n + LCD_H) - g.y : ph.n);

  if (!pf.def && !pf.bad && pf.n !== undefined) { g.font = pf.n; g.fontDefault = false; }

  g.oob = (g.x < 0 || g.x >= LCD_W || g.x + g.w > LCD_W ||
           g.y < 0 || g.y >= LCD_H || g.y + g.h > LCD_H);
  return g;
}

/* %Cl and %dr take viewport-relative coordinates; percentages scale to the viewport. */
function resolveInner(args, vp) {
  const p = [0, 1, 2, 3].map(k => numOf(args[k] ? args[k].raw : ""));
  const val = (q, mult, fallback) => q.def || q.bad ? fallback
      : (q.permille !== undefined ? Math.trunc(q.permille * mult / 1000) : q.n);
  const x = val(p[0], vp.w, 0), y = val(p[1], vp.h, 0);
  return { x: vp.x + x, y: vp.y + y, w: val(p[2], vp.w, vp.w - x), h: val(p[3], vp.h, vp.h - y) };
}

/* ============================================================
   Model — viewport blocks, fonts, references, issues
   ============================================================ */

function build(text) {
  const { nodes, ranges, issues } = scan(text);
  const fonts = {};
  const fontSites = {};
  const vps = [];
  const shown = new Set();
  const preloaded = new Set();
  const images = [];
  let cur = null;

  const startBlock = (node) => {
    const labelled = node.name !== "V";
    const label = labelled && node.args[0] ? node.args[0].raw.trim() : null;
    const g = resolveVp(node.args, labelled);
    const kind = node.name === "V" ? "vp" : node.name === "Vl" ? "cond" : "ui";
    cur = {
      kind, label, tagName: node.name, node,
      x: g.x, y: g.y, w: g.w, h: g.h, font: g.font, fontDefault: g.fontDefault,
      oob: g.oob, off: node.s, art: null, rects: [], visible: true,
      empty: node.args.length === 0,
    };
    if (node.args.length === 0) { cur.x = 0; cur.y = 0; cur.w = LCD_W; cur.h = LCD_H; cur.oob = false; }
    vps.push(cur);
    if (g.oob) issues.push({ sev: "err", off: node.s,
      msg: "Viewport " + (label ? label + " " : "") + "resolves to " + g.x + "," + g.y + " " + g.w + "×" + g.h +
           " — outside the 320×240 screen. Rockbox rejects the whole skin on this." });
  };

  /* Local line index so the linter works without the editor's globals. */
  const ls = [0];
  for (let k = 0; k < text.length; k++) if (text[k] === "\n") ls.push(k + 1);
  const lnOf = off => {
    let lo = 0, hi = ls.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (ls[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
  };

  const lbLabels = new Set();            /* labels named by %Lb */
  const vdRefs = [], viRefs = [], lbRefs = [];
  const selNodes = [], twNodes = [], csTests = [], clNodes = [], drNodes = [];
  const allTags = [];                    /* every tag written, nested ones too */
  /* Every %Cl in the skin, in the order written, and the one a bare %Cd binds
     to (the most recent). See the %Cl/%Cd block in walk(). */
  const arts = [], cdNodes = [];
  let curArt = null;
  const physLines = new Map();           /* physical line -> what sits on it, top level only */
  const lineRec = off => {
    const k = lnOf(off);
    if (!physLines.has(k)) physLines.set(k, { nobreak: [], draws: [], off });
    return physLines.get(k);
  };

  /* skin_parse_viewport() creates a default viewport when a file does not open
     with %V — everything before the first declaration lives in it. themify_2's
     whole $VD block sits there, so without this its %Vd calls never run. */
  const firstVp = nodes.find(n => n.type === "tag" && (n.name === "V" || n.name === "Vl" || n.name === "Vi"));
  const hasLeading = nodes.some(n =>
    n.type !== "comment" && n.type !== "nl" && (!firstVp || n.s < firstVp.s));
  if (hasLeading) {
    cur = {
      kind: "vp", label: null, tagName: "V", node: null,
      x: 0, y: 0, w: LCD_W, h: LCD_H, font: null, fontDefault: true,
      oob: false, off: 0, art: null, rects: [], visible: true,
      empty: true, implicit: true,
    };
    vps.push(cur);
  }

  const walk = (list, top) => {
    for (const n of list) {
      if (n.type === "text" && top && n.text && n.text.trim() !== "")
        lineRec(n.s).draws.push("text");
      if (n.type === "tag") {
        if (n.info) allTags.push(n);
        if (cur) { if (!cur.tags) cur.tags = new Map(); if (!cur.tags.has(n.name)) cur.tags.set(n.name, n.s); }
        if (top) {
          const rec = lineRec(n.s);
          if (NOBREAK_TAGS.has(n.name)) rec.nobreak.push(n.name);
          if (producesText(n)) rec.draws.push(n.name);
        }
        if (n.name === "Lb" && n.args[0]) { lbLabels.add(n.args[0].raw.trim()); lbRefs.push(n); }
        if (n.name === "Vd" && n.args[0]) vdRefs.push(n);
        if (n.name === "VI" && n.args[0]) viRefs.push(n);
        if (n.name === "sel") selNodes.push(n);
        if (n.name === "tw") twNodes.push(n);
        if (n.name === "if" && n.args.length >= 3 && n.args[0].raw.trim() === "%cs") csTests.push(n);
        if (top && (n.name === "V" || n.name === "Vl" || n.name === "Vi")) { startBlock(n); continue; }
        if (n.name === "Fl" && n.args.length >= 2) {
          const id = (n.args[0].raw || "").trim();
          fonts[id] = (n.args[1].raw || "").trim();
          fontSites[id] = n.s;
        }
        /* A skin may declare several %Cl now. Each is remembered with the box
           it resolved to in its OWN viewport, because the %Cd that draws it
           may sit in another one. cur.art stays for the bare single-%Cl case
           the rest of the model still reads. */
        if (n.name === "Cl") {
          clNodes.push(n);
          const box = cur ? resolveInner(n.args, cur) : null;
          const chain = n.args.length > 6 ? parseFilterChain(n.args[6].raw.trim()) : null;
          const label = (n.args.length > 8 && n.args[8].raw.trim() !== "-")
                        ? n.args[8].raw.trim() : null;
          curArt = { label, box, node: n,
                     filters: (chain && !chain.error) ? chain.steps : [] };
          arts.push(curArt);
          if (cur) cur.art = box;
        }
        /* Bind each %Cd the way the parser does: the label if it names one,
           otherwise the nearest %Cl above it. */
        if (n.name === "Cd") {
          cdNodes.push(n);
          const lbl = (n.args.length > 0 && n.args[0].raw.trim() !== "-")
                      ? n.args[0].raw.trim() : null;
          n.artRef = lbl ? (arts.find(a => a.label === lbl) || null) : (curArt || arts[0] || null);
          n.artLabel = lbl;
        }
        if (n.name === "dr" && cur && n.args.length >= 2) {
          drNodes.push({ node: n, vp: cur });
          cur.rects.push(resolveInner(n.args, cur));
        }
        if (n.name === "VB" && cur) cur.backdrop = true;
        /* %Lb names the viewport group that draws each list row — that counts
           as showing it, exactly like %Vd does. */
        if ((n.name === "Vd" || n.name === "VI" || n.name === "Lb") && n.args[0])
          shown.add(n.args[0].raw.trim());
        if ((n.name === "xl" || n.name === "x") && n.args[0]) {
          preloaded.add(n.args[0].raw.trim());
          const xs = numOf(n.args[2] ? n.args[2].raw : ""), ys = numOf(n.args[3] ? n.args[3].raw : "");
          if (xs.n !== undefined || ys.n !== undefined)
            images.push({ id: n.args[0].raw.trim(), x: xs.n || 0, y: ys.n || 0, off: n.s,
                          file: n.args[1] ? n.args[1].raw.trim() : "" });
        }
        if (n.name === "xd" && n.args[0]) {
          /* With a single 2-character argument the second character picks the
             subimage — %xd(Aa) is subimage 'a' of image 'A'. */
          const raw = n.args[0].raw.trim();
          const base = (n.args.length === 1 && raw.length === 2) ? raw[0] : raw;
          if (!preloaded.has(raw) && !preloaded.has(base))
            issues.push({ sev: "warn", off: n.s, msg: "%xd(" + raw + ") draws an image that no %xl or %x has loaded." });
        }
        for (const a of n.args) if (a.nodes) walk(a.nodes, false);
      } else if (n.type === "cond") {
        if (n.subject) walk([n.subject], false);
        for (const b of n.branches) walk(b.nodes, false);
      }
    }
  };
  walk(nodes, true);

  /* %Vd / %VI naming a label that was never declared */
  const declared = new Set(vps.filter(v => v.label).map(v => v.label));
  for (const name of shown) {
    if (!declared.has(name)) {
      const at = text.indexOf("(" + name + ")");
      issues.push({ sev: "warn", off: at < 0 ? 0 : at, msg: "%Vd/%VI names \"" + name + "\", but no %Vl or %Vi declares it." });
    }
  }
  /* One warning per unshown label, not per declaration — sharing a label across
     several viewports is normal, and %Vd shows the whole group at once. */
  const unshown = new Set();
  for (const v of vps) {
    /* %Vi(-,…) is the default list viewport — it has no label to be named by. */
    if (v.label === "-") continue;
    if (v.label && !shown.has(v.label) && !unshown.has(v.label)) {
      unshown.add(v.label);
      issues.push({ sev: "warn", off: v.off, msg: "Viewport \"" + v.label + "\" is declared but never shown by %Vd or %VI — it will not draw." });
    }
  }
  /* Count the group so the viewport list can say "1 of 4". */
  const groupCount = {};
  for (const v of vps) if (v.label) groupCount[v.label] = (groupCount[v.label] || 0) + 1;
  const groupSeen = {};
  for (const v of vps) if (v.label) {
    groupSeen[v.label] = (groupSeen[v.label] || 0) + 1;
    if (groupCount[v.label] > 1) v.part = groupSeen[v.label] + " of " + groupCount[v.label];
  }

  const labelKinds = new Map();
  for (const v of vps) {
    if (!v.label || v.label === "-") continue;
    if (!labelKinds.has(v.label)) labelKinds.set(v.label, new Set());
    labelKinds.get(v.label).add(v.kind);
  }
  const add = (sev, off, rule, msg) => issues.push({ sev, off, rule, msg });

  /* ---------- the parser's own rules ----------
     Every rule below this block describes a skin that loads and then draws
     wrong. These two describe one that does not load at all: skin_parse_tag()
     in lib/skin_parser/skin_parser.c refuses it and Rockbox drops the whole
     file — no line number, no message on screen, the theme simply does not
     apply. That silence is the reason to catch them here rather than leave
     them to the device.

     Both read the same spec strings the inspector explains, so they stay
     right only as long as the tables at the top of this file match
     tag_table.c and custom_tags.c. */
  for (const n of allTags) {
    const spec = n.info.def[0] || "";
    const names = n.info.def[2] || [];
    const toks = specTokens(spec);
    const last = toks.length ? toks[toks.length - 1] : null;
    const nth = k => (names[k] ? '"' + names[k] + '"' : "argument " + (k + 1));
    const plural = k => k + " argument" + (k === 1 ? "" : "s");

    /* A. Argument count. The list may be left off entirely when the spec opens
          with '?', or when every argument is optional — otherwise a tag with
          required arguments and no '(' is ARGLIST_EXPECTED, too few is
          INSUFFICIENT_ARGS and too many is TOO_MANY_ARGS. A trailing '*'
          repeats the last token, so those specs can never overflow. */
    if (!n.hasArgList) {
      if (toks.required > 0 && !toks.qmark)
        add("err", n.s, "arg-count",
          "%" + n.name + " needs " + plural(toks.required) + " and is written with none. " +
          "Rockbox rejects the whole skin on this — write %" + n.name + "(" +
          (names.length ? names.slice(0, toks.required).join(",") : spec) + ").");
    } else if (n.args.length < toks.required) {
      add("err", n.s, "arg-count",
        "%" + n.name + " needs " + plural(toks.required) + " and only " + n.args.length +
        " " + (n.args.length === 1 ? "is" : "are") + " written — " + nth(n.args.length) +
        " is missing. There is no '|' before it in the spec (" + spec + "), so it is required, " +
        "and Rockbox rejects the whole skin rather than defaulting it.");
    } else if (!(last && last.repeat) && n.args.length > toks.length) {
      add("err", n.s, "arg-count",
        "%" + n.name + " takes at most " + plural(toks.length) + " and " + n.args.length +
        " are written. Rockbox rejects the whole skin on this.");
    }

    /* B. '-' means "use the default", and only a lowercase type code accepts
          it. A bracketed group always does: the parser guesses the code from
          the value written and lowercases it on the way, so [IP] takes '-'
          even though both letters are capitals. Only a bare capital refuses.
          A '-' followed by a digit is a negative number, not a default. */
    if (n.hasArgList) n.args.forEach((a, k) => {
      if (a.raw.trim() !== "-") return;
      const tok = toks[k] || (last && last.repeat ? last : null);
      if (!tok || tok.bracket || !/^[A-Z]$/.test(tok.t)) return;
      add("err", a.s, "default-not-allowed",
        "%" + n.name + " " + nth(k) + " has type '" + tok.t + "' — a capital, which rejects '-'. " +
        "Only a lowercase type code treats it as \"use the default\". Rockbox rejects the whole " +
        "skin on this, so write the value out.");
    });
  }

  /* ---------- semantic rules ----------
     From here on, each rule encodes something the parser accepts happily and
     the renderer then does something unhelpful with. */

  /* 1. A NOBREAK tag suppresses this line's break, so the next line overdraws it.
        line_number resets per viewport, so this only bites when another drawing
        line follows inside the SAME viewport — on a viewport's last line,
        suppressing the break changes nothing. */
  const vpEndOf = off => {
    let end = text.length;
    for (const v of vps) if (v.off > off && v.off < end) end = v.off;
    return end;
  };
  const drawingLines = [...physLines.entries()]
    .filter(([, r]) => r.draws.length).map(([k]) => k).sort((a, b) => a - b);
  for (const [ln, rec] of physLines) {
    if (!rec.nobreak.length || !rec.draws.length) continue;
    const next = drawingLines.find(k => k > ln);
    if (next === undefined) continue;
    if (ls[next] >= vpEndOf(rec.off)) continue;   /* next drawing line is a new viewport */
    add("warn", rec.off, "line-merge",
      "This line draws, and also carries %" + rec.nobreak[0] + ", which suppresses the line break — " +
      "line " + (next + 1) + " draws on top of it instead of below. Move %" + rec.nobreak[0] +
      " onto a line of its own.");
  }

  /* 2. Row tags need a listitem, which only exists inside a %Lb viewport group. */
  for (const v of vps) {
    if (!v.tags) continue;
    for (const [name, off] of v.tags) {
      if (!ROW_TAGS.has(name)) continue;
      if (v.label && lbLabels.has(v.label)) break;
      add("warn", off, "row-tag-outside-list",
        "%" + name + " only resolves while list rows are being drawn. This viewport " +
        (v.label ? '("' + v.label + '") is never named by a %Lb' : "has no label, so no %Lb can name it") +
        ", so %" + name + " finds no row and draws nothing.");
      break;
    }
  }

  /* 3. %Vd skips info viewports; %VI only sets one; %Lb wants row viewports. */
  const kindCheck = (nodes_, want, tagName, why) => {
    for (const n of nodes_) {
      const label = n.args[0].raw.trim();
      const kinds = labelKinds.get(label);
      if (!kinds || kinds.has(want)) continue;
      const got = kinds.has("ui") ? "%Vi" : "%Vl";
      add("warn", n.s, "label-kind-mismatch",
        "%" + tagName + "(" + label + ") names a viewport declared with " + got + ", but " + why);
    }
  };
  kindCheck(vdRefs, "cond", "Vd", "%Vd only shows %Vl viewports — it skips %Vi ones entirely.");
  kindCheck(viRefs, "ui", "VI", "%VI only selects %Vi list viewports.");
  kindCheck(lbRefs, "cond", "Lb", "%Lb draws its rows from %Vl viewports.");

  /* An art row shorter than an ordinary one is not an art row at all:
     skinlist_item_is_art_row() compares the two, so the list quietly falls back
     to plain rows and the art layout never appears. Equal is allowed and is the
     usual way to make one row's cover meet the next with no gap. */
  for (const n of lbRefs) {
    if (n.args.length < 5) continue;
    const rowH = parseInt(n.args[2].raw.trim(), 10);
    const artH = parseInt(n.args[4].raw.trim(), 10);
    if (!isNaN(rowH) && !isNaN(artH) && artH > 0 && artH < rowH)
      add("warn", n.s, "art-row-shorter-than-row",
        "%Lb art height " + artH + " is less than its row height " + rowH +
        " — the list then draws no art rows at all, and nothing on screen says so.");
  }

  /* 4. A font id nothing loaded falls back silently. */
  const fontRefs = [];
  for (const v of vps) if (!v.fontDefault && v.font !== null) fontRefs.push([v.font, v.off]);
  for (const n of twNodes) if (n.args.length > 1) {
    const id = parseInt(n.args[1].raw.trim(), 10);
    if (!isNaN(id)) fontRefs.push([id, n.s]);
  }
  const badFonts = new Set();
  for (const [id, off] of fontRefs) {
    if (id <= 1 || fonts[String(id)] || badFonts.has(id)) continue;
    badFonts.add(id);
    add("warn", off, "font-not-loaded",
      "Font id " + id + " is used but no %Fl loads it. Ids 0 and 1 are the built-in and UI fonts; " +
      "anything else has to be loaded first.");
  }

  /* 5. %Cd draws the slot %Cl claims. Without %Cl there is nothing to draw.
        (%Cl on its own is fine — it is a valid invisible loader.) */
  const clAt = text.indexOf("%Cl("), cdAt = text.indexOf("%Cd");
  if (cdAt >= 0 && clAt < 0)
    add("warn", cdAt, "cd-without-cl",
      "%Cd draws the album art box that %Cl claims, but this skin never calls %Cl — nothing will be drawn. " +
      "The same applies to 'dynamic colors', which needs a %Cl to claim the art slot.");

  /* 6. %sel evaluates its values for text; %Vd/%VI act by side effect. */
  for (const n of selNodes) {
    for (let k = 1; k < n.args.length; k++) {
      const raw = n.args[k].raw;
      if (!/%V[dI]\s*\(/.test(raw)) continue;
      add("warn", n.args[k].s, "vd-in-sel",
        "%sel evaluates its values for the text they produce, and %Vd/%VI produce none — they act by side effect. " +
        "Keep viewport selection in an ordinary %?if(...)<%Vd(A)|%Vd(B)> conditional.");
      break;
    }
  }

  /* 7. %cs is compared numerically against enum current_activity. */
  for (const n of csTests) {
    const v = parseInt(n.args[2].raw.trim(), 10);
    if (isNaN(v) || (v >= 0 && v <= ACTIVITY_MAX)) continue;
    add("warn", n.s, "cs-out-of-range",
      "%cs is compared against " + v + ", but the screen ids run 0–" + ACTIVITY_MAX + ". This test can never match.");
  }

  /* 8. %tw with no font id measures in the current viewport's font, which is
        not the font the text will be drawn in when the decision is made up front. */
  for (const n of twNodes) {
    if (n.args.length > 1) continue;
    const rec = physLines.get(lnOf(n.s));
    if (!rec) continue;
    const line = text.slice(ls[lnOf(n.s)], ls[lnOf(n.s) + 1] === undefined ? text.length : ls[lnOf(n.s) + 1]);
    if (!/%Vd\s*\(/.test(line)) continue;
    add("warn", n.s, "tw-without-font",
      "%tw here measures in the current viewport's font, but this line is choosing a layout with %Vd — " +
      "it does not run in the font the text will be drawn in. Pass the font id as a second argument.");
  }

  /* 9. %Cl's filter chain is compiled when the theme loads, so a typo in it
        discards the whole skin rather than drawing unfiltered art. */
  for (const n of clNodes) {
    if (n.args.length < 7) continue;
    const raw = n.args[6].raw.trim();
    const chain = parseFilterChain(raw);
    if (chain.error)
      add("err", n.args[6].s, "filter-chain",
        "%Cl's filter chain will not compile — " + chain.error + ". The chain is checked when the " +
        "theme loads, so this discards the whole skin, and there is no error on screen to say why.");
  }
  /* The palette dynamic colours use comes from the FIRST %Cl, and so does the
     filter chain the preview falls back on when a %Cd resolves to nothing. */
  let artFilters = arts.length ? arts[0].filters : [];

  /* %Cl's eighth argument is a corner radius, an integer. A label written
     there instead is the mistake this fork's ninth argument invites, and it
     discards the whole skin with nothing on screen to say why. */
  for (const n of clNodes) {
    if (n.args.length < 8) continue;
    const raw = n.args[7].raw.trim();
    if (raw === "" || raw === "-" || /^\d+$/.test(raw)) continue;
    add("err", n.args[7].s, "cl-radius",
      "%Cl's eighth argument is the corner radius and has to be a number or '-'. " +
      (n.args.length < 9
        ? "If '" + raw + "' is meant to be the label, it belongs in the NINTH argument: " +
          "%Cl(x,y,w,h,halign,valign,filters,radius,label) — write the radius as '-' to reach it."
        : "Arguments are positional; the label is the ninth.") +
      " A bad one discards the whole skin at load.");
  }

  /* Labels are per skin file. Reaching for one declared in the .wps from the
     .sbs (or the other way round) is the natural mistake, because the buffered
     pixels ARE shared -- but only by size, never by name. */
  for (const n of cdNodes) {
    if (!n.artLabel || n.artRef) continue;
    add("err", n.args[0].s, "cd-unknown-label",
      "No %Cl in this skin is called '" + n.artLabel + "'. Labels are local to one file — a .sbs " +
      "cannot draw a %Cl the .wps declared. Declare one here too: two %Cl at the same size share " +
      "the buffered bitmap between them, so the second costs nothing. An unresolved name discards " +
      "the whole skin at load.");
  }

  /* A window is all four or none; three describes no rectangle. */
  for (const n of cdNodes) {
    const given = [1, 2, 3, 4].filter(k => n.args.length > k && n.args[k].raw.trim() !== "-"
                                        && n.args[k].raw.trim() !== "").length;
    if (given === 0 || given === 4) continue;
    add("err", n.args[1] ? n.args[1].s : n.s, "cd-window",
      "%Cd's window needs all four of x, y, width and height — %Cd(label,x,y,w,h). " +
      given + " of them is not a rectangle, and the skin is discarded at load.");
  }

  /* The real budget is distinct SIZES, and it is shared with the other skin
     file, so even two here can be one too many. A blurred %Cl always counts
     separately: it claims a decimated source of its own. */
  {
    const sizes = new Set();
    for (const a of arts) {
      if (!a.box) continue;
      const blurred = a.filters.some(s => s.name === "blur");
      sizes.add(a.box.w + "x" + a.box.h + (blurred ? "/blur" : ""));
    }
    if (sizes.size > 2)
      add("warn", clNodes[clNodes.length - 1].s, "art-slots",
        "This skin asks for " + sizes.size + " different album art sizes and there are only two slots. " +
        "Whichever misses out draws nothing. Slots are claimed by SIZE, not per tag, so %Cl of matching " +
        "dimensions cost one between them — and the two are shared with the other skin file, which makes " +
        "even two here a risk. To show part of a size you already have, use a %Cd window instead of a " +
        "second %Cl.");
  }

  /* 10. The %dr tint and radius are this fork's, and both have ranges the
         parser rejects outright rather than clamping. */
  const tintsPerVp = new Map();
  for (const { node: n, vp } of drNodes) {
    const argRaw = k => (n.args[k] ? n.args[k].raw.trim() : "");
    const opacityRaw = argRaw(6), radiusRaw = argRaw(7);
    const opacity = opacityRaw === "" || opacityRaw === "-" ? 15 : parseInt(opacityRaw, 10);
    const radius = radiusRaw === "" || radiusRaw === "-" ? 0 : parseInt(radiusRaw, 10);
    const tinted = !isNaN(opacity) && opacity < 15;

    if (opacityRaw !== "" && opacityRaw !== "-" && (isNaN(opacity) || opacity < 0 || opacity > 15))
      add("err", n.args[6].s, "dr-range",
        "%dr opacity runs 0 (invisible) to 15 (opaque). " + JSON.stringify(opacityRaw) +
        " is rejected when the theme loads, taking the whole skin with it.");
    if (radiusRaw !== "" && radiusRaw !== "-" && (isNaN(radius) || radius < 0 || radius > 32))
      add("err", n.args[7].s, "dr-range",
        "%dr's corner radius stops at 32 — above that it is an error rather than a clamp, because the " +
        "mask grows as the square of the radius. " + JSON.stringify(radiusRaw) + " will not load.");

    /* A '-' colour is the viewport foreground as it stood when the parser
       reached this line, which below the declaration line is the theme's, not
       the viewport's — a white wash where a black blear was meant. */
    /* Only the start colour: a '-' end colour is how you say "no gradient". */
    if ((tinted || radius > 0) && argRaw(4) === "-")
      add("warn", n.s, "dr-dash-colour",
        "Write the fill colour out rather than passing '-'. A '-' takes the viewport foreground as it stood " +
        "when the parser reached this line, and %Vf only feeds into that on the viewport's own " +
        "declaration line — below it, '-' quietly takes the theme foreground. On a light-on-dark theme " +
        "that turns a scrim into a white wash. (It also crashes CheckWPS.)");

    /* Gradients lose to both of the new arguments. */
    if (argRaw(5) !== "" && argRaw(5) !== "-" && (tinted || radius > 0))
      add("warn", n.s, "dr-gradient-conflict",
        "This %dr gives an end colour and " + (tinted ? "an opacity" : "a radius") +
        ", which do not combine — the rectangle is filled with the start colour and the gradient is dropped.");

    if (tinted && vp && !vp.backdrop)
      add("warn", n.s, "dr-tint-outside-vb",
        "A tinted %dr in an ordinary viewport survives only until the first line of text crosses it, " +
        "because every text line repaints its own strip of background first. Put it in a %VB viewport, " +
        "in the same viewport as whatever it darkens.");

    if (tinted && vp) {
      const seen = (tintsPerVp.get(vp) || 0) + 1;
      tintsPerVp.set(vp, seen);
      if (seen === 9)
        add("warn", n.s, "dr-tint-limit",
          "This is the ninth tinted %dr in one viewport. Eight is the limit — the extras are dropped.");
    }
  }

  /* Every %Vd/%VI/%Lb site with the conditional branches it sits inside, so
     the preview can say which test kept a viewport off screen. */
  const refSites = [];
  const collectRefs = (list, path) => {
    for (const n of list) {
      if (n.type === "tag") {
        if ((n.name === "Vd" || n.name === "VI" || n.name === "Lb") && n.args[0])
          refSites.push({ label: n.args[0].raw.trim(), tag: n.name, off: n.s, path: path.slice() });
        for (const a of n.args) if (a.nodes) collectRefs(a.nodes, path);
      } else if (n.type === "cond") {
        if (n.subject) collectRefs([n.subject], path);
        n.branches.forEach((b, bi) => collectRefs(b.nodes, path.concat([{
          condOff: n.s, branch: bi, count: n.branches.length,
          subject: n.subject && n.subject.name ? n.subject.name : "?",
        }])));
      }
    }
  };
  collectRefs(nodes, []);

  /* ---------- where a %Lb row layout actually lands ----------
     list_skinned.c reads a row viewport's declared x/y as an offset inside the
     row, not as a screen position, and the row itself is offset from the %Vi
     list area. Written down, a row layout therefore reads as 0,0 — and drawing
     it there puts it in the top-left corner of the screen, the one place it
     never appears. Resolve the row origin here so the outline, the hit test and
     the viewport table all agree with the renderer. */
  const lbByLabel = new Map();
  for (const n of lbRefs) {
    const label = n.args[0].raw.trim();
    if (lbByLabel.has(label)) continue;      /* the first %Lb wins, as at runtime */
    const num = (k, d) => {
      const v = parseInt((n.args[k] ? n.args[k].raw : "").trim(), 10);
      return isNaN(v) ? d : v;
    };
    lbByLabel.set(label, {
      label, off: n.s, w: num(1, 0), h: num(2, 0),
      tile: (n.args[3] ? n.args[3].raw.trim().toLowerCase() : "") === "tile",
    });
  }
  for (const v of vps)
    if (v.kind === "cond" && v.label && lbByLabel.has(v.label)) v.row = lbByLabel.get(v.label);

  /* The list draws in one %Vi at a time — whichever %VI last named one. With
     no %VI at all, the unlabelled %Vi(-,…) is the default list area. */
  const uiVps = vps.filter(v => v.kind === "ui");
  let listArea = null;
  if (uiVps.length) {
    const named = viRefs.length ? viRefs[viRefs.length - 1].args[0].raw.trim() : null;
    listArea = (named && uiVps.find(v => v.label === named)) ||
               uiVps.find(v => v.label === "-") || uiVps[0];
  }
  if (lbByLabel.size && !listArea)
    add("warn", [...lbByLabel.values()][0].off, "lb-without-vi",
      "%Lb sets a row layout, but no %Vi declares the area the list draws in — the rows have " +
      "nothing to be positioned inside, so their coordinates are left as written.");

  /* Each %ft, for the .options checks, which need the file the skin names */
  const ftRefs = allTags.filter(n => n.name === "ft" && n.args.length >= 1).map(n => ({
    off: n.s, file: n.args[0].raw.trim(), key: n.args.length > 1 ? n.args[1].raw.trim() : null,
  }));

  issues.sort((a, b) => a.off - b.off);
  return { nodes, ranges, issues, vps, fonts, fontSites, images, text, refSites, listArea, artFilters, arts, ftRefs };
}

/* ---------- a theme's .options files ----------
   A copy of the firmware's parser, apps-ipod/skin/theme_options.c: the same
   rules, messages and line numbers. Change one and change the other.

   Returns { ok, title, help, options, errors }. `ok` is false for a file the
   player gives no menu (not format 1); `options` holds only the good ones,
   each { key, name, help, type, max, choices: [{ value, label }], def, line };
   each error is { line, msg }. */
function parseOptions(text) {
  const KEY = 32, NAME = 40, HELP = 1023, VALUE = 32, TEXT = 120,
        CHOICES = 16, MAX_OPTS = 32, LINE = 256;
  const enc = new TextEncoder();
  const bytes = s => enc.encode(s).length;
  const trim = s => s.replace(/^[ \t]+|[ \t]+$/g, "");
  const errors = [], options = [], seen = [];
  const head = { title: "", help: "" };
  let format = 0, headBad = false, cur = null, count = 0, lineNo = 0;
  let toggle = "", toggleLine = 0;

  const err = (line, msg) => errors.push({ line, msg });
  const drop = msg => { err(lineNo, msg); cur.bad = true; };
  const joinHelp = (h, t) => (h ? h + " " + t : t);

  /* The option ends: what can only be checked whole, at its heading */
  const finish = () => {
    if (!cur || cur.bad) return false;
    const o = cur.opt;
    let why = null;
    if (!cur.name || !cur.type || !cur.def)
      why = "option lacks name, type or default";
    else if (o.type === "choice") {
      if (cur.max) why = "max on a choice";
      else if (o.choices.length < 2) why = "fewer than two choices";
      else if (!optionValid(o, o.def)) why = "default is not a choice";
    }
    else if (o.choices.length > 0) why = "choice on a text option";
    else if (!optionValid(o, o.def)) why = "default is empty or longer than max";
    if (why) err(cur.line, why);
    return !why;
  };

  const heading = s => {
    cur = { bad: false, line: lineNo, name: false, type: false, def: false, max: false,
            opt: { key: "", name: "", help: "", type: null, max: 60, choices: [], def: "", line: lineNo } };
    const key = trim(s.slice(1, -1));
    if (count++ >= MAX_OPTS) return drop("more than 32 options");
    if (!/^[a-z0-9_-]([a-z0-9 _-]*[a-z0-9_-])?$/.test(key) || bytes(key) > KEY)
      return drop("bad key");
    if (seen.includes(key)) return drop("duplicate key");
    seen.push(key);
    cur.opt.key = key;
  };

  const field = (f, v) => {
    const o = cur.opt;
    if (f === "name") {
      if (bytes(v) > NAME || !v) drop("name empty or too long"); else o.name = v;
      cur.name = true;
    }
    else if (f === "help") {
      const h = joinHelp(o.help, v);
      if (bytes(h) > HELP) drop("help too long"); else o.help = h;
    }
    else if (f === "type") {
      if (v === "choice" || v === "text") o.type = v;
      else drop("type is not choice or text");
      cur.type = true;
    }
    else if (f === "choice") {
      const bar = v.indexOf("|");
      let value = trim(bar < 0 ? v : v.slice(0, bar));
      let label = bar < 0 ? "" : trim(v.slice(bar + 1));
      if (!label) label = value;
      if (o.choices.length >= CHOICES) drop("more than 16 choices");
      else if (!value || bytes(value) > VALUE || /[ \t]/.test(value))
        drop("choice value empty, too long or with spaces");
      else if (bytes(label) > NAME) drop("choice label too long");
      else o.choices.push({ value, label });
    }
    else if (f === "default") {
      if (bytes(v) > TEXT) drop("default too long"); else o.def = v;
      cur.def = true;
    }
    else if (f === "max") {
      const m = /^[+-]?\d+$/.test(v) ? parseInt(v, 10) : 0;
      if (m < 1 || m > TEXT) drop("max is not 1 to 120"); else o.max = m;
      cur.max = true;
    }
  };

  let lines = text.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  if (lines.length && lines[0].charCodeAt(0) === 0xfeff) lines[0] = lines[0].slice(1);

  for (const raw of lines) {
    lineNo++;
    const line = raw.replace(/\r/g, "");
    if (bytes(line) >= LINE - 1) {            /* a line too long is an error */
      if (cur) drop("line too long");
      else { err(lineNo, "line too long"); headBad = true; }
      continue;
    }
    const s = trim(line);
    if (!s || s[0] === "#") continue;
    if (s[0] === "[" && s[s.length - 1] === "]") {
      if (finish()) options.push(cur.opt);
      else if (!cur && (headBad || format !== 1)) {
        if (format !== 1) err(lineNo, "format is not 1, or comes after an option");
        return { ok: false, title: head.title, help: head.help, options: [], errors };
      }
      heading(s);
      continue;
    }
    const eq = s.indexOf("=");
    if (eq < 0) {
      if (cur) drop("not a heading, comment or field");
      else { err(lineNo, "not a heading, comment or field"); headBad = true; }
      continue;
    }
    const f = trim(s.slice(0, eq)), v = trim(s.slice(eq + 1));
    if (cur) { if (!cur.bad) field(f, v); }
    else if (f === "format") format = parseInt(v, 10) || 0;
    else if (f === "title") { if (bytes(v) <= NAME) head.title = v; }
    else if (f === "help") {
      const h = joinHelp(head.help, v);
      if (bytes(h) <= HELP) head.help = h;
    }
    else if (f === "toggle") {
      if (/^[a-z0-9_-]([a-z0-9 _-]*[a-z0-9_-])?$/.test(v) && bytes(v) <= KEY) {
        toggle = v;
        toggleLine = lineNo;
      }
      else err(lineNo, "toggle is not a key");
    }
  }
  if (!cur)
    return { ok: format === 1 && !headBad, title: head.title, help: head.help, options, errors };
  if (finish()) options.push(cur.opt);
  /* `toggle`: the choice option Theme Toggle changes until the owner picks one */
  if (toggle && !options.some(o => o.type === "choice" && o.key === toggle)) {
    err(toggleLine, "toggle names no good choice option");
    toggle = "";
  }
  return { ok: true, title: head.title, help: head.help, toggle, options, errors };
}

/* Whether `value` is one `opt` may have: a choice exactly, case and all, as a
   skin compares it; text not empty, within max, with no control characters */
function optionValid(opt, value) {
  if (opt.type === "choice") return opt.choices.some(c => c.value === value);
  return value.length > 0 && new TextEncoder().encode(value).length <= opt.max &&
         !/[\x00-\x1f\x7f]/.test(value);
}

/* parseOptions(), once per text: %ft reads it many times a render */
const optionsCache = new Map();
function optionsFor(text) {
  if (!optionsCache.has(text)) {
    if (optionsCache.size > 8) optionsCache.clear();
    optionsCache.set(text, parseOptions(text));
  }
  return optionsCache.get(text);
}
/* ============================================================
   Stage 4 — real glyphs, real bitmaps, evaluated tags.

   Font format per firmware/font.c: 36-byte header, then bits[bits_size],
   padding to a 2- or 4-byte boundary, offsets[noffset], widths[nwidth].
   ============================================================ */

const MAX_FONTSIZE_FOR_16_BIT_OFFSETS = 0xFFDB;

function parseFnt(buf) {
  const d = new DataView(buf);
  if (buf.byteLength < 36) throw new Error("too short for a font header");
  const magic = String.fromCharCode(d.getUint8(0), d.getUint8(1), d.getUint8(2), d.getUint8(3));
  if (magic !== "RB12") throw new Error("not a Rockbox font (magic " + JSON.stringify(magic) + ")");
  const f = {
    maxwidth: d.getUint16(4, true), height: d.getUint16(6, true),
    ascent:   d.getUint16(8, true), depth:  d.getUint16(10, true),
    firstchar: d.getUint32(12, true), defaultchar: d.getUint32(16, true),
    size: d.getUint32(20, true), bitsSize: d.getUint32(24, true),
    noffset: d.getUint32(28, true), nwidth: d.getUint32(32, true),
  };
  const bytes = new Uint8Array(buf);
  f.bits = bytes.subarray(36, 36 + f.bitsSize);

  let p = 36 + f.bitsSize;
  const longOffset = !(f.bitsSize < MAX_FONTSIZE_FOR_16_BIT_OFFSETS);
  p = longOffset ? (p + 3) & ~3 : (p + 1) & ~1;

  f.offsets = null;
  if (f.noffset) {
    f.offsets = new Array(f.noffset);
    for (let k = 0; k < f.noffset; k++)
      f.offsets[k] = longOffset ? d.getUint32(p + k * 4, true) : d.getUint16(p + k * 2, true);
    p += f.noffset * (longOffset ? 4 : 2);
  }
  f.widths = f.nwidth ? bytes.subarray(p, p + f.nwidth) : null;

  /* glyph_bytes() from font.c, rounded up to an even count */
  f.glyphBytes = w => {
    const r = f.depth ? ((f.height * w + 1) >> 1) : w * ((f.height + 7) >> 3);
    return (r + 1) & ~1;
  };
  f.glyph = ch => {
    let idx = ch - f.firstchar;
    if (idx < 0 || idx >= f.size) idx = f.defaultchar - f.firstchar;
    if (idx < 0 || idx >= f.size) return null;
    const w = f.widths ? f.widths[idx] : f.maxwidth;
    const off = f.offsets ? f.offsets[idx] : idx * f.glyphBytes(f.maxwidth);
    return { w, h: f.height, off };
  };
  /* Coverage 0..15 for a pixel, 15 being solid foreground.

     depth is 0 for a 1bpp font and 1 for a 4bpp antialiased one — it is a
     format code, not a bit count. 1bpp is vertical-within-byte, LSB at the
     top, and a set bit is foreground.

     The 4bpp nibble is stored the other way up: it is *transparency*, not
     coverage. lcd_alpha_bitmap_part() feeds it to blend_two_colors(px, fg, a),
     which returns fg at a = 0 and leaves the pixel alone at a = 15 — so 0 is a
     solid glyph pixel and 15 is clear. Read as coverage it draws every glyph
     as a filled block with the letter knocked out of it. */
  f.pixel = (g, x, y) => {
    if (f.depth) {
      const i = y * g.w + x;
      const b = f.bits[g.off + (i >> 1)];
      if (b === undefined) return 0;
      return 15 - ((i & 1) ? (b >> 4) & 0xf : b & 0xf);
    }
    const b = f.bits[g.off + (y >> 3) * g.w + x];
    if (b === undefined) return 0;
    return ((b >> (y & 7)) & 1) ? 15 : 0;
  };
  f.textWidth = s => {
    let w = 0;
    for (const c of String(s)) { const g = f.glyph(c.codePointAt(0)); if (g) w += g.w; }
    return w;
  };
  return f;
}

/* BMP loader. Rockbox skins use 1bpp (drawn in the viewport's fg/bg) and
   packed colour formats; both appear in this tree. */
function parseBmp(buf) {
  const d = new DataView(buf), u8 = new Uint8Array(buf);
  if (u8[0] !== 0x42 || u8[1] !== 0x4d) throw new Error("not a BMP");
  const dataOff = d.getUint32(10, true);
  const hdrSize = d.getUint32(14, true);
  const w = d.getInt32(18, true);
  const hRaw = d.getInt32(22, true);
  const h = Math.abs(hRaw), topDown = hRaw < 0;
  const bpp = d.getUint16(28, true);
  const compression = d.getUint32(30, true);
  let paletteCount = d.getUint32(46, true);
  if (!paletteCount && bpp <= 8) paletteCount = 1 << bpp;

  const palette = [];
  const palOff = 14 + hdrSize + (compression === 3 ? 12 : 0);
  for (let k = 0; k < paletteCount; k++) {
    const o = palOff + k * 4;
    palette.push([u8[o + 2], u8[o + 1], u8[o]]);
  }

  const rowSize = (((w * bpp + 31) >> 5) << 2);
  const px = new Uint8ClampedArray(w * h * 4);
  const mono = bpp === 1;

  for (let y = 0; y < h; y++) {
    const srcY = topDown ? y : h - 1 - y;
    const row = dataOff + srcY * rowSize;
    for (let x = 0; x < w; x++) {
      let r = 0, g = 0, b = 0, a = 255, idx = -1;
      if (bpp === 1)      idx = (u8[row + (x >> 3)] >> (7 - (x & 7))) & 1;
      else if (bpp === 4) idx = (x & 1) ? u8[row + (x >> 1)] & 0xf : u8[row + (x >> 1)] >> 4;
      else if (bpp === 8) idx = u8[row + x];
      else if (bpp === 16) {
        const v = d.getUint16(row + x * 2, true);
        r = ((v >> 11) & 0x1f) * 255 / 31; g = ((v >> 5) & 0x3f) * 255 / 63; b = (v & 0x1f) * 255 / 31;
      } else if (bpp === 24) { b = u8[row + x * 3]; g = u8[row + x * 3 + 1]; r = u8[row + x * 3 + 2]; }
      else if (bpp === 32) { b = u8[row + x * 4]; g = u8[row + x * 4 + 1]; r = u8[row + x * 4 + 2]; }
      if (idx >= 0) { const c = palette[idx] || [0, 0, 0]; r = c[0]; g = c[1]; b = c[2]; }
      const o = (y * w + x) * 4;
      px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
    }
  }
  return { w, h, px, mono, bpp };
}

/* ---------- evaluation ---------- */

const DEFAULT_STATE = {
  title: "Nightcall", artist: "Kavinsky", album: "OutRun", albumartist: "Kavinsky",
  genre: "Electronic", year: "2013", tracknum: "3", disc: "1", composer: "", comment: "",
  filename: "03 Nightcall", codec: "FLAC", bitrate: "1006", frequency: "44100", filesize: "32571",
  playback: 2, shuffle: false, repeat: 1, hold: false, artFound: true,
  elapsed: 84, length: 258, playlistPos: 3, playlistCount: 12,
  battery: 72, charging: false, usb: false, sleepTimer: 0, sleepLength: 30, building: false,
  screen: 2, listTitle: "Settings", listItem: "Playback Settings", selected: true,
  listRows: "Sound Settings\nPlayback Settings\nGeneral Settings\nTheme Settings\nManage Settings\nBrightness\nShuffle\nRepeat",
  listSel: 1, listStart: 0, listIcon: 3, listArtRows: false, artRowHeight: 0,
  hour: 14, minute: 35, second: 0, volume: -12, rating: 4, playcount: 7,
  volumeChanging: false, idle: 0,
};

const fmtTime = s => {
  s = Math.max(0, Math.round(s));
  const m = Math.floor(s / 60), r = s % 60;
  return m + ":" + String(r).padStart(2, "0");
};
const pad2 = n => String(n).padStart(2, "0");

/* Tags whose conditional value is an index rather than a boolean. */
const COND_INDEX = {
  mp: e => e.st.playback,
  mm: e => e.st.repeat,
  bl: e => Math.min(10, Math.floor(e.st.battery / 10)) + 1,
  la: e => (e.frame % 1000) + 1,
  cs: e => e.st.screen + 1,
  rr: e => e.st.rating + 1,
  /* Equal parts of 0-100, lowest first; no timer falls through to the
     empty-value rule and takes the last branch, as on the player. */
  bsp: (e, num) => { const p = sleepPercent(e.st);
    return p === "" ? -1 : Math.min(Math.floor(num * p / 100) + 1, num); },
};

/* %bsp: the timer left as a share of the length it started from, rounded up. */
function sleepPercent(st) {
  if (!st.sleepTimer) return "";
  const len = Math.max(st.sleepTimer, st.sleepLength || 0);
  return Math.min(100, Math.ceil(st.sleepTimer * 100 / len));
}

/* Branch selection, mirroring evaluate_conditional() in skin_display.c.
   Returns a 0-based branch index, or -1 meaning "draw nothing".
   Note a false boolean picks the LAST branch, not the second — and with a
   single branch it picks nothing at all. */
function condBranchIndex(n, env) {
  const s = n.subject;
  const num = n.branches.length;
  if (!s || !s.name || !num) return -1;
  let intval = COND_INDEX[s.name] ? COND_INDEX[s.name](env, num) : -1;
  if (intval === -1) {
    const v = evalTag(s, env);
    if (num === 1) intval = v !== "" ? 1 : 0;
    else intval = v !== "" ? 1 : num;
  } else if (intval > num || intval < 1) {
    intval = num;
  }
  return intval - 1;
}

function evalNodes(nodes, env) {
  let out = "";
  for (const n of nodes) out += evalNode(n, env);
  return out;
}

function evalNode(n, env) {
  if (!n) return "";
  if (n.type === "text") return n.text;
  if (n.type === "comment" || n.type === "nl" || n.type === "subline") return "";
  if (n.type === "cond") {
    const idx = condBranchIndex(n, env);
    return idx < 0 ? "" : evalNodes(n.branches[idx].nodes, env);
  }
  if (n.type === "tag") return evalTag(n, env);
  return "";
}

const argStr = (n, k, env) =>
  n.args && n.args[k]
    ? (n.args[k].nodes ? evalNodes(n.args[k].nodes, env) : n.args[k].raw.trim())
    : "";
const argNum = (n, k, env) => { const v = parseFloat(argStr(n, k, env)); return isNaN(v) ? 0 : v; };

/* The mock list, one entry per line.
   A row is its text, optionally followed by "| n" giving the icon id %LI
   returns for it — which is how a file browser row says "folder" where the one
   below it says "audio file". Without one, every row reports the same icon,
   and a layout that picks its glyph by %LI can only ever draw the default. */
function listRowData(st) {
  return String(st.listRows || "").split("\n")
    .filter(s => s.trim() !== "")
    .map(line => {
      const bar = line.lastIndexOf("|");
      const icon = bar < 0 ? "" : line.slice(bar + 1).trim();
      if (bar < 0 || !/^-?\d+$/.test(icon)) return { text: line.trim(), icon: null };
      return { text: line.slice(0, bar).trim(), icon };
    });
}

/* %LT / %LI / %La take a row offset from the one being drawn — 0 is that row —
   and wrap round the ends of the list unless 'nowrap' is given. */
function rowAt(n, env) {
  if (!env.row) return null;
  const rows = env.rows || [];
  if (!rows.length) return null;
  let off = 0;
  if (n.args && n.args.length) {
    const v = parseInt(argStr(n, 0, env), 10);
    if (!isNaN(v)) off = v;
  }
  const nowrap = n.args && n.args[1] &&
                 n.args[1].raw.trim().toLowerCase() === "nowrap";
  let i = env.row.index + off;
  if (nowrap) return (i < 0 || i >= rows.length) ? null : rows[i];
  i = ((i % rows.length) + rows.length) % rows.length;
  return rows[i];
}

function wrapText(text, width, font) {
  const words = String(text).split(/\s+/).filter(Boolean);
  const lines = [];
  let cur = "";
  for (const w of words) {
    const cand = cur ? cur + " " + w : w;
    if (font && font.textWidth(cand) > width && cur) { lines.push(cur); cur = w; }
    else cur = cand;
  }
  if (cur) lines.push(cur);
  return lines.length ? lines : [""];
}

function evalTag(n, env) {
  const st = env.st, name = n.name;
  switch (name) {
    /* metadata */
    case "it": case "It": return st.title;
    case "ia": case "Ia": return st.artist;
    case "id": case "Id": return st.album;
    case "iA": case "IA": return st.albumartist;
    case "ig": case "Ig": return st.genre;
    case "iy": case "Iy": return st.year;
    case "in": case "In": return st.tracknum;
    case "ik": case "Ik": return st.disc;
    case "ic": case "Ic": return st.composer;
    case "iC": case "IC": return st.comment;
    case "fn": case "Fn": return st.filename;
    case "fm": case "Fm": return st.filename + ".flac";
    case "fc": case "Fc": return st.codec;
    case "fb": case "Fb": return st.bitrate;
    case "ff": case "Ff": return st.frequency;
    case "fk": case "Fk": return String(Math.round(+st.frequency / 1000));
    case "fs": case "Fs": return st.filesize;
    case "fp": case "Fp": return "/Music/" + st.artist + "/" + st.album + "/" + st.filename;
    /* playback */
    case "mp": return String(st.playback);
    case "mm": return String(st.repeat);
    case "ps": return st.shuffle ? "s" : "";
    case "mh": return st.hold ? "h" : "";
    case "pc": return fmtTime(st.elapsed);
    case "pr": return fmtTime(st.length - st.elapsed);
    case "pt": case "Pt": return fmtTime(st.length);
    case "px": return String(Math.round(st.elapsed * 100 / Math.max(1, st.length)));
    case "pp": return String(st.playlistPos);
    case "pe": return String(st.playlistCount);
    case "pn": return "Now Playing";
    case "C": return st.artFound ? "c" : "";
    case "rp": return String(st.playcount);
    case "rr": return String(st.rating);
    case "xf": return "";
    /* status */
    case "bl": return String(st.battery);
    case "bc": return st.charging ? "c" : "";
    case "bp": return st.charging ? "p" : "";
    case "bu": return st.usb ? "u" : "";
    case "bs": return st.sleepTimer ? fmtTime(st.sleepTimer * 60) : "";
    case "bsp": return String(sleepPercent(st));
    /* The lens plays no audiobook */
    case "prb": case "pbp": return "";
    case "bt": return "5:20";
    case "bv": return "3.9";
    case "lh": return "";
    case "lb": return st.building ? "b" : "";
    case "lw": return "";
    case "la": return String(env.frame % 1000);
    case "mv": return st.volumeChanging ? "v" : "";
    case "Tl": return (st.idle || 0) < (argNum(n, 0, env) || 0) ? "t" : "";
    case "cs": return String(st.screen);
    /* clock */
    case "ck": return String(st.hour);
    case "cH": return pad2(st.hour);
    case "cl": return String(((st.hour + 11) % 12) + 1);
    case "cI": return pad2(((st.hour + 11) % 12) + 1);
    case "cM": return pad2(st.minute);
    case "cS": return pad2(st.second);
    case "cP": return st.hour < 12 ? "AM" : "PM";
    case "cp": return st.hour < 12 ? "am" : "pm";
    case "cf": return "";
    /* list */
    case "Lt": return st.listTitle;
    case "LT": {
      const r = rowAt(n, env);
      return r ? r.text : (env.row ? "" : st.listItem);
    }
    case "Lc": return env.row ? (env.row.selected ? "s" : "") : (st.selected ? "s" : "");
    case "LN": return env.row ? String(env.row.index + 1) : "1";
    case "LR": return env.row ? String(env.row.index + 1) : "1";
    case "LC": return "1";
    case "Li": return String(st.listIcon);          /* the title's icon, not a row's */
    case "LI": {
      const r = rowAt(n, env);
      if (!r) return env.row ? "" : String(st.listIcon);
      return r.icon === null ? String(st.listIcon) : r.icon;
    }
    case "Lk": {
      const r = rowAt(n, env);
      return r && r.kind !== undefined ? String(r.kind) : "0";
    }
    case "Lp": {
      const r = rowAt(n, env);
      return r && r.kind === 7 && r.track !== undefined ? String(r.track) : "";
    }
    case "Ls": {
      const r = rowAt(n, env);
      return r && r.kind === 7 && r.disc ? String(r.disc) : "";
    }
    case "Lv": { const r = rowAt(n, env); return r && r.value ? r.value : ""; }
    case "Lr": { const r = rowAt(n, env); return r && r.range !== undefined ? String(r.range) : ""; }
    case "LP": { const r = rowAt(n, env); return r && r.playing ? "p" : ""; }
    case "La": return st.listArtRows ? "a" : "";
    case "LB": return "s";
    /* viewport metrics */
    case "Vw": return String(env.vp ? env.vp.w : LCD_W);
    case "Vh": return String(env.vp ? env.vp.h : LCD_H);
    /* logic + strings */
    case "if": {
      const a = argStr(n, 0, env), op = argStr(n, 1, env), b = argStr(n, 2, env);
      const na = parseFloat(a), nb = parseFloat(b);
      const numeric = !isNaN(na) && !isNaN(nb);
      let r;
      switch (op) {
        case "=": case "==": r = numeric ? na === nb : a === b; break;
        case "!=": r = numeric ? na !== nb : a !== b; break;
        case ">": r = numeric ? na > nb : a > b; break;
        case "<": r = numeric ? na < nb : a < b; break;
        case ">=": r = numeric ? na >= nb : a >= b; break;
        case "<=": r = numeric ? na <= nb : a <= b; break;
        default: r = false;
      }
      return r ? "t" : "";
    }
    case "and": {
      for (let k = 0; k < n.args.length; k++) if (argStr(n, k, env) === "") return "";
      return n.args.length ? "t" : "";
    }
    case "or": {
      for (let k = 0; k < n.args.length; k++) if (argStr(n, k, env) !== "") return "t";
      return "";
    }
    case "sel": {
      const subject = argStr(n, 0, env);
      const rest = n.args.length - 1;
      const pairs = Math.floor(rest / 2);
      for (let k = 0; k < pairs; k++)
        if (argStr(n, 1 + k * 2, env) === subject) return argStr(n, 2 + k * 2, env);
      return (rest % 2) ? argStr(n, n.args.length - 1, env) : "";
    }
    case "ss": {
      const start = argNum(n, 0, env);
      const lenRaw = argStr(n, 1, env);
      const src = argStr(n, 2, env);
      const len = (lenRaw === "" || lenRaw === "-") ? src.length : parseInt(lenRaw, 10);
      return src.substr(start, len);
    }
    case "trm": {
      const set = Array.from(argStr(n, 1, env));
      const cs = Array.from(argStr(n, 0, env));
      let a = 0, b = cs.length;
      while (a < b && set.includes(cs[a])) a++;
      while (b > a && set.includes(cs[b - 1])) b--;
      return cs.slice(a, b).join("");
    }
    case "sl": return String(argStr(n, 0, env).length);
    case "sf": return String(argStr(n, 0, env).indexOf(argStr(n, 1, env)));
    case "pd": {
      const w = argNum(n, 0, env), s = argStr(n, 1, env);
      return s.length >= w ? s.slice(0, w) : s + " ".repeat(w - s.length);
    }
    case "ma": {
      const a = argNum(n, 0, env), op = argStr(n, 1, env), b = argNum(n, 2, env);
      let r = 0;
      if (op === "+") r = a + b; else if (op === "-") r = a - b;
      else if (op === "*") r = a * b;
      else if (op === "/") r = b ? Math.trunc(a / b) : 0;
      else if (op === "%") r = b ? a % b : 0;
      return String(Math.trunc(r));
    }
    case "tw": {
      const s = argStr(n, 0, env);
      const fid = n.args.length > 1 ? parseInt(argStr(n, 1, env), 10) : null;
      const f = env.fontFor(fid === null || isNaN(fid) ? (env.vp ? env.vp.font : null) : fid);
      return String(f ? f.textWidth(s) : s.length * 6);
    }
    case "wr": {
      const idx = argNum(n, 0, env), s = argStr(n, 1, env);
      const f = env.fontFor(env.vp ? env.vp.font : null);
      const lines = wrapText(s, env.vp ? env.vp.w : LCD_W, f);
      return lines[idx] || "";
    }
    case "ft": {
      const file = argStr(n, 0, env), key = argStr(n, 1, env).trim();
      return env.readKey ? env.readKey(file, key) : "";
    }
    case "St": return env.setting ? env.setting(argStr(n, 0, env)) : "";
    case "Sx": return argStr(n, 0, env);
    case "Sr": return "";
    case "vg": return env.vars && env.vars[argStr(n, 0, env)] !== undefined ? String(env.vars[argStr(n, 0, env)]) : "";
    default:
      return "";
  }
}

/* ---------- reading an argument spec ----------
   The strings in the tag tables are the parser's own. Split one into a token
   per argument so each argument can say what it accepts: '|' marks where the
   optional ones begin, '*' repeats the token before it, '?' means the whole
   list may be left off, and a bracketed group is a choice.

   Two properties are hung on the returned array for the argument-count and
   '-' rules below: `required` (how many must be written) and `qmark` (the '?'
   prefix, which lets the whole argument list be omitted). */
function specTokens(spec) {
  const out = [];
  let optionalFrom = Infinity, qmark = false;
  for (let i = 0; i < spec.length; i++) {
    const c = spec[i];
    if (c === "?") { qmark = true; continue; }
    if (c === "|") { optionalFrom = out.length; continue; }
    if (c === "*") { if (out.length) out[out.length - 1].repeat = true; continue; }
    if (c === "[") {
      const j = spec.indexOf("]", i);
      if (j < 0) break;
      out.push({ t: spec.slice(i + 1, j), bracket: true });
      i = j;
      continue;
    }
    out.push({ t: c, bracket: false });
  }
  out.forEach((o, i) => { o.optional = i >= optionalFrom; });
  out.required = Math.min(optionalFrom, out.length);
  out.qmark = qmark;
  return out;
}

const SPEC_LETTER = { I: "integer", D: "decimal", P: "percentage", S: "string",
                      F: "filename", T: "tag", C: "code block" };

/* ---------- pixel surface ---------- */
function makeSurface() {
  const px = new Uint8ClampedArray(LCD_W * LCD_H * 4);
  px.fill(255);
  return {
    px,
    clip: null,       /* { x0, y0, x1, y1 }: the viewport being drawn */
    set(x, y, r, g, b, a) {
      if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
      const c = this.clip;
      if (c && (x < c.x0 || y < c.y0 || x >= c.x1 || y >= c.y1)) return;
      const o = (y * LCD_W + x) * 4;
      if (a >= 255) { px[o] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = 255; return; }
      if (a <= 0) return;
      const k = a / 255;
      px[o] = px[o] * (1 - k) + r * k;
      px[o + 1] = px[o + 1] * (1 - k) + g * k;
      px[o + 2] = px[o + 2] * (1 - k) + b * k;
      px[o + 3] = 255;
    },
    fill(x, y, w, h, c) {
      for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) this.set(x + i, y + j, c[0], c[1], c[2], 255);
    },
    /* %dr's rectangle, with this fork's two extra arguments. A tint reads the
       screen as well as writing it; a radius blends the corners into whatever
       is already underneath, which is what makes a rounded panel read as
       floating over the artwork rather than cut out of a flat colour. */
    panel(x, y, w, h, c, opacity, radius) {
      if (w <= 0 || h <= 0) return;
      const a = Math.max(0, Math.min(15, opacity === undefined ? 15 : opacity)) / 15 * 255;
      const r = Math.max(0, Math.min(radius || 0, w >> 1, h >> 1));
      for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
        let cov = 1;
        if (r > 0) {
          const dx = i < r ? r - i - 0.5 : (i >= w - r ? i - (w - r) + 0.5 : 0);
          const dy = j < r ? r - j - 0.5 : (j >= h - r ? j - (h - r) + 0.5 : 0);
          if (dx > 0 && dy > 0) cov = Math.max(0, Math.min(1, r - Math.sqrt(dx * dx + dy * dy) + 0.5));
        }
        if (cov > 0) this.set(x + i, y + j, c[0], c[1], c[2], a * cov);
      }
    },
  };
}

/* ---------- %Cl's filter chain, on the stand-in artwork ----------
   An approximation of apps-ipod/draw/img_filter.c: the same stage order
   (spatial → colour → levels → dither) and the same amounts, worked in 8 bits
   rather than the display's field widths. It shows what a chain does to
   contrast, colour and detail — which is what you are judging when you decide
   whether text will read over it — not the exact pixels the device produces. */
function matMul(a, b) {
  const o = new Array(9);
  for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++)
    o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
  return o;
}
/* Rec. 601 luma, the weights img_filter.c desaturates and rotates around. */
const LUMA = [0.299, 0.587, 0.114];
function satMatrix(s) {
  const o = new Array(9);
  for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++)
    o[r * 3 + c] = LUMA[c] * (1 - s) + (r === c ? s : 0);
  return o;
}
function hueMatrix(deg) {
  const a = deg * Math.PI / 180, cos = Math.cos(a), sin = Math.sin(a);
  const o = new Array(9);
  for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++) {
    let v = LUMA[c] + cos * ((r === c ? 1 : 0) - LUMA[c]);
    v += sin * (r === c ? -LUMA[c] : (((r + 1) % 3) === c ? LUMA[(r + 2) % 3] : -LUMA[(r + 1) % 3]));
    o[r * 3 + c] = v;
  }
  return o;
}

/* An in-place box blur on a decimated copy, the way the device does it: the
   picture is shrunk, blurred small and scaled back, which is why a blur is the
   cheapest chain that touches every pixel rather than the dearest. */
function boxBlur(px, w, h, radius) {
  const tmp = new Float32Array(px.length);
  const pass = (src, dst, dx, dy) => {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let r = 0, g = 0, b = 0, n = 0;
      for (let k = -radius; k <= radius; k++) {
        const sx = Math.min(w - 1, Math.max(0, x + k * dx));
        const sy = Math.min(h - 1, Math.max(0, y + k * dy));
        const o = (sy * w + sx) * 3;
        r += src[o]; g += src[o + 1]; b += src[o + 2]; n++;
      }
      const o = (y * w + x) * 3;
      dst[o] = r / n; dst[o + 1] = g / n; dst[o + 2] = b / n;
    }
  };
  pass(px, tmp, 1, 0);
  pass(tmp, px, 0, 1);
}

function blockAverage(px, w, h, block) {
  for (let by = 0; by < h; by += block) for (let bx = 0; bx < w; bx += block) {
    const x1 = Math.min(w, bx + block), y1 = Math.min(h, by + block);
    let r = 0, g = 0, b = 0, n = 0;
    for (let y = by; y < y1; y++) for (let x = bx; x < x1; x++) {
      const o = (y * w + x) * 3;
      r += px[o]; g += px[o + 1]; b += px[o + 2]; n++;
    }
    r /= n; g /= n; b /= n;
    for (let y = by; y < y1; y++) for (let x = bx; x < x1; x++) {
      const o = (y * w + x) * 3;
      px[o] = r; px[o + 1] = g; px[o + 2] = b;
    }
  }
}

const BAYER8 = (() => {
  const m = [[0]];
  let n = 1;
  let cur = m;
  while (n < 8) {
    const next = [];
    for (let y = 0; y < n * 2; y++) next.push(new Array(n * 2));
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
      const v = cur[y][x] * 4;
      next[y][x] = v; next[y][x + n] = v + 2;
      next[y + n][x] = v + 3; next[y + n][x + n] = v + 1;
    }
    cur = next; n *= 2;
  }
  return cur;
})();

function applyImgFilters(px, w, h, steps) {
  if (!steps || !steps.length) return;

  /* spatial */
  for (const s of steps) {
    if (s.name === "pixellate") blockAverage(px, w, h, s.amount);
    else if (s.name === "blur") boxBlur(px, w, h, Math.max(1, Math.round(s.amount)));
  }

  /* colour — every step in this stage is a 3×3 matrix, so the chain folds into
     one before a single pixel is touched. */
  let mat = null;
  for (const s of steps) {
    let m = null;
    if (s.name === "bw") m = satMatrix(1 - s.amount / 100);
    else if (s.name === "saturate") m = satMatrix(1 + s.amount / 100);
    else if (s.name === "hue") m = hueMatrix(s.amount);
    if (m) mat = mat ? matMul(m, mat) : m;
  }
  if (mat) {
    for (let o = 0; o < px.length; o += 3) {
      const r = px[o], g = px[o + 1], b = px[o + 2];
      px[o]     = mat[0] * r + mat[1] * g + mat[2] * b;
      px[o + 1] = mat[3] * r + mat[4] * g + mat[5] * b;
      px[o + 2] = mat[6] * r + mat[7] * g + mat[8] * b;
    }
  }

  /* levels — likewise, one lookup table for the whole stage. `lighter` and
     `darker` with no amount aim at a fixed mean brightness instead of removing
     a fixed amount, which is why they suit a cover whose brightness you cannot
     know in advance. */
  const levels = steps.filter(s => ["invert","brightness","lighter","darker","contrast","reduce"].includes(s.name));
  if (levels.length) {
    let mean = 0;
    for (let o = 0; o < px.length; o += 3)
      mean += (LUMA[0] * px[o] + LUMA[1] * px[o + 1] + LUMA[2] * px[o + 2]);
    mean /= (px.length / 3);

    const lut = new Uint8ClampedArray(256);
    for (let i = 0; i < 256; i++) {
      let v = i;
      for (const s of levels) {
        switch (s.name) {
          case "invert": v = 255 - v; break;
          case "brightness":
            v += s.amount > 0 ? (255 - v) * s.amount / 100 : v * s.amount / 100;
            break;
          case "lighter": case "darker": {
            const sign = s.name === "darker" ? -1 : 1;
            /* Adaptive: how far this picture is from the mean it is aimed at. */
            const target = s.name === "darker" ? 90 : 165;
            const pct = s.adaptive
              ? Math.max(0, Math.min(100, sign > 0 ? (target - mean) * 100 / 255 : (mean - target) * 100 / 255))
              : s.amount;
            v += sign > 0 ? (255 - v) * pct / 100 : -v * pct / 100;
            break;
          }
          case "contrast": {
            const k = 1 + s.amount / 100;
            v = 128 + (v - 128) * k;
            break;
          }
          case "reduce": {
            const n = Math.max(2, s.amount);
            v = Math.round(Math.round(v * (n - 1) / 255) * 255 / (n - 1));
            break;
          }
        }
        v = Math.max(0, Math.min(255, v));
      }
      lut[i] = v;
    }
    for (let o = 0; o < px.length; o++) px[o] = lut[Math.max(0, Math.min(255, Math.round(px[o])))];
  }

  /* scrim: fold_cap() in img_filter.c. Only the highlights come down, along a
     curve from a knee to a ceiling set by the text colour, so text of that
     luminance reads at 4.5:1 over the brightest of the picture. Text below
     half scale gets none. */
  const scrim = steps.find(s => s.name === "scrim");
  if (scrim) {
    const avoid = Math.min(255, scrim.textLum !== null && scrim.textLum !== undefined ? scrim.textLum : 255);
    if (2 * avoid >= 255) {
      const FULL = 65025, P5 = 3251;
      const ceiling = Math.trunc((avoid * avoid + P5) * 100 / 450) - P5;
      const knee = Math.trunc(ceiling * 40 / 100);
      const scale = new Float32Array(256);
      for (let b = 0; b < 256; b++) {
        const top = Math.min(b * 256 + 255, FULL), d = top - knee, room = ceiling - knee;
        const want = knee + Math.trunc(d * room * (FULL - knee) /
                                       (room * (FULL - knee) + d * (FULL - ceiling)));
        scale[b] = top <= knee ? 1 : Math.sqrt(want / top);
      }
      for (let o = 0; o < px.length; o += 3) {
        const r = px[o], g = px[o + 1], b = px[o + 2];
        const lum = (r * r * 13933 + g * g * 46871 + b * b * 4732) / 65536;
        if (lum <= knee) continue;
        const k = scale[Math.min(FULL, Math.floor(lum)) >> 8];
        px[o] = r * k; px[o + 1] = g * k; px[o + 2] = b * k;
      }
    }
  }

  /* dither, last, on the values the display will actually hold */
  if (steps.some(s => s.name === "dither")) {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const bias = (BAYER8[y & 7][x & 7] / 64 - 0.5) * 8;
      const o = (y * w + x) * 3;
      for (let c = 0; c < 3; c++) px[o + c] = Math.max(0, Math.min(255, px[o + c] + bias));
    }
  }
}

/* A leading '!' marks the colour COLOR_FIXED (apps-ipod/draw/color.h): the
   dynamic-colour palette leaves it as written. The preview does not apply that
   palette at all, so here the prefix only has to be accepted and dropped.
   The palette words `accent`, `dominant` and `vivid` draw as their `:RRGGBB`
   fallback, exactly as written, or without one as the theme's foreground and
   background (taken here as black and white), then bounded and shaded. A
   `>NN`/`<NN` bound on a grey is the grey at that tone, on the player's
   luminance (gamma 2.0), which is all a theme colour here can be. */
const PALETTE_WORD = /^(accent|dominant|vivid)(?:([<>])(\d{1,3}))?(?:\.(\d{1,3}))?(?::([0-9a-f]{6}))?$/i;
const toneGrey = t => {
  const y = t <= 8 ? t * 27 / 24389 : Math.pow((t + 16) / 116, 3);
  return Math.round(255 * Math.sqrt(y));
};
const greyTone = v => {
  const y = (v / 255) * (v / 255);
  return y > 216 / 24389 ? 116 * Math.cbrt(y) - 16 : y * 24389 / 27;
};
const hexToRgbAsWritten = s => {
  const w = PALETTE_WORD.exec(String(s).trim());
  if (w) {
    if (w[5] !== undefined) {
      const f = parseInt(w[5], 16);
      return [(f >> 16) & 255, (f >> 8) & 255, f & 255];
    }
    let v = w[1].toLowerCase() === "dominant" ? 255 : 0;
    if (w[2] !== undefined) {
      const t = Math.min(100, +w[3]);
      if (w[2] === ">" ? greyTone(v) < t : greyTone(v) > t) v = toneGrey(t);
    }
    if (w[4] !== undefined) v = Math.round(v * Math.min(100, +w[4]) / 100);
    return [v, v, v];
  }
  const m = /^!?([0-9a-f]{6})$/i.exec(String(s).trim().replace(/^#/, ""));
  if (!m) return null;
  const v = parseInt(m[1], 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
};
const hexToRgb = hexToRgbAsWritten;

function blitGlyph(surf, font, ch, x, y, fg) {
  const g = font.glyph(ch);
  if (!g) return 0;
  for (let j = 0; j < g.h; j++)
    for (let i = 0; i < g.w; i++) {
      const a = font.pixel(g, i, j);
      if (a) surf.set(x + i, y + j, fg[0], fg[1], fg[2], Math.round(a * 255 / 15));
    }
  return g.w;
}

function drawString(surf, font, s, x, y, fg) {
  let cx = x;
  for (const c of String(s)) cx += blitGlyph(surf, font, c.codePointAt(0), cx, y, fg);
  return cx - x;
}

/* Mono bitmaps follow the viewport's fg/bg. Colour bitmaps key magenta out. */
function blitBmp(surf, bmp, x, y, fg, bg) {
  for (let j = 0; j < bmp.h; j++)
    for (let i = 0; i < bmp.w; i++) {
      const o = (j * bmp.w + i) * 4;
      const r = bmp.px[o], g = bmp.px[o + 1], b = bmp.px[o + 2];
      if (bmp.mono) {
        const on = r < 128;
        if (on) surf.set(x + i, y + j, fg[0], fg[1], fg[2], 255);
        else if (bg) surf.set(x + i, y + j, bg[0], bg[1], bg[2], 255);
      } else {
        if (r > 240 && g < 16 && b > 240) continue;   /* magenta = transparent */
        surf.set(x + i, y + j, r, g, b, 255);
      }
    }
}


/* ---------- album art ----------
   A picture, scaled the way the player scales art into a %Cl box: fitted
   inside it with its proportions kept, and placed by the box's alignment.
   Scaling and the filter chain are worked out once per size and chain, since
   a blurred cover is the dearest thing on the screen. */
class Art {
  constructor(w, h, rgba) {
    this.w = w; this.h = h; this.rgba = rgba;
    this.cache = new Map();
  }

  /* RGB floats at w x h: area-averaged going down, bilinear going up. */
  scaled(w, h) {
    const out = new Float32Array(w * h * 3), src = this.rgba, sw = this.w, sh = this.h;
    const kx = sw / w, ky = sh / h;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let r = 0, g = 0, b = 0;
      if (kx > 1 || ky > 1) {
        const x0 = Math.floor(x * kx), x1 = Math.max(x0 + 1, Math.floor((x + 1) * kx));
        const y0 = Math.floor(y * ky), y1 = Math.max(y0 + 1, Math.floor((y + 1) * ky));
        let n = 0;
        for (let j = y0; j < y1 && j < sh; j++) for (let i = x0; i < x1 && i < sw; i++) {
          const o = (j * sw + i) * 4;
          r += src[o]; g += src[o + 1]; b += src[o + 2]; n++;
        }
        r /= n; g /= n; b /= n;
      } else {
        const fx = Math.max(0, (x + 0.5) * kx - 0.5), fy = Math.max(0, (y + 0.5) * ky - 0.5);
        const ix = Math.min(sw - 1, Math.floor(fx)), iy = Math.min(sh - 1, Math.floor(fy));
        const ax = fx - ix, ay = fy - iy;
        const ix2 = Math.min(sw - 1, ix + 1), iy2 = Math.min(sh - 1, iy + 1);
        for (let c = 0; c < 3; c++) {
          const p = (j, i) => src[(j * sw + i) * 4 + c];
          const v = (p(iy, ix) * (1 - ax) + p(iy, ix2) * ax) * (1 - ay) +
                    (p(iy2, ix) * (1 - ax) + p(iy2, ix2) * ax) * ay;
          if (c === 0) r = v; else if (c === 1) g = v; else b = v;
        }
      }
      const o = (y * w + x) * 3;
      out[o] = r; out[o + 1] = g; out[o + 2] = b;
    }
    return out;
  }

  /* The box's pixels, with `mask` marking the ones the picture covers. */
  fitted(bw, bh, halign, valign, chain) {
    const key = bw + "x" + bh + halign + valign + JSON.stringify(chain || []);
    if (this.cache.has(key)) return this.cache.get(key);
    const k = Math.min(bw / this.w, bh / this.h);
    const dw = Math.max(1, Math.round(this.w * k)), dh = Math.max(1, Math.round(this.h * k));
    const dx = halign === "l" ? 0 : halign === "r" ? bw - dw : (bw - dw) >> 1;
    const dy = valign === "t" ? 0 : valign === "b" ? bh - dh : (bh - dh) >> 1;
    const pic = this.scaled(dw, dh);
    if (chain && chain.length) applyImgFilters(pic, dw, dh, chain);
    const px = new Float32Array(bw * bh * 3), mask = new Uint8Array(bw * bh);
    for (let y = 0; y < dh; y++) for (let x = 0; x < dw; x++) {
      const tx = x + dx, ty = y + dy;
      if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
      const o = (ty * bw + tx) * 3, p = (y * dw + x) * 3;
      px[o] = pic[p]; px[o + 1] = pic[p + 1]; px[o + 2] = pic[p + 2];
      mask[ty * bw + tx] = 1;
    }
    const out = { px, mask };
    this.cache.set(key, out);
    return out;
  }

  draw(surf, x, y, w, h, halign, valign, chain) {
    const f = this.fitted(w, h, halign, valign, chain);
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
      if (!f.mask[j * w + i]) continue;
      const o = (j * w + i) * 3;
      surf.set(x + i, y + j, f.px[o], f.px[o + 1], f.px[o + 2], 255);
    }
  }
}

/* ---------- render ----------
   One skin file, drawn for one state. Everything it reads comes in through
   ctx, so Theme Lens and the simulator draw with the same code:
     state      DEFAULT_STATE's fields: track, list, screen
     settings   %St values
     assets     get(ref, kind, where) -> parsed font/bmp/text or null, and a
                `missing` Map the render fills in
     cfg        the theme .cfg, parsed
     frame      animation frame, for %la, %Sb and the like
     vpOverride viewport index -> forced on/off (optional)
     lineOf     offset -> line (optional; worked out from model.text)
     palette    a PB.Palette (palette.js) to resolve colours through, as the
                player's dynamic colours do (optional; colours as written)
     cover      the playing track's art, for %Cd: an Art (optional; a
                stand-in picture)
     rows       the list, for a caller with more to say about each row than
                State.listRows can: text, icon, and kind, value, range, track,
                playing and art (an Art) as the %L tags read them (optional)
     enabled    the labels the last render of this skin switched on, so a
                caller drawing frame after frame saves the second pass below
                (optional)
   Returns the surface, and what the render decided: which viewports drew, why
   the others did not, and where the list landed. */
function renderSkin(model, ctx) {
  /* A %Vd switches its label on until a branch not taken switches it off, and
     the player keeps that from one redraw to the next -- so a %Vd written
     below the viewport it names shows it from the second frame on. One pass
     cannot see that; a second, seeded with what the first switched on, can.
     A seed that matches what the pass switched on is the steady state. */
  let seed = ctx.enabled || new Set(), r = null;
  for (let pass = 0; pass < 3; pass++) {
    r = renderSkinPass(model, ctx, seed);
    if (r.enabled.size === seed.size && [...seed].every(l => r.enabled.has(l))) break;
    seed = r.enabled;
  }
  return r;
}

function renderSkinPass(model, ctx, seed) {
  const State = ctx.state, Settings = ctx.settings || {}, Assets = ctx.assets;
  const cfg = ctx.cfg || {};
  const animFrame = ctx.frame || 0;
  const vpOverride = ctx.vpOverride || new Map();
  const lineOf = ctx.lineOf || lineIndex(model.text);
  const shownVps = new Set(), whyByLabel = new Map();
  let lastListView = null, lastRowLayout = null;
  const surf = makeSurface();
  Assets.missing.clear();     /* this render's misses only */
  const palette = ctx.palette || null;
  const hexToRgb = palette
    ? s => { const c = palette.resolve(s); return c === null ? null : PB.rgb(c); }
    : hexToRgbAsWritten;
  const fgDefault = hexToRgb(cfg["foreground color"]) || [20, 20, 20];
  const bgDefault = hexToRgb(cfg["background color"]) || [245, 245, 245];
  surf.fill(0, 0, LCD_W, LCD_H, bgDefault);

  const enabled = new Set();
  const vars = {};
  const notes = [];

  /* Font "-" means the user's UI font, which lives in the config rather than
     the skin. Fall back to the one named in the State pane. */
  const fontFor = id => {
    if (id === null || id === undefined) id = 1;
    const ref = model.fonts[String(id)];
    if (ref) {
      const off = model.fontSites ? model.fontSites[String(id)] : undefined;
      return Assets.get(ref, "font",
        off === undefined ? "%Fl(" + id + ")" : "%Fl(" + id + ") on line " + (lineOf(off) + 1));
    }
    if (id <= 1) {
      const fromSettings = (Settings["font"] || "").trim();
      const ui = fromSettings || (cfg["font"] || "").trim();
      if (ui) return Assets.get(/\.fnt$/i.test(ui) ? ui : ui + ".fnt", "font",
        fromSettings ? "the UI font set in the State tab" : "the theme .cfg 'font:' line");
    }
    return null;
  };
  /* The .options file beside a .preferences one, parsed, if it has a good
     option: the player then creates the .preferences file itself */
  const optionsBeside = file => {
    if (!/\.preferences$/i.test(file) || typeof Assets.resolve !== "function") return null;
    const ref = file.replace(/\.preferences$/i, ".options");
    if (!Assets.resolve(ref)) return null;
    const txt = Assets.get(ref, "text");
    if (typeof txt !== "string") return null;
    const p = optionsFor(txt);
    return p.ok && p.options.length ? p : null;
  };
  /* As the player reads it: with an options file, a missing .preferences, a
     missing key or a bad value is the option's default, which the player
     writes there before the skin reads it */
  const readKey = (file, key) => {
    const p = optionsBeside(file);
    const k = key.replace(/:\s*$/, "").trim().toLowerCase();
    const opt = p ? p.options.find(o => o.key.toLowerCase() === k) || null : null;
    const txt = p && !Assets.resolve(file) ? null : Assets.get(file, "text");
    let value = null;
    if (typeof txt === "string") {
      for (const line of txt.split(/\r?\n/)) {
        const i = line.indexOf(":");
        if (i < 0) continue;
        if (line.slice(0, i + 1).trim() === key.trim() || line.slice(0, i).trim() === key.replace(/:$/, "").trim()) {
          value = line.slice(i + 1).trim();
          break;
        }
      }
    }
    if (opt && (value === null || !optionValid(opt, value))) return opt.def;
    return value === null ? "" : value;
  };

  /* Group each viewport's top-level nodes by physical line. */
  const groupsFor = idx => {
    const vp = model.vps[idx];
    const end = model.vps[idx + 1] ? model.vps[idx + 1].off : model.text.length;
    const byLine = new Map();
    for (const n of model.nodes) {
      if (n.type === "nl" || n.type === "comment") continue;
      if (n.s < vp.off || n.s >= end) continue;
      if (n.s === vp.off) continue;                       /* the %V declaration */
      const k = lineOf(n.s);
      if (!byLine.has(k)) byLine.set(k, []);
      byLine.get(k).push(n);
    }
    return [...byLine.entries()].sort((a, b) => a[0] - b[0]).map(e => e[1]);
  };

  const rowData = ctx.rows || listRowData(State);
  lastListView = null;              /* set again below if a list is drawn */
  const tally = { drawn: 0, notEnabled: 0, oob: 0, noFont: 0, rows: 0 };
  const chosen = new Map();     /* conditional offset -> branch index actually taken */
  const uiVps = new Map();      /* %Vi label -> viewport (the list's parent area) */
  let activeLb = null;          /* the running %Lb config */
  let activeUiLabel = null;     /* the label last named by %VI */

  /* `rect` carries the geometry to draw at, so list rows can reuse this with
     the row's translated position; `row` is the list-row context or null. */
  /* Everything a viewport draws is clipped to it, as the LCD driver clips
     to the viewport it is set to: a long title stops at its edge. */
  const paintViewport = (idx, rect, row) => {
    const save = surf.clip;
    surf.clip = { x0: rect.x, y0: rect.y, x1: rect.x + rect.w, y1: rect.y + rect.h };
    try { paintViewportLines(idx, rect, row); } finally { surf.clip = save; }
  };
  const paintViewportLines = (idx, rect, row) => {
    let fg = fgDefault.slice(), bg = null;
    const font = fontFor(rect.fontDefault ? 1 : rect.font);
    if (!font) tally.noFont++;
    const lineH = font ? font.height : 10;
    let lineNo = 0;
    const vp = rect;

    const env = {
      st: State, vp, vars, frame: animFrame, row, rows: rowData,
      fontFor, readKey,
      setting: k => {
        const v = Settings[k];
        if (v !== undefined && v !== "") return v;
        if (k === "font") return baseName(cfg["font"]);
        return v !== undefined ? v : "";
      },
    };

    let invert = false;            /* set by %Vs(invert), cleared by %Vs(clear) */
    /* %Vs(gradient) draws the line on the bar %Vg describes: start colour at
       the top, end colour at the bottom, the text in the third. */
    let gradient = false, grad = null;
    /* %Vy: the box a line is centred in and %Vs(invert) fills. null means the
       tag is absent and the box is the font height; 0 means bare %Vy, which is
       the viewport's own height. It does NOT move the next line: write_line()
       still steps by the font height. */
    let lineBox = null;
    for (const group of groupsFor(idx)) {
      let noBreak = false;
      const segs = { l: "", c: "", r: "" };
      let align = "l";

      const runNode = n => {
        if (n.type === "tag" && NOBREAK_TAGS.has(n.name)) noBreak = true;
        if (n.type === "tag") {
          switch (n.name) {
            case "Vd": {
              const lbl = argStr(n, 0, env);
              if (lbl) enabled.add(lbl);
              return;
            }
            case "VI": {
              const lbl = argStr(n, 0, env);
              if (lbl) { enabled.add(lbl); activeUiLabel = lbl; }
              return;
            }
            case "Lb": {
              const lbl = argStr(n, 0, env);
              if (lbl) activeLb = {
                label: lbl,
                w: parseInt(argStr(n, 1, env), 10) || 0,
                h: parseInt(argStr(n, 2, env), 10) || 0,
                tile: (n.args[3] ? n.args[3].raw.trim() : "") === "tile",
                artH: parseInt(argStr(n, 4, env), 10) || 0,
              };
              return;
            }
            case "La": {
              if (!row || !State.listArtRows) return;
              const s = Math.min(vp.w, vp.h);
              const art = rowData[row.index] && rowData[row.index].art;
              if (art) { art.draw(surf, vp.x, vp.y, s, s, "c", "c", null); return; }
              for (let j = 0; j < s; j++) for (let i = 0; i < s; i++) {
                const t = (i / s + j / s) / 2;
                surf.set(vp.x + i, vp.y + j, 60 + t * 80, 70 + t * 60, 110 + t * 50, 255);
              }
              return;
            }
            case "Vf": { const c = hexToRgb(argStr(n, 0, env)); if (c) fg = c; return; }
            case "Vb": {
              const c = hexToRgb(argStr(n, 0, env));
              if (c) { bg = c; surf.fill(vp.x, vp.y, vp.w, vp.h, c); }
              return;
            }
            case "al": align = "l"; return;
            case "ac": align = "c"; return;
            case "ar": align = "r"; return;
            case "aL": align = "l"; return;
            case "aR": align = "r"; return;
            case "dr": {
              const r = resolveInner(n.args, vp);
              const c = hexToRgb(argStr(n, 4, env)) || fg;
              const raw = k => (n.args[k] ? n.args[k].raw.trim() : "");
              const dflt = (s, d) => (s === "" || s === "-") ? d : (parseInt(s, 10) || 0);
              surf.panel(r.x, r.y, r.w, r.h, c, dflt(raw(6), 15), dflt(raw(7), 0));
              return;
            }
            case "Cd": {
              /* Which %Cl this draws was worked out with the file in front of
                 us (n.artRef); the box comes from THAT one's viewport, not
                 from this one, because the two need not be the same. */
              const art = n.artRef;
              const box = (art && art.box) || vp.art || { x: vp.x, y: vp.y, w: vp.w, h: vp.h };
              if (!State.artFound) return;
              const w = Math.max(1, box.w), h = Math.max(1, box.h);
              const chain = (art && art.filters) || model.artFilters || [];
              const cl = art && art.node ? art.node.args : [];
              const align = k => (cl[k] ? cl[k].raw.trim().toLowerCase() : "c")[0] || "c";
              const real = ctx.cover ? ctx.cover.fitted(w, h, align(4), align(5), chain) : null;
              const px = real ? real.px : new Float32Array(w * h * 3);
              if (!real) for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
                /* A stand-in cover, not a real one — but a chain judged on a
                   flat wash tells you nothing, so it carries a diagonal ramp
                   and a warm quadrant for the colour filters to work on. */
                const t = (i / w + j / h) / 2, warm = (i / w) * (1 - j / h);
                const o = (j * w + i) * 3;
                px[o]     = 40 + t * 90 + warm * 120;
                px[o + 1] = 55 + t * 70 + warm * 40;
                px[o + 2] = 90 + t * 60;
              }
              /* Each %Cl carries its own chain, so take the one this %Cd
                 draws rather than a skin-wide setting. */
              if (!real) applyImgFilters(px, w, h, chain);

              /* An optional window: a rectangle of the VIEWPORT to reveal,
                 rather than the whole box. The art stays anchored where %Cl
                 put it, so the window opens onto the composition — which is
                 what lets one slot serve several cut-outs. */
              let cx = box.x, cy = box.y, cw = w, ch = h;
              const wr = k => (n.args[k] ? n.args[k].raw.trim() : "");
              /* A reflection: the art's bottom rows upside down from the top
                 of this viewport, fading from `strength` per cent to nothing
                 at its foot. */
              if (wr(5).toLowerCase() === "reflect") {
                const s = (parseInt(wr(6), 10) || 35) / 100;
                const rows = Math.min(vp.h, h);
                for (let j = 0; j < rows; j++) {
                  const a = Math.round(255 * s * (rows - j) / rows);
                  for (let i = 0; i < w; i++) {
                    const o = ((h - 1 - j) * w + i) * 3;
                    if (real && !real.mask[o / 3]) continue;
                    surf.set(box.x + i, vp.y + j, px[o], px[o + 1], px[o + 2], a);
                  }
                }
                return;
              }
              if ([1, 2, 3, 4].every(k => wr(k) !== "" && wr(k) !== "-")) {
                const wx = vp.x + (parseInt(wr(1), 10) || 0);
                const wy = vp.y + (parseInt(wr(2), 10) || 0);
                cx = Math.max(box.x, wx);
                cy = Math.max(box.y, wy);
                cw = Math.min(box.x + w, wx + (parseInt(wr(3), 10) || 0)) - cx;
                ch = Math.min(box.y + h, wy + (parseInt(wr(4), 10) || 0)) - cy;
              }
              if (cw <= 0 || ch <= 0) return;

              for (let j = 0; j < ch; j++) for (let i = 0; i < cw; i++) {
                const o = ((cy - box.y + j) * w + (cx - box.x + i)) * 3;
                if (real && !real.mask[o / 3]) continue;
                surf.set(cx + i, cy + j, px[o], px[o + 1], px[o + 2], 255);
              }
              if (font && !real) drawString(surf, font, "ART", cx + 4, cy + 4, [255, 255, 255]);
              return;
            }
            case "x": case "xd": case "x9": {
              const id = argStr(n, 0, env);
              const im = model.images.find(m => m.id === id || m.id === id[0]);
              const bmp = im ? Assets.get(im.file, "bmp",
                "%xl(" + im.id + ") on line " + (lineOf(im.off) + 1)) : null;
              if (bmp) blitBmp(surf, bmp, im.x, im.y, fg, null);
              return;
            }
            case "pb": case "pv": case "bl": case "pP": case "pX": case "St": {
              if (!n.args.length) break;              /* bare form is text */
              const r = resolveInner(n.args, vp);
              let frac = 0.5;
              if (n.name === "pb") frac = State.elapsed / Math.max(1, State.length);
              else if (n.name === "bl") frac = State.battery / 100;
              else if (n.name === "pv") frac = (State.volume + 60) / 60;
              else if (n.name === "pP" || n.name === "pX") frac = State.playlistPos / Math.max(1, State.playlistCount);
              frac = Math.max(0, Math.min(1, frac));
              const bw = r.w > 0 ? r.w : vp.w, bh = r.h > 0 ? r.h : 4;
              /* The unfilled part is cleared, not painted: to the viewport's
                 background where it has one, otherwise to what lies beneath. */
              if (bg) surf.fill(r.x, r.y, bw, bh, bg);
              surf.fill(r.x, r.y, Math.round(bw * frac), bh, fg);
              return;
            }
            case "wt": {
              const s = argStr(n, 0, env);
              const al = (n.args[1] ? n.args[1].raw.trim() : "tl") || "tl";
              if (!font) return;
              const lines = wrapText(s || argStr(n, 2, env), vp.w, font);
              const total = lines.length * font.height;
              let oy = 0;
              if (al[0] === "c") oy = Math.max(0, (vp.h - total) >> 1);
              else if (al[0] === "b") oy = Math.max(0, vp.h - total);
              lines.forEach((ln, k) => {
                const tw = font.textWidth(ln);
                let ox = 0;
                if (al[1] === "c") ox = Math.max(0, (vp.w - tw) >> 1);
                else if (al[1] === "r") ox = Math.max(0, vp.w - tw);
                drawString(surf, font, ln, vp.x + ox, vp.y + oy + k * font.height, fg);
              });
              return;
            }
            case "Sb": {
              const raw = k => (n.args[k] ? n.args[k].raw.trim() : "");
              const bars = Math.max(1, Math.min(8, parseInt(argStr(n, 0, env), 10) || 5));
              const align = raw(1).toLowerCase();
              /* Radiate is centre-out both ways, so it carries centre growth. */
              const radiating = align === "radiate";
              const centred = radiating || align === "center";
              const dflt = (s, d) => (s === "" || s === "-") ? d : Math.max(0, parseInt(s, 10) || 0);
              const radius = dflt(raw(2), 0);
              /* Absent means what every %Sb written before the argument existed
                 got: one pixel, or none when there is only one bar to separate. */
              const gap = dflt(raw(3), (bars > 1 || radiating) ? 1 : 0);
              /* The cap thickness is what switches the caps on, so a theme can
                 have them without naming a colour. Absent, they take the
                 viewport foreground the bars are drawn in. */
              const capH = Math.min(vp.h, dflt(raw(4), 0));
              const capFg = (raw(5) === "" || raw(5) === "-")
                          ? fg : (hexToRgb(argStr(n, 5, env)) || fg);
              /* Radiate lays the band table out twice, once per channel, so the
                 tag's bar count is per side. */
              const cols = radiating ? bars * 2 : bars;
              /* The bars share what is left after the gaps, so a wider gap
                 narrows the bars rather than the meter. */
              const span = Math.max(1, Math.floor((vp.w - gap * (cols - 1)) / cols));
              /* Segments: whole ones on a grid from the floor, or out from the
                 middle when centred, so tops and caps land on boundaries. */
              const segH = dflt(raw(6), 0);
              const segGap = dflt(raw(7), 1);
              const pitch = segH + segGap;
              const mid = vp.h >> 1, halfGap = segGap >> 1;
              const slots = segH > 0
                ? Math.max(1, Math.floor(((centred ? mid - halfGap : vp.h) + segGap) / pitch)) : 0;
              const toSlots = px => Math.floor((Math.floor(px * 100 / vp.h) * slots + 50) / 100);
              const segs = (bx, from, to, col) => {
                for (let s = from; s < to; s++) {
                  const rise = s * pitch + segH;
                  if (centred) {
                    surf.panel(bx, vp.y + mid - halfGap - rise, span, segH, col, 15, radius);
                    surf.panel(bx, vp.y + mid + segGap - halfGap + s * pitch, span, segH, col, 15, radius);
                  } else {
                    surf.panel(bx, vp.y + vp.h - rise, span, segH, col, 15, radius);
                  }
                }
              };
              for (let k = 0; k < cols; k++) {
                /* Bass at the middle out to treble at both edges. The halves run
                   off different channels, so offset the phase rather than
                   mirroring it. */
                const right = radiating && k >= bars;
                const band = radiating ? (right ? k - bars : bars - 1 - k) : k;
                const barAt = f => Math.round(vp.h * (0.25 + 0.6 * Math.abs(Math.sin(band * 1.7 + (right ? 0.9 : 0) + f * 0.4))));
                const hgt = barAt(animFrame);
                const bx = vp.x + k * (span + gap);
                if (segH > 0) {
                  segs(bx, 0, Math.min(Math.max(toSlots(hgt), 1), slots), fg);
                  if (capH > 0) {
                    let reach = hgt;
                    for (let b = 1; b <= 6; b++)
                      reach = Math.max(reach, barAt(animFrame - b) - Math.round(vp.h * 0.04) * b);
                    const capSlots = Math.min(Math.ceil(capH / pitch), slots);
                    const to = Math.min(Math.max(toSlots(Math.min(vp.h, reach)), capSlots), slots);
                    segs(bx, to - capSlots, to, capFg);
                  }
                  continue;
                }
                if (centred) {
                  const half = Math.max(1, hgt >> 1);
                  surf.panel(bx, vp.y + ((vp.h >> 1) - half), span, half * 2, fg, 15, radius);
                } else {
                  surf.panel(bx, vp.y + vp.h - hgt, span, hgt, fg, 15, radius);
                }
                if (capH > 0) {
                  /* The player holds the cap at each new maximum and then lets
                     it fall; here the recent maxima are discounted by a ramp,
                     which puts the cap in the same places without keeping
                     state between preview frames. */
                  let reach = hgt;
                  for (let b = 1; b <= 6; b++)
                    reach = Math.max(reach, barAt(animFrame - b) - Math.round(vp.h * 0.04) * b);
                  reach = Math.max(capH, Math.min(vp.h, reach));
                  if (centred) {
                    const top = vp.y + ((vp.h - reach) >> 1);
                    surf.panel(bx, top, span, capH, capFg, 15, radius);
                    surf.panel(bx, top + reach - capH, span, capH, capFg, 15, radius);
                  } else {
                    surf.panel(bx, vp.y + vp.h - reach, span, capH, capFg, 15, radius);
                  }
                }
              }
              return;
            }
            /* %Vs(invert) swaps fore and background for the lines that follow
               it in this viewport, which is how nearly every skinned list
               draws its selected row. The other styles are not simulated. */
            case "Vs": {
              const style = (n.args[0] ? n.args[0].raw.trim() : "").toLowerCase();
              if (style === "invert") invert = true;
              else if (style === "gradient") gradient = true;
              else if (style === "clear") invert = gradient = false;
              return;
            }
            case "Vy": {
              const raw = (n.args[0] ? n.args[0].raw.trim() : "");
              lineBox = (raw === "" || raw === "-") ? 0 : (parseInt(raw, 10) || 0);
              return;
            }
            case "Vg": {
              const c = k => hexToRgb(argStr(n, k, env));
              grad = { start: c(0) || fg, end: c(1) || c(0) || fg, text: c(2) || fg };
              return;
            }
            case "VB": case "wd": case "we": case "wi": case "Fl": case "Cl":
            case "xl": case "X": case "T": case "Vp":
              return;
            case "vs": { vars[argStr(n, 0, env)] = argStr(n, 2, env); return; }
          }
        }
        /* everything else contributes text */
        if (n.type === "cond") {
          const idx2 = condBranchIndex(n, env);
          chosen.set(n.s, idx2);
          if (idx2 >= 0) for (const kid of n.branches[idx2].nodes) runNode(kid);
          return;
        }
        segs[align] += evalNode(n, env);
      };

      /* Sublines alternate in place, so only one is ever on screen. Draw the
         one that is up longest: a theme that writes %t(0.1)…;%t(3600)… means
         the second as its steady state and the first as a flash on entry, and
         running them together draws the line's content twice. */
      let best = [], bestFor = -1, part = [], secs = null;
      const closeSubline = () => {
        const shown = secs === null ? 2 : secs;      /* the parser's own default */
        if (shown > bestFor) { bestFor = shown; best = part; }
        part = []; secs = null;
      };
      for (const n of group) {
        if (n.type === "subline") { closeSubline(); continue; }
        if (n.type === "tag" && n.name === "t" && n.args.length) {
          const v = parseFloat(n.args[0].raw);
          if (!isNaN(v)) secs = v;
        }
        part.push(n);
      }
      closeSubline();
      for (const n of best) runNode(n);

      if (font && (segs.l || segs.c || segs.r)) {
        const y = vp.y + lineNo * lineH;
        if (y < vp.y + vp.h) {
          /* An inverted line fills its own strip first, the way the driver
             does — which is what makes the selected row a solid bar. */
          const bar = gradient && grad;
          const ink = bar ? grad.text : invert ? (bg || bgDefault) : fg;
          /* y is the top of the box; %Vy can make it taller than the font, and
             the text is then centred in it while the fill covers all of it. */
          const box = lineBox === null ? lineH : (lineBox > 0 ? lineBox : vp.h);
          const ty = y + ((box - lineH) >> 1);
          if (invert)
            surf.fill(vp.x, y, vp.w, Math.min(box, vp.y + vp.h - y), fg);
          if (bar) {
            const h = Math.min(box, vp.y + vp.h - y);
            for (let j = 0; j < h; j++) {
              const t = h > 1 ? j / (h - 1) : 0;
              surf.fill(vp.x, y + j, vp.w, 1, grad.start.map((v, k) => v + (grad.end[k] - v) * t));
            }
          }
          if (segs.l) drawString(surf, font, segs.l, vp.x, ty, ink);
          if (segs.c) {
            const w = font.textWidth(segs.c);
            drawString(surf, font, segs.c, vp.x + Math.max(0, (vp.w - w) >> 1), ty, ink);
          }
          if (segs.r) {
            const w = font.textWidth(segs.r);
            drawString(surf, font, segs.r, vp.x + Math.max(0, vp.w - w), ty, ink);
          }
        }
      }
      if (!noBreak) lineNo++;
    }
  };

  model.vps.forEach((vp, idx) => {
    /* %Vi viewports are VP_NEVER_VISIBLE — they mark the list's area only. */
    if (vp.kind === "ui") { if (vp.label) uiVps.set(vp.label, vp); return; }
    let show = vp.kind === "vp" || (vp.label && (enabled.has(vp.label) || seed.has(vp.label)));
    if (vpOverride.has(idx)) show = vpOverride.get(idx);   /* the checkbox wins */
    if (!show) { tally.notEnabled++; return; }
    if (vp.w <= 0 || vp.h <= 0 || vp.oob) { tally.oob++; return; }
    tally.drawn++;
    shownVps.add(idx);
    paintViewport(idx, vp, null);
  });

  /* ---- list rows ----
     Per list_skinned.c: every %Vl sharing the %Lb label is redrawn once per
     visible row, its declared x/y treated as an offset inside the row, and the
     row itself offset from the parent %Vi area by the %Lb height. */
  /* There is no list on the now-playing screen, so a %Vi declared in a .wps
     must not sprout menu rows. */
  /* "database art row height" is a theme setting, so the .cfg usually carries
     it; State overrides when non-zero. 52 is the firmware default. */
  const cfgArtH = parseInt(cfg["database art row height"], 10);
  const artRowH = State.artRowHeight > 0 ? State.artRowHeight
                : (cfgArtH > 0 ? cfgArtH : 52);
  const listScreen = State.screen !== 2;
  const parent = listScreen
    ? ((activeUiLabel && uiVps.get(activeUiLabel)) || uiVps.get("-") || null)
    : null;
  lastRowLayout = parent && activeLb ? { parent, pitch: 0, label: activeLb.label } : null;
  if (activeLb && activeLb.h > 0 && parent) {
    const rows = rowData;
    const rowVps = [];
    model.vps.forEach((v, i) => { if (v.label === activeLb.label && v.kind === "cond") rowVps.push([v, i]); });

    /* An art list overrides the row pitch (gui_synclist_set_row_height with
       "database art row height"); each row viewport keeps its declared height
       and lays out inside the row. Tiled lists always use the %Lb grid. */
    /* The %Lb's own art height wins over the theme setting, as
       skinlist_art_row_height() has it. */
    const rowH = (State.listArtRows && !activeLb.tile)
               ? (activeLb.artH > 0 ? activeLb.artH : artRowH) : activeLb.h;
    lastRowLayout.pitch = rowH;
    const cols = activeLb.tile ? Math.max(1, Math.floor(parent.w / Math.max(1, activeLb.w))) : 1;
    const perCol = Math.floor(parent.h / rowH);
    const displayLines = activeLb.tile ? cols * perCol : perCol;

    /* Tall rows rarely divide the area evenly — Rockbox centres them in the
       leftover rather than leaving it all at the bottom. */
    let rowTop = (rowH > activeLb.h && rows.length) ? Math.floor((parent.h % rowH) / 2) : 0;
    for (let cur = 0; cur < displayLines; cur++) {
      const item = (State.listStart || 0) + cur;
      if (item >= rows.length) break;
      const rowCtx = { text: rows[item].text, icon: rows[item].icon,
                       index: item, selected: item === State.listSel };
      for (const [v, i] of rowVps) {
        const rx = activeLb.tile ? parent.x + activeLb.w * (cur % cols) + v.x : parent.x + v.x;
        const ry = activeLb.tile ? parent.y + activeLb.h * Math.floor(cur / cols) + v.y
                                 : parent.y + v.y + rowTop;
        if (ry >= parent.y + parent.h) continue;
        paintViewport(i, {
          x: rx, y: ry, w: v.w, h: v.h,
          font: v.font, fontDefault: v.fontDefault, art: v.art,
        }, rowCtx);
      }
      tally.rows++;
      if (!activeLb.tile) rowTop += rowH;
    }
    tally.rowPitch = rowH;
    lastListView = { total: rows.length, visible: displayLines };
  }

  /* No %Lb means the skin only marks out the area with %Vi and Rockbox draws
     the rows itself, in the UI font with the configured selector. Most themes
     do this — without it their menu area renders empty. */
  if (parent && !activeLb) {
    const uiFont = fontFor(1);
    if (uiFont) {
      const rows = rowData;
      /* Same override as a skinned list — an art list gets the tall pitch. */
      const rowH = State.listArtRows ? artRowH : uiFont.height;
      const lines = Math.floor(parent.h / rowH);
      const inverse = /inverse/i.test(cfg["selector type"] || "bar (inverse)");
      const selBg = hexToRgb(cfg["line selector start color"]) || fgDefault;
      const selFg = hexToRgb(cfg["line selector text color"]) || bgDefault;
      for (let k = 0; k < lines; k++) {
        const item = (State.listStart || 0) + k;
        if (item >= rows.length) break;
        const y = parent.y + k * rowH;
        const sel = item === State.listSel;
        if (sel) surf.fill(parent.x, y, parent.w, rowH, inverse ? fgDefault : selBg);
        let tx = parent.x + 1;
        if (State.listArtRows) {
          const s = Math.max(0, rowH - 2);
          for (let j = 0; j < s; j++) for (let i = 0; i < s; i++) {
            const t = (i / s + j / s) / 2;
            surf.set(parent.x + i, y + 1 + j, 60 + t * 80, 70 + t * 60, 110 + t * 50, 255);
          }
          tx = parent.x + s + 4;
        }
        drawString(surf, uiFont, rows[item].text, tx, y + ((rowH - uiFont.height) >> 1),
                   sel ? (inverse ? bgDefault : selFg) : fgDefault);
        tally.rows++;
      }
      tally.builtinList = true;
      tally.rowPitch = rowH;
      lastListView = { total: rows.length, visible: lines };
    }
  }

  /* Why each label did or did not get switched on: walk every %Vd/%VI/%Lb
     site's enclosing conditionals and find the first one that went elsewhere. */
  whyByLabel.clear();
  for (const site of (model.refSites || [])) {
    let blocker = null;
    for (const p of site.path) {
      const took = chosen.get(p.condOff);
      if (took === undefined) { blocker = Object.assign({ took: null }, p); break; }
      if (took !== p.branch) { blocker = Object.assign({ took }, p); break; }
    }
    if (!whyByLabel.has(site.label)) whyByLabel.set(site.label, []);
    whyByLabel.get(site.label).push({ off: site.off, tag: site.tag, blocker });
  }

  tally.lb = activeLb ? activeLb.label : null;
  tally.activeUi = activeUiLabel;
  tally.uiVps = [...uiVps.keys()];
  if (tally.rows && tally.builtinList) notes.push(tally.rows + " list rows drawn by Rockbox (no %Lb)");
  else if (tally.rows) notes.push(tally.rows + " list rows via %Lb(" + activeLb.label + ")");
  else if (activeLb && !parent) notes.push("%Lb is set but no %Vi list area is active");
  if (tally.notEnabled) notes.push(tally.notEnabled + " viewports not shown at this state");
  if (tally.noFont) notes.push(tally.noFont + " viewports have no loaded font");
  return { surf, notes, tally, shownVps, whyByLabel, enabled,
           listView: lastListView, rowLayout: lastRowLayout };
}

/* Offset -> 0-based line, for a render with no editor to ask. */
function lineIndex(text) {
  const ls = [0];
  for (let k = 0; k < text.length; k++) if (text[k] === "\n") ls.push(k + 1);
  return off => {
    let lo = 0, hi = ls.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (ls[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo;
  };
}
