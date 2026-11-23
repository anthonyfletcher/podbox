/* The IDE's icons, from Lucide (ISC licence): icon(name) is the SVG as text,
 * in the text's colour; iconButton(name, title, onclick) a toolbar button. */

import {
    createElement, X, Minus, FolderOpen, Save, Play, Pause, SkipBack, SkipForward, RotateCcw,
    Camera, ScanEye, Eye, EyeOff, Music, ListMusic, Image, Type, Settings2, FileText, FileCode,
    MonitorPlay, PanelTop, Radio, Square, Disc3, Gauge, Clock, AlignLeft, LayoutList,
    BatteryMedium, Menu, Images, Sparkles, SlidersHorizontal, CircleAlert, TriangleAlert,
    ZoomIn, ZoomOut, Grid2x2, ChevronDown
} from 'lucide';

const ICONS = {
    close: X, minimise: Minus, open: FolderOpen, save: Save, play: Play, pause: Pause,
    prev: SkipBack, next: SkipForward, restart: RotateCcw, shot: Camera, outlines: ScanEye,
    shown: Eye, hidden: EyeOff, music: Music, list: ListMusic, image: Image, font: Type,
    cfg: Settings2, text: FileText, skin: FileCode, wps: MonitorPlay, sbs: PanelTop, fms: Radio,
    box: Square, art: Disc3, bar: Gauge, time: Clock, words: AlignLeft, rows: LayoutList,
    battery: BatteryMedium, menu: Menu, images: Images, fx: Sparkles, options: SlidersHorizontal,
    err: CircleAlert, warn: TriangleAlert, zoomIn: ZoomIn, zoomOut: ZoomOut, quick: Grid2x2,
    down: ChevronDown
};

export function icon(name) {
    const el = createElement(ICONS[name] || Square);
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
