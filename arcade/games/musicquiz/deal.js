// deal: the quiz's ten tracks, and four wrong answers beside each --
// titles, artists, albums or years, whichever the round asks for.
//
// The game gets harder as it goes, and harder means the wrong answers are
// more like the right one. Where the Sound Index has measured the track
// that plays, "like" is how it sounds: the first round draws from the
// forty-odd tracks nearest it and the last from the nearest handful. Where
// it has not, "like" is the same genre and the same decade, and the rounds
// climb from anything at all to both. An album round's hardest wrong
// answers are the artist's own other albums; a year round's are the years
// either side.
//
// A deal takes a tick for the sample, one for every round's neighbours at
// once -- a single walk of the Sound Index however big the library -- and
// one to fill each round, since an album round can walk the library again:
// start() begins one, step() takes the next part, and done or failed says
// when it has finished.
//
// Parts: the sample; wrong answers by genre and decade; by sound; albums
// and years; a round; stepping through.

const ROUNDS = 10, CHOICES = 5;
const KIND_TITLE = 0, KIND_ARTIST = 1, KIND_ALBUM = 2, KIND_YEAR = 3;
const KINDS = 4;

// How many tracks the library is sampled for: ten to play and the rest to
// draw wrong answers from by genre and decade. Enough that a genre with a
// twentieth of the library still offers a dozen.
const POOL_MAX = 256;

// Shorter than this and a clip from partway in would run into the end
const MIN_LENGTH = 90000;

// How many of each track's nearest by sound are kept, and how far down
// that list each round starts: round 1 at RANK_STEP * 9, the last at the top
const NEAR_MAX = 60, RANK_STEP = 5;

// How alike a wrong answer has to be in each round where the index cannot
// say: 1 is the same decade, 2 the same genre, 3 both. A round that cannot
// find enough takes the next closest.
var like_wanted = [0, 0, 0, 1, 1, 1, 2, 2, 3, 3];

// Each round: its track and length (len), what it asks, the five choices
// and which is right
var rounds = [];

// The sample: { id, len, year, title, artist, album, genre, answer }
var pool = [];
var answer_pool = [];

// Each round's neighbours by sound, nearest first, or null
var nearest = [];

var kind_on = [true, true, true, false];
var year_max = 0;
var stage = 0;
var done = false;
var failed = false;
var ids = new Int32Array(POOL_MAX);
var near_ids = new Int32Array(NEAR_MAX * ROUNDS);

// ---- the sample -----------------------------------------------------------

function usable(s) {
    return s != null && s != "";
}

function same(a, b) {
    return a.toLowerCase() == b.toLowerCase();
}

// A track's answer to a kind of question, or null where it has none
function pool_text(t, kind) {
    if (kind == KIND_TITLE) return t.title;
    if (kind == KIND_ARTIST) return usable(t.artist) ? t.artist : null;
    if (kind == KIND_ALBUM) return usable(t.album) ? t.album : null;
    return t.year > 0 ? "" + t.year : null;
}

function taken(r, text) {
    for (var i = 0; i < r.choice.length; i++)
        if (same(r.choice[i], text)) return true;
    return false;
}

function number_of(id, f) {
    var v = tracks.field(id, f);
    return v == null ? 0 : v;
}

// POOL_MAX tracks from the whole library, shuffled: a library smaller than
// the sample comes back whole in database order, and would open every game
// on the same ten tracks
function sample() {
    var k = tracks.pick(POOL_MAX, { music: true, minlen: MIN_LENGTH }, ids);
    pool = [];
    year_max = 0;
    for (var i = 0; i < k; i++) {
        var id = ids[i];
        var title = tracks.field(id, TRACK.TITLE);
        if (!usable(title)) continue;
        var t = { id: id, title: title,
                  artist: tracks.field(id, TRACK.ARTIST),
                  album: tracks.field(id, TRACK.ALBUM),
                  genre: tracks.field(id, TRACK.GENRE),
                  year: number_of(id, TRACK.YEAR),
                  len: number_of(id, TRACK.LENGTH), answer: false };
        if (t.year > year_max) year_max = t.year;
        pool.push(t);
    }
    for (var j = pool.length - 1; j > 0; j--) {
        var o = rnd(j + 1), held = pool[j];
        pool[j] = pool[o];
        pool[o] = held;
    }
}

// The answers: the first ten of the sample with an answer to a question
// that is on, each asking one of those at random, and no two asking the
// same thing of the same answer
function choose_answers() {
    rounds = [];
    answer_pool = [];
    for (var i = 0; i < pool.length && rounds.length < ROUNDS; i++) {
        var kinds = [];
        for (var k = 0; k < KINDS; k++)
            if (kind_on[k] && pool_text(pool[i], k) != null) kinds.push(k);
        if (kinds.length == 0) continue;
        var kind = rnd(kinds), text = pool_text(pool[i], kind), clash = false;
        for (var j = 0; j < rounds.length && !clash; j++)
            clash = rounds[j].kind == kind && same(rounds[j].choice[0], text);
        if (clash) continue;
        pool[i].answer = true;
        rounds.push({ id: pool[i].id, len: pool[i].len, kind: kind,
                      choice: [text], right: 0 });
        answer_pool.push(i);
    }
}

// ---- wrong answers by genre and decade ------------------------------------

// 2 for the genre, 1 for the decade, summed
function likeness(a, b) {
    var like = 0;
    if (usable(a.genre) && usable(b.genre) && same(a.genre, b.genre))
        like += 2;
    if (a.year > 0 && b.year > 0 && flr(a.year / 10) == flr(b.year / 10))
        like += 1;
    return like;
}

