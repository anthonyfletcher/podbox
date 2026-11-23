/* Names for things a theme author knows them by: a file by the screen it
 * draws, a viewport by what it shows, a tag by what it does. The skin
 * language's own names (%Vl, %Cd, wps) are kept for the second line. */

/* a theme file: { group, name, what, icon }; group orders the Files panel */
export function fileInfo(path) {
    const base = path.split('/').pop();
    const ext = (base.match(/\.([^.]+)$/) || [])[1];
    const e = (ext || '').toLowerCase();
    const stem = base.replace(/\.[^.]+$/, '');
    if (e === 'wps') return { group: 'Screens', order: 0, name: 'Now Playing', what: base, icon: 'wps', skin: 'wps' };
    if (e === 'sbs') return { group: 'Screens', order: 0, name: 'Menus and status bar', what: base, icon: 'sbs', skin: 'sbs' };
    if (e === 'fms') return { group: 'Screens', order: 0, name: 'Radio', what: base, icon: 'fms', skin: 'fms' };
    if (e === 'rwps' || e === 'rsbs' || e === 'rfms')
        return { group: 'Remote control screens', order: 5, name: base, what: 'not on an iPod', icon: 'skin' };
    if (e === 'cfg') return { group: 'Theme', order: 1, name: stem, what: 'theme settings', icon: 'cfg' };
    if (e === 'options') return { group: 'Theme', order: 1, name: stem, what: 'Theme Options', icon: 'options' };
    if (e === 'preferences') return { group: 'Theme', order: 1, name: stem, what: 'option choices', icon: 'options' };
    if (e === 'bmp' || e === 'png' || e === 'jpg') return { group: 'Images', order: 2, name: base, what: '', icon: 'image' };
    if (e === 'fnt') return { group: 'Fonts', order: 3, name: stem, what: '', icon: 'font' };
    return { group: 'Other', order: 4, name: base, what: '', icon: 'text' };
}

export const isSkin = (path) => /\.(wps|sbs|fms)$/i.test(path);
export const isText = (path) => /\.(wps|sbs|fms|rwps|rsbs|rfms|cfg|options|preferences|txt|md|icons|colours)$/i.test(path);

/* what a viewport shows, from the tags written in it */
const SHOWS = [
    [['Cd', 'Cl'], 'Album art', 'art'],
    [['Sb'], 'Spectrum', 'bar'],
    [['pb', 'pf'], 'Progress bar', 'bar'],
    [['pv'], 'Volume bar', 'bar'],
    [['pP', 'pX'], 'Playlist progress', 'bar'],
    [['Lb'], 'List rows', 'rows'],
    [['LT', 'LI', 'La', 'Lc'], 'A list row', 'rows'],
    [['LH', 'LD'], 'List header', 'rows'],
    [['QT', 'Qt', 'QL', 'Ql', 'QR', 'Qr', 'QB', 'Qb'], 'Quick Settings', 'quick'],
    [['bl', 'bc', 'bp'], 'Battery', 'battery'],
    [['cH', 'ck', 'cI', 'cl', 'cM'], 'Clock', 'time'],
    [['pc', 'pr', 'pt'], 'Time', 'time'],
    [['Lt'], 'Screen title', 'words'],
];
const TRACK = [['it', 'Title'], ['ia', 'Artist'], ['id', 'Album'], ['iA', 'Album artist'],
               ['ig', 'Genre'], ['iy', 'Year'], ['in', 'Track number'], ['It', 'Next title'],
               ['Ia', 'Next artist']];

/* { name, detail, icon } for one of skin-core's viewports (build().vps) */
export function viewportInfo(v) {
    const tags = v.tags || new Map();
    const has = (names) => names.some((n) => tags.has(n));
    let name = null, icon = 'box';
    if (v.kind === 'ui') { name = 'List area'; icon = 'rows'; }
    if (!name) for (const [names, what, ic] of SHOWS) if (has(names)) { name = what; icon = ic; break; }
    if (!name) {
        const words = TRACK.filter(([t]) => tags.has(t)).map(([, w]) => w);
        if (words.length) { name = words.slice(0, 2).join(' · '); icon = 'words'; }
    }
    if (!name && has(['x', 'xd', 'X'])) { name = 'Image'; icon = 'image'; }
    if (!name && has(['dr'])) { name = 'Panel'; icon = 'box'; }
    if (!name && v.implicit) name = 'Before the first viewport';
    if (!name) name = v.empty ? 'Whole screen' : 'Text';
    const decl = v.implicit ? '' : '%' + v.tagName + (v.label ? '(' + v.label + ')' : '');
    return { name, detail: decl, icon };
}

/* The firmware's own tag tables (tags.js, from build.js) over skin-core's
   hand-kept copy: each tag takes the arguments the firmware says, a tag the
   copy lacks is added, and one the firmware does not have is dropped. The
   descriptions stay skin-core's. Call once, before anything is scanned. */
export function adoptFirmwareTags() {
    /* global UPSTREAM, CUSTOM */
    const fw = window.FIRMWARE_TAGS;
    if (!fw) return;
    for (const [table, specs] of [[UPSTREAM, fw.upstream], [CUSTOM, fw.custom]]) {
        for (const name of Object.keys(table)) if (!(name in specs)) delete table[name];
        for (const [name, spec] of Object.entries(specs)) {
            if (table[name]) table[name][0] = spec;
            else table[name] = [spec, 'Described nowhere yet: see the skin language reference'];
        }
    }
}

/* a tag's description and argument names, from skin-core's tables */
export function tagInfo(name) {
    /* global lookup */
    const l = lookup(name);
    if (!l) return null;
    const [spec, desc, args] = l.def;
    return { name, spec, desc, args: args || [], custom: l.custom };
}

/* every tag, for completion and the reference */
export function allTags() {
    /* global UPSTREAM, CUSTOM */
    const out = [];
    for (const [table, custom] of [[UPSTREAM, false], [CUSTOM, true]])
        for (const [name, def] of Object.entries(table))
            out.push({ name, spec: def[0], desc: def[1], args: def[2] || [], custom });
    return out.sort((a, b) => a.name.localeCompare(b.name));
}
