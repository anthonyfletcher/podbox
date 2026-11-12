/* Replays: a run's every input, recorded as it runs, so it can be played
 * again exactly -- to see a bug happen again, or how a change does on the
 * same play.
 *
 * A run is the same run again given the same seed, the same store at its
 * start, the music playing or not, and, step by step, the same input: a
 * step is one turn of the IDE's loop -- the ticks skipped first, if any,
 * then the tick drawn -- with any dialog frames answered within them, and
 * the music buttons pressed and the Game-menu choices made before them. Skipping goes by real time, so it
 * is recorded, not worked out again: a replay skips where the run did,
 * whatever player it runs as. Each drawn tick's screen is hashed, so a
 * replay can say where it stopped matching.
 *
 *   tape: { version, cart: { id, name, code }, seed, music, store, kept,
 *           start, steps } -- kept: the scores, options and saved numbers
 *           the run began with; start: its date, in ms
 *   step: { s: ticks skipped, a: [held, clicks, pos] (the skipped tick's),
 *           b: [held, clicks, pos] (the drawn tick's), d: [[held, wheel]]
 *           (dialog frames), m: [music button] (before it), g: [choice]
 *           (after those), h: screen hash }
 *   choice: { i: menu item, d: [[held, wheel]] (its dialog frames) }, or
 *           { o: [[option, value]] } (the options set)
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
        /* the player's bounds: 64 names and 256 KB a cart */
        write: (name, bytes) => {
            let total = bytes.length;
            for (const [k, v] of m) if (k !== name) total += new TextEncoder().encode(v).length;
            if ((!m.has(name) && m.size >= 64) || total > 256 * 1024) return false;
            m.set(name, new TextDecoder().decode(bytes));
            return true;
        },
        remove: (name) => m.delete(name),
        list: () => [...m.keys()].sort()
    };
}

/* ---- reading a tape ---- */

/* a tape from a file's text, or an Error saying what is wrong with it */
export function readTape(text) {
    let t;
    try { t = JSON.parse(text); } catch (e) { return new Error('it is not a replay: ' + e.message); }
    if (!t || t.version !== VERSION || !Array.isArray(t.steps) || !t.cart || typeof t.cart !== 'object' ||
        typeof t.seed !== 'string' || !/^\d{1,20}$/.test(t.seed))
        return new Error('it is not a replay this IDE plays');
    const bad = badStore(t.store) || badKept(t.kept);
    if (bad) return new Error(bad);
    for (let n = 0; n < t.steps.length; n++) {
        const why = badStep(t.steps[n]);
        if (why) return new Error('its frame ' + (n + 1) + ' ' + why);
    }
    return t;
}

const num = (v) => typeof v === 'number' && Number.isFinite(v);
const nums = (v, n) => Array.isArray(v) && (n === undefined || v.length === n) && v.every(num);
const frames = (v) => v === undefined || (Array.isArray(v) && v.every((f) => nums(f, 2)));

/* what is wrong with a step, or null */
function badStep(st) {
    if (!st || typeof st !== 'object') return 'is not a frame';
    if (!nums(st.b, 3)) return 'has no input';
    if (st.s !== undefined && !(Number.isInteger(st.s) && st.s >= 0)) return 'skips a number of ticks that is not one';
    if (st.s && !nums(st.a, 3)) return 'skips ticks with no input for them';
    if (!frames(st.d)) return 'has dialog frames that are not';
    if (st.m !== undefined && !nums(st.m)) return 'has music buttons that are not';
    if (st.g !== undefined && !(Array.isArray(st.g) && st.g.every((c) => c && (
        c.o !== undefined ? Array.isArray(c.o) && c.o.every((o) => nums(o, 2))
            : Number.isInteger(c.i) && frames(c.d)))))
        return 'has Game-menu choices that are not';
    return null;
}

function badStore(store) {
    if (store === undefined) return null;
    if (!store || typeof store !== 'object' || !Object.values(store).every((v) => typeof v === 'string'))
        return 'its store is not names and text';
    return null;
}

/* the scores, options and saved numbers: base64, or none */
function badKept(kept) {
    if (kept === undefined) return null;
    if (!kept || typeof kept !== 'object') return 'what it kept is not a record';
    for (const k of ['saves', 'scores', 'options']) {
        const v = kept[k];
        if (v == null) continue;
        if (typeof v !== 'string') return 'its kept ' + k + ' are not base64';
        try { atob(v); } catch (e) { return 'its kept ' + k + ' are not base64'; }
    }
    return null;
}
