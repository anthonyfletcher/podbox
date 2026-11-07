/* The IDE's icons, from Lucide (ISC licence): icon(name) is the SVG as
 * text, in the text's colour; iconButton(name, title, onclick) is a
 * toolbar button of one. */

import {
    createElement, Pencil, Slash, Square, Circle, PaintBucket, Pipette, SquareDashed,
    SquareSplitHorizontal, FlipHorizontal2, FlipVertical2, RotateCw, Contrast, Grid3x3,
    Ghost, ZoomIn, ZoomOut, Play, Pause, Plus, ChevronLeft, ChevronRight, X, Eraser,
    FileCode, Palette, Type, Trash2, Film, BoxSelect, SkipBack, SkipForward, Ban,
    Maximize2, Layers, MousePointer2, Grid2x2, Timer
} from 'lucide';

const ICONS = {
    pencil: Pencil, line: Slash, rect: Square, oval: Circle, fill: PaintBucket, pick: Pipette,
    select: SquareDashed, mirror: SquareSplitHorizontal, flipH: FlipHorizontal2,
    flipV: FlipVertical2, turn: RotateCw, invert: Contrast, tile: Grid3x3, onion: Ghost,
    zoomIn: ZoomIn, zoomOut: ZoomOut, play: Play, pause: Pause, stop: Square, plus: Plus,
    left: ChevronLeft, right: ChevronRight, close: X, erase: Eraser, text: FileCode,
    swatches: Palette, glyphs: Type, trash: Trash2, animate: Film, object: BoxSelect,
    prev: SkipBack, next: SkipForward, clear: Ban, resize: Maximize2, layers: Layers,
    pointer: MousePointer2, grid: Grid2x2, metronome: Timer
};

export function icon(name) {
    const el = createElement(ICONS[name]);
    el.setAttribute('width', '18');
    el.setAttribute('height', '18');
    el.setAttribute('stroke-width', '1.75');
    el.setAttribute('aria-hidden', 'true');
    return el.outerHTML;
}

export function iconButton(name, title, onclick) {
    const b = document.createElement('button');
    b.className = 'ib';
    b.type = 'button';
    b.innerHTML = icon(name);
    b.title = title;
    if (onclick) b.onclick = onclick;
    return b;
}
