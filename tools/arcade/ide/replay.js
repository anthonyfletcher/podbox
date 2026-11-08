/* Replays: a run's every input, recorded as it runs, so it can be played
 * again exactly -- to see a bug happen again, or how a change does on the
 * same play.
 *
 * A run is the same run again given the same seed, the same store at its
 * start, the music playing or not, and, step by step, the same input: a
 * step is one turn of the IDE's loop -- the ticks skipped first, if any,
 * then the tick drawn -- with any dialog frames answered within them and
 * the music buttons pressed before them. Skipping goes by real time, so it
 * is recorded, not worked out again: a replay skips where the run did,
 * whatever player it runs as. Each drawn tick's screen is hashed, so a
 * replay can say where it stopped matching.
 *
 *   tape: { version, cart: { id, name, code }, seed, music, store, steps }
 *   step: { s: ticks skipped, a: [held, clicks, pos] (the skipped tick's),
 *           b: [held, clicks, pos] (the drawn tick's), d: [[held, wheel]]
 *           (dialog frames), m: [music button] (before it), h: screen hash }
 *
 * Parts: hashing; a store kept in memory; reading a tape. */

export const VERSION = 1;

/* ---- hashing ---- */

/* FNV-1a over the screen's palette numbers */
export function hashScreen(px) {
    let h = 0x811c9dc5;
    for (let i = 0; i < px.length; i++) h = Math.imul(h ^ px[i], 0x01000193);
    return h >>> 0;
}

/* the cart's code, to tell whether it changed since a tape was made */
export function hashCart(bytes) {
    return hashScreen(bytes).toString(16);
}

/* ---- a store kept in memory ---- */

/* a replay's store: the recorded one, changed only in memory, so the
 * browser's stays as it is */
export function memoryStore(snapshot) {
    const m = new Map(Object.entries(snapshot || {}));
    return {
        read: (name) => (m.has(name) ? new TextEncoder().encode(m.get(name)) : undefined),
        write: (name, bytes) => { m.set(name, new TextDecoder().decode(bytes)); return true; },
        remove: (name) => m.delete(name),
        list: () => [...m.keys()].sort()
    };
}

/* ---- reading a tape ---- */

/* a tape from a file's text, or an Error saying what is wrong with it */
export function readTape(text) {
    let t;
    try { t = JSON.parse(text); } catch (e) { return new Error('it is not a replay: ' + e.message); }
    if (!t || t.version !== VERSION || !Array.isArray(t.steps) || !t.cart || typeof t.seed !== 'string')
        return new Error('it is not a replay this IDE plays');
    return t;
}
