import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compileFiles, loadDefs, inflate } from '../src/node.js';
import { load, verify, disassemble } from '../src/bytecode.js';
import { readCart } from '../src/cart.js';

const CART = JSON.stringify({ id: '0123456789abcdef', name: 'Test' });
const ENTRY = '\nfunction init() {}\nfunction update() {}\n';

function build(main, extra) {
    const files = Object.assign({ 'cart.json': CART, 'main.js': main }, extra || {});
    return compileFiles(files);
}

/* Compiles, verifies, and returns the disassembly. */
function ok(main, extra) {
    const res = build(main, extra);
    assert.deepEqual(res.errors, [], 'unexpected errors');
    const d = loadDefs();
    const prog = load(res.cart);
    verify(d.isa, d.api, prog);
    return { res: res, prog: prog, dis: disassemble(d.isa, d.api, prog) };
}

function rejects(main, message, line) {
    const res = build(main);
    assert.equal(res.cart, null, 'compiled, but should not have: ' + main);
    const hit = res.errors.find((e) => e.message.includes(message));
    assert.ok(hit, 'expected "' + message + '", got ' +
              JSON.stringify(res.errors.map((e) => e.message)));
    if (line !== undefined) assert.equal(hit.line, line);
}

/* Each rejected construct, in a function body, on line 2. */
const REJECTED = [
    ['let x = 1;', 'let is not supported; use var'],
    ['const x = 1;', 'const is only allowed at the top level'],
    ['var x = 40000.5;', 'too big for a fraction'],
    ['var f = (a) => a;', 'arrow functions'],
    ['var s = `x`;', 'template strings'],
    ['var o = this;', 'this is not supported'],
    ['for (var k in o) {}', 'for...in'],
    ['for (var k of o) {}', 'for...of'],
    ['try { } catch (e) { }', 'try/catch'],
    ['throw 1;', 'throw is not supported'],
    ['x: for (;;) {}', 'labels'],
    ['var r = /a/;', 'regular expressions'],
    ['var a = arguments;', 'arguments is not available'],
    ['var a = Math.random();', 'use rnd()'],
    ['var a = Math.PI;', 'Math.PI is not supported'],
    ['delete o.x;', 'delete is not supported'],
    ['var a = new Array(3);', 'new is only for Int32Array, Uint8Array and FixArray'],
    ['var a = new Uint8Array([1]);', 'takes just a length'],
    ['var a = 1 ** 2;', 'use Math.pow'],
    ['var a = "x" in o;', 'in operator'],
    ['var a = o instanceof Object;', 'instanceof'],
    ['var a = o?.x;', 'optional chaining'],
    ['var a = o ?? 1;', '?? is not supported'],
    ['var a = [...o];', 'spread'],
    ['var {a} = o;', 'destructuring'],
    ['var a = {x};', 'write x: x'],
    ['var a = {f() {}};', 'methods in a record'],
    ['var a = {[k]: 1};', 'computed keys'],
    ['var a = +s;', 'unary + does not convert'],
    ['var a = void 0;', 'void is not supported'],
    ['var a = [1, , 2];', 'holes'],
    ['var a = o.map(f);', '.map() is not supported; use a for loop'],
    ['var a = s.split(",");', 'use indexOf and substring'],
    ['console.log(1);', 'console is not available; use printh()'],
    ['nope = 1;', 'nope is not declared'],
    ['var a = nope;', 'nope is not declared'],
    ['rect(1, 2);', 'rect() takes 4 to 5 arguments'],
    ['var a = cls;', 'cls is a built-in function and cannot be used as a value'],
    ['init = 1;', 'init is a function and cannot be assigned'],
    ['SCREEN = 1;', 'SCREEN is a constant'],
    ['var a = deg(o);', 'deg() needs a number written in the code'],
    ['var a = fx(1);', 'fx is not declared'],
    ['var c = 5; var a = rgb(c);', 'rgb() needs a colour written in the code'],
    ['var a = [].sort();', 'a comparator'],
    ['var a = 0x100000000;', 'does not fit in 32 bits'],
    ['function inner() {}', 'functions must be declared at the top level'],
    ['break;', 'break outside a loop'],
    ['debugger;', 'debugger'],
    ['class A {}', 'classes'],
    ['async function f() {}', 'top level'],
    ['var a = x.length = 1;', 'length cannot be assigned'],
    ['o.x &&= 1;', '&&= is not supported'],
    ['var a = FONT.HUGE;', 'FONT has no HUGE'],
    ['var a = Math;', 'Math cannot be used as a value']
];

