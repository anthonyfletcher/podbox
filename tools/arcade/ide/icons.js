/* The IDE's icons, from Lucide (ISC licence): icon(name) is the SVG as
 * text, in the text's colour; iconButton(name, title, onclick) is a
 * toolbar button of one. */

import {
    createElement, Pencil, Slash, Square, Circle, PaintBucket, Pipette, SquareDashed,
    SquareSplitHorizontal, FlipHorizontal2, FlipVertical2, RotateCw, Contrast, Grid3x3,
    Ghost, ZoomIn, ZoomOut, Play, Pause, Plus, ChevronLeft, ChevronRight, ChevronDown, X, Eraser,
    FileCode, Palette, Type, Trash2, Film, SkipBack, SkipForward, Ban,
    Maximize2, Layers, MousePointer2, Grid2x2, Timer, MapPinPlus,
    LayoutGrid, Image, ImagePlus, ImageOff, CircleDot, SquareFunction, ListOrdered, ChevronUp, CaseSensitive, WholeWord, Regex, Replace, ReplaceAll, Power,
    Settings, Coins, Zap, Bomb, Sparkles, Crosshair, ArrowBigUp, MousePointerClick, Dices, WandSparkles,
    Minus, Crop, Scaling
} from 'lucide';

/* the four waves sfxr makes, drawn as Lucide draws */
const wave = (d) => [['path', { d }]];
const WAVES = {
    square: wave('M2 16h4V8h6v8h6V8h4'),
    saw: wave('M2 8l8 8V8l8 8V8l4 4'),
    sine: wave('M2 12c1.5-5 4.5-5 6 0s4.5 5 6 0 4.5-5 6 0'),
    noise: wave('M2 12l2-5 2 9 2-7 2 10 2-12 2 8 2-4 2 6 2-7 2 4')
};

const ICONS = {
    pencil: Pencil, line: Slash, rect: Square, oval: Circle, fill: PaintBucket, pick: Pipette, recolour: Replace,
    select: SquareDashed, mirror: SquareSplitHorizontal, flipH: FlipHorizontal2,
    flipV: FlipVertical2, turn: RotateCw, invert: Contrast, tile: Grid3x3, onion: Ghost,
    zoomIn: ZoomIn, zoomOut: ZoomOut, play: Play, pause: Pause, stop: Square, plus: Plus,
    left: ChevronLeft, right: ChevronRight, down: ChevronDown, close: X, minimise: Minus, erase: Eraser, text: FileCode,
    swatches: Palette, glyphs: Type, trash: Trash2, animate: Film, object: MapPinPlus,
    prev: SkipBack, next: SkipForward, clear: Ban, resize: Maximize2, layers: Layers,
    crop: Crop, canvas: Scaling,
    pointer: MousePointer2, grid: Grid2x2, metronome: Timer,
    coin: Coins, laser: Zap, explosion: Bomb, powerup: Sparkles, hit: Crosshair, jump: ArrowBigUp,
    sprites: LayoutGrid, picture: Image, trace: ImagePlus, traceOff: ImageOff, record: CircleDot, functions: SquareFunction,
    lines: ListOrdered, up: ChevronUp, matchCase: CaseSensitive,
    wholeWord: WholeWord, regex: Regex, replace: Replace, replaceAll: ReplaceAll, power: Power, settings: Settings, blip: MousePointerClick, random: Dices, mutate: WandSparkles,
    waveSquare: WAVES.square, waveSaw: WAVES.saw, waveSine: WAVES.sine, waveNoise: WAVES.noise
};

export function icon(name) {
    const el = createElement(ICONS[name]);
    el.setAttribute('width', '18');
    el.setAttribute('height', '18');
    el.setAttribute('stroke-width', '1.75');
    el.setAttribute('aria-hidden', 'true');
    return el.outerHTML;
}

/* the point of each tool's icon that does the work, in the icon's 24 units;
 * the tools not here draw from a crosshair */
const HOTSPOT = { pencil: [2, 22], pick: [2, 22], fill: [20, 22], erase: [5, 19], object: [12, 22] };
const cursors = {};

/* a CSS cursor for a tool: its icon, white on a dark edge so it shows on
 * any colour, or the crosshair */
export function toolCursor(name) {
    if (!HOTSPOT[name]) return 'crosshair';
    if (!cursors[name]) {
        const inner = createElement(ICONS[name]).innerHTML, k = 22 / 24;
        const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" ' +
            'fill="none" stroke-linecap="round" stroke-linejoin="round">' +
            '<g stroke="#000" stroke-width="4">' + inner + '</g>' +
            '<g stroke="#fff" stroke-width="1.75">' + inner + '</g></svg>';
        const [x, y] = HOTSPOT[name];
        cursors[name] = 'url("data:image/svg+xml,' + encodeURIComponent(svg) + '") ' +
            Math.round(x * k) + ' ' + Math.round(y * k) + ', crosshair';
    }
    return cursors[name];
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
