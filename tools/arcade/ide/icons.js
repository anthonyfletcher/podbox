/* The IDE's icons: line drawings on a 24-unit square, in the text's
 * colour. icon(name) is the SVG as text. */

const PATHS = {
    pencil: '<path d="M4 20l4-1L19 8l-3-3L5 16z"/><path d="M14 7l3 3"/>',
    line: '<path d="M5 19L19 5"/>',
    rect: '<rect x="5" y="6" width="14" height="12"/>',
    oval: '<ellipse cx="12" cy="12" rx="8" ry="6"/>',
    fill: '<path d="M5 12l6-6 7 7-6 6z"/><path d="M8 9l7 7"/><path d="M20 15c0 2-.8 3-1.6 3s-1.6-1-1.6-3l1.6-2.4z"/>',
    pick: '<path d="M18.5 3.5a2.1 2.1 0 0 1 3 3L19 9l-3-3z"/><path d="M16 6l-9.5 9.5L5 20l4.5-1.5L19 9"/>',
    select: '<rect x="4" y="5" width="16" height="14" stroke-dasharray="3 2.5"/>',
    mirror: '<path d="M12 3v18" stroke-dasharray="2 2"/><path d="M9 8l-4 4 4 4"/><path d="M15 8l4 4-4 4"/>',
    flipH: '<path d="M12 3v18"/><path d="M8 7L4 12l4 5z" fill="currentColor"/><path d="M16 7l4 5-4 5z"/>',
    flipV: '<path d="M3 12h18"/><path d="M7 8l5-4 5 4z" fill="currentColor"/><path d="M7 16l5 4 5-4z"/>',
    turn: '<path d="M19 12a7 7 0 1 1-2.1-5"/><path d="M19 4v4h-4"/>',
    invert: '<circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" fill="currentColor"/>',
    tile: '<rect x="4" y="4" width="16" height="16"/><path d="M9.3 4v16M14.7 4v16M4 9.3h16M4 14.7h16"/>',
    onion: '<rect x="4" y="4" width="11" height="11" stroke-dasharray="2.5 2"/><rect x="9" y="9" width="11" height="11"/>',
    zoomIn: '<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5"/><path d="M8 10.5h5M10.5 8v5"/>',
    zoomOut: '<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5"/><path d="M8 10.5h5"/>',
    play: '<path d="M8 5v14l11-7z" fill="currentColor"/>',
    pause: '<path d="M7 5h3v14H7zM14 5h3v14h-3z" fill="currentColor"/>'
};

export function icon(name) {
    return '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" ' +
        'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
        PATHS[name] + '</svg>';
}