for (const [code, message] of REJECTED) {
    test('rejects: ' + code, () => {
        rejects('var o = {x: 1}, s = "", k = 0, f = null, x = [];\n' +
                'function g() { ' + code + ' }' + ENTRY, message, 2);
    });
}

test('rejects code at the top level, naming init()', () => {
    rejects('var a = 1;\na = 2;' + ENTRY, 'put code that runs once in init()', 2);
});

test('rejects let at the top level', () => {
    rejects('let a = 1;' + ENTRY, 'let is not supported', 1);
});

test('rejects a const that is not known when compiling', () => {
    rejects('var v = 3;\nconst K = v;' + ENTRY, 'needs a value known when compiling', 2);
});

test('a const refused once is not refused again where it is used', () => {
    const res = build('const K = [1, 2];\nfunction f() { return K[1]; }' + ENTRY);
    assert.deepEqual(res.errors.map((e) => e.message),
                     ['const K needs a value known when compiling; use var']);
});

test('rejects a const cycle', () => {
    rejects('const A = B;\nconst B = A;' + ENTRY, 'refers to itself');
});

test('rejects a global named like a native', () => {
    rejects('var t = 0;' + ENTRY, 't is a built-in function; choose another name for a global', 1);
    rejects('var grid = 0;' + ENTRY, 'grid is the grid system (grid.collapse, grid.fall, grid.fill, ...)', 1);
});

test('rejects a capture, and says what to do', () => {
    rejects('function g() {\n var n = 1;\n var f = function () { return n; };\n}' + ENTRY,
            'functions cannot capture locals', 3);
});

test('needs init and update', () => {
    rejects('function update() {}', 'main.js needs function init()');
    rejects('function init() {}', 'main.js needs function update()');
});

test('reports a syntax error with its line', () => {
    rejects('function init() {\n  var = ;\n}\nfunction update() {}', 'Unexpected token', 2);
});

test('checks cart.json', () => {
    const res = compileFiles({ 'cart.json': '{"id": "xyz", "name": "", "rate": 31, "colour": 1}',
                               'main.js': ENTRY });
    const m = res.errors.map((e) => e.message).join('\n');
    assert.match(m, /id must be 16/);
    assert.match(m, /name is required/);
    assert.match(m, /rate must be one of/);
    assert.match(m, /unknown setting colour/);
});

test('reports errors from several functions at once', () => {
    const res = build('function a() { let x; }\nfunction b() { let y; }' + ENTRY);
    assert.equal(res.errors.length, 2);
});

test('locals may share a native\'s name', () => {
    ok('function g(t, min) { var rect = t + min; return rect; }' + ENTRY);
});

test('folds constants, fractions and deg', () => {
    const { dis } = ok('const A = 3 * 4 + 1;\nconst B = "n" + A;\nconst H = 0.5;\n' +
                       'function init() { var a = A, b = B, c = H * 3, d = deg(90), ' +
                       'e = -1.0, f = FONT.WORLD, g = SCREEN + 2, h = "x" + H, ' +
                       'i = 7 / 2, j = 7 / 2.0, k = 2.0; }\nfunction update() {}');
    assert.match(dis, /PUSHI8 13/);
    assert.match(dis, /PUSHS "n13"/);
    assert.match(dis, /PUSHF 1\.5\n.*STL 2/);
    assert.match(dis, /PUSHF 0\.25\n.*STL 3/);
    assert.match(dis, /PUSHF -1\n.*STL 4/);
    assert.match(dis, /PUSHI8 2\n.*STL 5/);
    assert.match(dis, /PUSHI8 2\n.*STL 6/);
    assert.match(dis, /PUSHS "x0\.5"/);
    assert.match(dis, /PUSHI8 3\n.*STL 8/);
    assert.match(dis, /PUSHF 3\.5\n.*STL 9/);
    assert.match(dis, /PUSHF 2\n.*STL 10/);
});

