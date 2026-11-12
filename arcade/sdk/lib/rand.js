// rand: chance beyond rnd(): shuffles, weighted picks and ranges. Each
// takes a generator from rng.seed() last, to draw from it rather than
// rnd()'s own stream -- a daily puzzle kept apart from the game's sparks.
//
//   rand.int(lo, hi, [g])       a whole number from lo to hi, both included
//   rand.shuffle(a, [g])        a's elements in a random order, in place
//   rand.weighted(w, [g])       an index into w, each as likely as its
//                               weight: [5, 1] gives 0 five times in six
//   rand.chance(n, [g])         true one time in n

function roll(n, g) {
    return g == null ? rnd(n) : rng.rnd(g, n);
}

function int(lo, hi, g) {
    return lo + roll(hi - lo + 1, g);
}

function shuffle(a, g) {
    for (var i = a.length - 1; i > 0; i--) {
        var j = roll(i + 1, g), t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
    return a;
}

function weighted(w, g) {
    var total = 0;
    for (var i = 0; i < w.length; i++) total += w[i];
    var r = roll(total, g);
    for (var k = 0; k < w.length; k++) {
        if (r < w[k]) return k;
        r -= w[k];
    }
    return w.length - 1;
}

function chance(n, g) {
    return roll(n, g) == 0;
}
