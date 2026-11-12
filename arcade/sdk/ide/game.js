/* What the player's pause menu holds, in the IDE's Game menu instead: the
 * cart's own menu items, its options, its high scores and achievements, its
 * help, and starting again. And what the player keeps for a cart between
 * plays -- its scores, its option values and its saved numbers -- kept in
 * the browser under the cart's id, so a run starts where the last left
 * off. A replay starts from what its run started with, and keeps nothing.
 *
 *   createGame(opts) -> game
 *     opts: say(kind, text), ask (the dialog), cart() (cart.json, parsed),
 *     engine() (the running Engine, or null), free() (whether a menu item
 *     or the options can be chosen now), choose(c) (a menu item, { i }, or
 *     options, { o: [[option, value]] }, made on the run), hold(fn) (fn()
 *     with the run held), restart(), dock, watchPane (the Watch panel's
 *     content)
 *
 * Also the IDE's own tools for a cart: watch()'s values, in a panel of
 * their own, and the wheel's click, as the player's little speaker plays
 * it.
 *
 * Parts: keeping; the run's start and each tick; the Game menu and its
 * dialogs; watch; the click. */

const KEY = 'arcade-kept/';

const b64 = (bytes) => { let s = ''; for (const b of bytes) s += String.fromCharCode(b); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

export function createGame(o) {
    /* ---- keeping ---- */

    const key = (kind) => KEY + ((o.cart() || {}).id || '') + '/' + kind;
    const read = (kind) => {
        try { const v = localStorage.getItem(key(kind)); return v ? unb64(v) : null; } catch (e) { return null; }
    };
    const write = (kind, bytes) => {
        try { localStorage.setItem(key(kind), b64(bytes)); } catch (e) { /* not kept */ }
    };

    let replaying = false;

    /* ---- the run's start and each tick ---- */

    /* before the cart starts: what it keeps put back, from the browser or
     * from the replay's record; returns the record for a tape */
    function begin(e, from) {
        replaying = !!from;
        const rec = from || {
            saves: read('saves'), scores: read('scores'), options: read('options')
        };
        const get = (k) => (rec[k] == null ? null : typeof rec[k] === 'string' ? unb64(rec[k]) : rec[k]);
        const saves = get('saves'), scores = get('scores'), options = get('options');
        if (saves) e.setSaves(saves);
        if (scores) e.setScores(scores);
        if (options) e.setOptions(options);
        watched.clear();
        watchOpened = false;
        drawWatch();
        return {
            saves: saves ? b64(saves) : null, scores: scores ? b64(scores) : null,
            options: options ? b64(options) : null
        };
    }

    /* after each tick: anything changed kept, a board the cart asked to
     * show shown, an achievement told; true when the cart asked to quit */
    function tick(e) {
        const sv = e.saves();
        if (sv.dirty && !replaying) write('saves', sv.bytes);
        const sc = e.scores();
        if (sc.dirty && !replaying) write('scores', sc.bytes);
        let a;
        while ((a = e.achieved()) >= 0) {
            const t = e.achievement(a);
            if (t) o.say('print', 'Achievement: ' + t.title + (t.text ? ' -- ' + t.text : ''));
        }
        const shown = e.scoreShown();
        if (shown) showScores(shown.board, shown.rank);
        return e.quitAsked();
    }

    /* ---- the Game menu and its dialogs ---- */

    /* the menu's rows, made as it opens: the cart's items while it runs,
     * then the rest */
    function fill(list, close) {
        list.innerHTML = '';
        const e = o.engine(), cart = o.cart() || {};
        const row = (label, fn, enabled) => {
            const b = document.createElement('button');
            b.textContent = label;
            b.disabled = !enabled;
            b.onclick = (ev) => { ev.stopPropagation(); close(); fn(); };
            list.append(b);
        };
        const rule = () => list.append(document.createElement('hr'));
        let items = 0;
        if (e) for (let i = 1; i <= 5; i++) {
            const label = e.menuItem(i);
            if (label) { row(label, () => o.choose({ i }), o.free()); items++; }
        }
        if (!items) {
            const l = document.createElement('div');
            l.className = 'label';
            l.textContent = e ? 'The cart has no menu items' : 'The cart\'s menu items show while it runs';
            list.append(l);
        }
        rule();
        row('Options…', optionsDialog, o.free() && (cart.options || []).length > 0);
        row('High scores…', () => showScores(-1, 0),
            !!e && ((cart.boards || []).length > 0 || (cart.achievements || []).length > 0));
        row('Help…', helpDialog, true);
        rule();
        row('Restart', () => o.restart(), true);
    }

    async function optionsDialog() {
        const e = o.engine(), opts = (o.cart() || {}).options || [];
        if (!e) return;
        const fields = opts.map((op, i) => {
            const title = op.title || op.name, v = e.option(i);
            if (op.type === 'choice')
                return { name: 'o' + i, label: title, options: op.choices.map((c, k) => [String(k), c]), value: String(v), note: op.help };
            if (op.type === 'onoff')
                return { name: 'o' + i, label: title, options: [['0', 'Off'], ['1', 'On']], value: String(v), note: op.help };
            const min = op.min, max = op.max;
            return { name: 'o' + i, label: title, type: 'number', value: String(v), note: op.help,
                     check: (n) => n !== '' && +n >= min && +n <= max ? null : min + ' to ' + max };
        });
        const a = await o.hold(() => o.ask('Options', fields, 'Set'));
        if (!a || o.engine() !== e || !o.free()) return;
        await o.choose({ o: opts.map((op, i) => [i, +a['o' + i]]) });
        if (!replaying) write('options', e.options());
    }

    /* the boards, or the one asked for with its rank marked, and the
     * achievements */
    function showScores(board, rank) {
        const e = o.engine();
        if (!e) return;
        const parts = [];
        for (let b = 0; ; b++) {
            const t = e.board(b);
            if (!t) break;
            if (board >= 0 && b !== board) continue;
            parts.push({ text: t.title });
            const lines = t.rows.map(([v, name, date], k) =>
                (b === board && k + 1 === rank ? '*' : ' ') + String(k + 1).padStart(2) + '  ' +
                v.padStart(10) + '  ' + (name || '').padEnd(15) + '  ' + day(+date));
            parts.push({ pre: lines.length ? lines.join('\n') : '  No scores yet' });
        }
        if (board < 0) {
            const got = [];
            for (let i = 0; ; i++) {
                const t = e.achievement(i);
                if (!t) break;
                got.push((t.date ? '✓ ' : '  ') + t.title + (t.text ? ' -- ' + t.text : ''));
            }
            if (got.length) parts.push({ text: 'Achievements' }, { pre: got.join('\n') });
        }
        o.hold(() => o.ask('High scores', parts.length ? parts : [{ text: 'No boards.' }], 'Close'));
    }

    const day = (d) => (d > 0 ? String(d).replace(/^(\d{4})(\d\d)(\d\d)$/, '$1-$2-$3') : '');

    function helpDialog() {
        const cart = o.cart() || {};
        const parts = [];
        if (cart.help) parts.push({ text: cart.help });
        for (const op of cart.options || [])
            if (op.help) parts.push({ text: (op.title || op.name) + ': ' + op.help });
        o.hold(() => o.ask((cart.name || 'The cart') + ': help', parts.length ? parts : [{ text: 'The cart has no help.' }], 'Close'));
    }

    /* ---- watch ---- */

    const watched = new Map();
    /* the panel opened at a run's first watch(), and left to the user after */
    let watchDue = false, watchOpened = false;

    function watch(name, value) {
        watched.set(name, value);
        if (!watchOpened) {
            watchOpened = true;
            if (!o.dock.shown('watch')) o.dock.show('watch', false);
        }
        if (!watchDue) {
            watchDue = true;
            requestAnimationFrame(() => { watchDue = false; drawWatch(); });
        }
    }

    function drawWatch() {
        const t = o.watchPane;
        t.innerHTML = '';
        if (!watched.size) {
            const p = document.createElement('div');
            p.className = 'watch-none';
            p.textContent = 'watch(name, value) in the cart shows a value here as it changes.';
            t.append(p);
            return;
        }
        for (const [k, v] of watched) {
            const n = document.createElement('div');
            n.className = 'watch-name';
            n.textContent = k;
            const val = document.createElement('div');
            val.className = 'watch-value';
            val.textContent = v;
            t.append(n, val);
        }
    }

    /* ---- the click ---- */

    /* click(): the wheel's tick, a sliver of noise; with beep, a short tone */
    function click(beep, audio) {
        if (!audio) return;
        const t = audio.currentTime;
        const g = audio.createGain();
        g.connect(audio.destination);
        if (beep) {
            const osc = audio.createOscillator();
            osc.frequency.value = 2000;
            g.gain.setValueAtTime(0.15, t);
            g.gain.exponentialRampToValueAtTime(0.001, t + 0.05);
            osc.connect(g);
            osc.start(t);
            osc.stop(t + 0.05);
        } else {
            const n = Math.floor(audio.sampleRate * 0.003);
            const buf = audio.createBuffer(1, n, audio.sampleRate);
            const d = buf.getChannelData(0);
            for (let i = 0; i < n; i++) d[i] = (Math.random() * 2 - 1) * (1 - i / n);
            const src = audio.createBufferSource();
            src.buffer = buf;
            g.gain.value = 0.3;
            src.connect(g);
            src.start(t);
        }
    }

    drawWatch();
    return { begin, tick, fill, watch, click, showScores };
}