test('wraps folded arithmetic at 32 bits', () => {
    const { dis } = ok('const A = 0x7fffffff + 1;\nconst B = 0xffffffff >>> 0;\n' +
                       'const C = -7 / 2;\nconst D = -7 % 2;\n' +
                       'function init() { var a = A, b = B, c = C, d = D; }\nfunction update() {}');
    assert.match(dis, /PUSHI32 -2147483648/);
    assert.match(dis, /PUSHI8 -1\n.*STL 1/);
    assert.match(dis, /PUSHI8 -3/);
    assert.match(dis, /PUSHI8 -1\n.*STL 3/);
});

test('drops functions nothing reaches', () => {
    const { res, dis } = ok('function used() {}\nfunction unused() {}\n' +
                            'function init() { used(); }\nfunction update() {}');
    assert.equal(res.stats.dropped, 1);
    assert.doesNotMatch(dis, /unused/);
});

test('keeps functions reached only through a global', () => {
    const { dis } = ok('var states = [a];\nvar cur = b;\nfunction a() {}\nfunction b() {}' + ENTRY);
    assert.match(dis, /function \d+ a /);
    assert.match(dis, /function \d+ b /);
});

test('namespaces resolve statically', () => {
    const { dis } = ok('var total = 0;\nfunction init() { brick.hit(2); brick.count = brick.MAX; }' +
                       '\nfunction update() {}',
                       { 'brick.js': 'const MAX = 9;\nvar count = 0;\n' +
                                     'function hit(n) { count += n; helper(); }\n' +
                                     'function helper() { total++; }',
                         'zz.js': 'var x = brick.MAX;' });
    assert.match(dis, /CALL brick\.hit/);
    assert.match(dis, /PUSHI8 9\n.*STG 0/);
});

test('a namespace sees main\'s globals', () => {
    ok('var total = 0;' + ENTRY, { 'brick.js': 'function f() { total++; }' });
});

test('a missing namespace member is an error', () => {
    const res = build('function init() { brick.nope(); }\nfunction update() {}',
                      { 'brick.js': 'var x;' });
    assert.match(res.errors[0].message, /brick.js has no nope/);
});

test('print takes the pieces of a concatenation', () => {
    const { dis } = ok('var s = 1;\nfunction init() { print("a" + s + "b" + (s + 1), 1, 2); ' +
                       'print(1 + 2 + "x"); print(s); }\nfunction update() {}');
    assert.match(dis, /PUSHI8 4\n.*PUSHS "a"\n.*LDG 0\n.*PUSHS "b"\n.*LDG 0\n.*PUSHI8 1\n.*ADD\n.*PUSHI8 1\n.*PUSHI8 2\n.*NCALL print 7/);
    assert.match(dis, /PUSHI8 1\n.*PUSHS "3x"\n.*NCALL print 2/);
    assert.match(dis, /PUSHI8 1\n.*LDG 0\n.*NCALL print 2/);
});

test('statement increments are one instruction', () => {
    const { dis } = ok('function init() { var i = 0; i++; i += 5; i -= 2; ++i; }' +
                       '\nfunction update() {}');
    assert.match(dis, /INCL 0 1\n.*INCL 0 5\n.*INCL 0 -2\n.*INCL 0 1/);
});