// From the sample until the round has all its choices, the alike first
function add_like(r, round) {
    var a = pool[answer_pool[round]], start = rnd(pool.length);
    for (var want = like_wanted[round]; want >= 0; want--) {
        for (var k = 0; k < pool.length; k++) {
            if (r.choice.length >= CHOICES) return;
            var t = pool[(start + k) % pool.length];
            if (t.answer || likeness(a, t) < want) continue;
            var text = pool_text(t, r.kind);
            if (text != null && !taken(r, text)) r.choice.push(text);
        }
    }
}

// ---- wrong answers by sound -----------------------------------------------

// Every round's neighbours, in one walk of the Sound Index
function find_near() {
    var asked = [];
    for (var i = 0; i < ROUNDS; i++) asked.push(rounds[i].id);
    tracks.near(asked, NEAR_MAX, near_ids, true);
    nearest = [];
    for (var r = 0; r < ROUNDS; r++) {
        var list = [];
        for (var n = 0; n < NEAR_MAX; n++) {
            var id = near_ids[r * NEAR_MAX + n];
            if (id < 0) break;
            var title = tracks.field(id, TRACK.TITLE);
            if (!usable(title)) continue;
            list.push({ title: title, artist: tracks.field(id, TRACK.ARTIST),
                        album: tracks.field(id, TRACK.ALBUM),
                        len: number_of(id, TRACK.LENGTH) });
        }
        nearest.push(list.length > 0 ? list : null);
    }
}

function album_used(albums, album) {
    if (!usable(album)) return false;
    for (var i = 0; i < albums.length; i++)
        if (same(albums[i], album)) return true;
    return false;
}

// Neighbour e as the round's next choice, if it can be one: a title round
// takes at most one from any album -- the nearest are mostly the answer's
// own album otherwise
function take_near(r, e, albums) {
    if (e.len < MIN_LENGTH) return;
    var text = r.kind == KIND_TITLE ? e.title :
               r.kind == KIND_ARTIST ? e.artist : e.album;
    if (!usable(text) || taken(r, text)) return;
    if (r.kind == KIND_TITLE && album_used(albums, e.album)) return;
    r.choice.push(text);
    if (usable(e.album)) albums.push(e.album);
}

// From this round's place down the list, then back up towards the nearest
// for what that left unfilled
function add_near(r, round, list) {
    var from = min(RANK_STEP * (ROUNDS - 1 - round), list.length);
    var albums = [];
    for (var k = from; k < list.length && r.choice.length < CHOICES; k++)
        take_near(r, list[k], albums);
    for (var b = from - 1; b >= 0 && r.choice.length < CHOICES; b--)
        take_near(r, list[b], albums);
}

// ---- albums and years -----------------------------------------------------

// The artist's other albums, the hardest wrong answers an album round has
function add_own_albums(r, artist) {
    var k = tracks.pick(64, { artist: artist, music: true }, ids);
    for (var i = 0; i < k && r.choice.length < CHOICES; i++) {
        var album = tracks.field(ids[i], TRACK.ALBUM);
        if (usable(album) && !taken(r, album)) r.choice.push(album);
    }
}

// Four other years: five apart in the first round, one apart in the last,
// either side of the answer and none after the library's latest
function add_years(r, round, year) {
    var order = [-4, -3, -2, -1, 1, 2, 3, 4];
    var step = 5 - flr(4 * round / (ROUNDS - 1)), latest = max(year_max, year);
    for (var i = order.length - 1; i > 0; i--) {
        var j = rnd(i + 1), held = order[i];
        order[i] = order[j];
        order[j] = held;
    }
    for (var k = 0; k < order.length && r.choice.length < CHOICES; k++) {
        var y = year + order[k] * step;
        if (y >= 1900 && y <= latest) r.choice.push("" + y);
    }
}

// ---- a round --------------------------------------------------------------

// One round's choices for one kind of question; whether they filled
function fill_round(r, round, kind) {
    var t = pool[answer_pool[round]], text = pool_text(t, kind);
    if (text == null) return false;
    r.kind = kind;
    r.choice = [text];
    if (kind == KIND_YEAR) {
        add_years(r, round, t.year);
    } else {
        // the same artist's albums are as alike as albums come, so they
        // wait for the rounds that ask for the same genre
        if (kind == KIND_ALBUM && like_wanted[round] >= 2 && usable(t.artist))
            add_own_albums(r, t.artist);
        if (nearest[round] != null) add_near(r, round, nearest[round]);
        add_like(r, round);
    }
    return r.choice.length >= CHOICES;
}

// The kind the round was dealt first, then any other that is on; the right
// answer then moved to a random place among the wrong ones
function finish_round(round) {
    var r = rounds[round], first = r.kind, filled = false;
    for (var k = 0; k < KINDS && !filled; k++) {
        var kind = (first + k) % KINDS;
        filled = kind_on[kind] && fill_round(r, round, kind);
    }
    if (!filled) return false;
    var at = rnd(CHOICES), held = r.choice[at];
    r.choice[at] = r.choice[0];
    r.choice[0] = held;
    r.right = at;
    return true;
}

// ---- stepping through -----------------------------------------------------

// A new deal, asking the kinds of question that are on
function start(title, artist, album, year) {
    kind_on = [title, artist, album, year];
    if (!artist && !album && !year) kind_on[KIND_TITLE] = true;
    stage = 0;
    done = false;
    failed = false;
}

// The next part of the deal: the sample, then the neighbours, then each
// round filled. How far it has come, 0 to 1.
function step() {
    if (done || failed) return 1;
    if (stage == 0) {
        sample();
        choose_answers();
        failed = rounds.length < ROUNDS;
    } else if (stage == 1) {
        find_near();
    } else {
        failed = !finish_round(stage - 2);
        done = !failed && stage == ROUNDS + 1;
    }
    stage++;
    return stage / (ROUNDS + 2.0);
}