test('every expression form compiles and verifies', () => {
    ok(`
var g = 0, a = [1, 2, 3], o = { x: 1, y: "s" }, t8 = new Uint8Array(4);
var fns = [function (p) { return p * 2; }];
function f(x, y) { return x + y; }
function init() {
    var i, j = 0, s = "", r;
    r = a[i = 1] = o.x = g = 5;
    r = a[0]++ + ++a[1] + o.x-- + --o.x + g++ + ++g + j++ + ++j;
    a[2] += 3; o.y += "t"; g <<= 1; g >>>= 1; t8[j] |= 4; t8[0] ^= t8[1];
    r = (a[0] += 1) + (o.x *= 2);
    r = f(1) + f(1, 2, 3) + fns[0](4) + o.x;
    r = i ? j : g ? 1 : 2;
    r = (i && j) || (g && !i);
    i && f(1, 2);
    g || f(2);
    i ? f(1) : f(2);
    r = typeof s == "string" && s.length > 0 && a.length && t8.length;
    r = -i + ~j + !g + (i % 3) + (i / 2) + (i << 2) + (i >> 1) + (i >>> 3) + (i & j | g ^ 1);
    r = i < j || i <= j || i > j || i >= j || i != j || i !== j || i === j;
    r = s.charCodeAt(0) + s.indexOf("x") + a.indexOf(2) + parseInt("12") +
        String.fromCharCode(65).length + Math.pow(2, 3) + Math.abs(-1) +
        Math.floor(7) + Math.min(1, 2, 3) + Math.sign(-4) + Math.sqrt(16);
    a.push(4); a.pop(); a.unshift(0); a.shift(); a.splice(1, 1); a.reverse();
    r = a.slice(1).join(",") + s.substring(1, 2).toUpperCase().toLowerCase();
    o["y"] = o["x"]; r = o[s]; o[s] = 1;
    for (i = 0, j = 9; i < j; i++, j--) { if (i == 3) continue; if (i == 5) break; }
    while (i > 0) { i--; if (!i) break; }
    do { j++; } while (j < 20);
    for (;;) { break; }
    while (true) { if (g) break; }
    switch (i) { case 1: case 2: j = 1; break; case f(1, 2): j = 2; default: j = 3; }
    switch (i) { case 1: break; }
    r = function named(n) { return n ? named(n - 1) : 0; };
    r = null; r = undefined; r = true; r = false;
    printh("i=" + i); watch("g", g); cls(); prof();
    return;
    r = 1;
}
function update() { if (btn(0)) return f(1, 2); else return; }
function draw() { var w = textw("x" + g); }
`);
});

test('the cart carries its source', () => {
    const { res } = ok('function init() {}\nfunction update() {}');
    const src = inflate(readCart(res.cart).chunks.SRC_);
    assert.match(new TextDecoder().decode(src), /function update/);
});

test('a corrupted cart fails its CRC', () => {
    const { res } = ok(ENTRY);
    const bad = res.cart.slice();
    bad[30] ^= 1;
    assert.throws(() => readCart(bad), /CRC|PBG1|past/);
});

test('the verifier rejects broken bytecode', () => {
    const d = loadDefs();
    const src = 'var g = 0;\nfunction init() { if (g) g = 1; else g = 2; }\nfunction update() {}';
    const fresh = () => load(build(src).cart);
    const op = (name) => d.isa.op[name].code;
    const init = (p) => p.fns[p.entries.init];
    const jf = (code) => code.indexOf(op('JF'));

    let p = fresh();
    init(p).code[jf(init(p).code) + 1] += 1;                 /* jump mid-instruction */
    assert.throws(() => verify(d.isa, d.api, p), /mid-instruction/);

    p = fresh();
    init(p).code[0] = op('POP');                             /* pop an empty stack */
    assert.throws(() => verify(d.isa, d.api, p), /empty stack|meet|mid-instruction/);

    p = fresh();
    init(p).code[init(p).code.length - 1] = op('NOP');       /* fall off the end */
    assert.throws(() => verify(d.isa, d.api, p), /end/);

    p = fresh();
    init(p).maxstack = 0;
    assert.throws(() => verify(d.isa, d.api, p), /declares 0/);

    p = fresh();
    const c = init(p).code, at = c.indexOf(op('STG'));
    c[at + 1] = 9;                                            /* global out of range */
    assert.throws(() => verify(d.isa, d.api, p), /global out of range/);

    p = fresh();
    init(p).code[0] = 250;                                    /* unknown opcode */
    assert.throws(() => verify(d.isa, d.api, p), /unknown opcode/);
});

test('print keeps pieces only when no later piece has side effects', () => {
    const { dis } = ok('var a = [1], s = 0;\nfunction init() { print("x" + a + a.pop()); ' +
                       'print("y" + a + s++); print("z" + a + abs(s)); }\nfunction update() {}');
    assert.match(dis, /METHOD \.pop 0\n.*ADD\n.*NCALL print 2/);
    assert.match(dis, /STG 1\n.*ADD\n.*NCALL print 2/);
    assert.match(dis, /PUSHI8 3\n.*PUSHS "z"\n.*LDG 0\n.*LDG 1\n.*NCALL abs 1\n.*NCALL print 4/);
});
