/* The Arcade compiler: a folder of cart source to a .pbg.
 *
 * Parts, in order:
 *   messages     the rejected constructs and what to write instead
 *   constants    folding, with the VM's integer and string semantics
 *   Scope, Fn    name resolution; one Fn per function, with its emitter
 *   statements   control flow; every statement leaves the stack as it found it
 *   expressions  value() leaves one value, effect() leaves none
 *   calls        user functions, natives, methods, Math and the intrinsics
 *   Program      declarations, globals, reachability, the chunks
 *   compile()    the entry point
 *
 * Runs unchanged in Node and the browser: the caller passes in the .def
 * texts' parsed tables and a raw-deflate function. */

import * as acorn from 'acorn';
import { Writer, utf8, writeCart } from './cart.js';
import { buildAssets } from './assets.js';
import { DEFAULT_RGB } from './unpack.js';
import * as num from './numbers.js';

const FORMAT = 2;
const RATES = [20, 25, 33, 50];
const LEVELS = { LISTEN: 0, CONTROL: 1, OWN: 2 };
const MUSIC = ['listen', 'control', 'own'];
const MAX_PARTS = 32;

export class CompileError extends Error {
    constructor(message, node) { super(message); this.node = node; }
}

function nullProto(o) { return Object.assign(Object.create(null), o); }

function fail(node, message) { throw new CompileError(message, node); }

/* An emitter invariant broken: a compiler bug, not the cart's fault. */
function internal(message) { throw new Error('compiler bug: ' + message); }

/* ---- messages ---------------------------------------------------------- */

const UNSUPPORTED = nullProto({
    ArrowFunctionExpression: 'arrow functions (=>) are not supported; use function',
    ClassDeclaration: 'classes are not supported; use records and functions',
    ClassExpression: 'classes are not supported; use records and functions',
    TemplateLiteral: 'template strings are not supported; join strings with +',
    TaggedTemplateExpression: 'template strings are not supported',
    SpreadElement: 'spread (...) is not supported',
    RestElement: 'rest parameters (...) are not supported',
    AwaitExpression: 'await is not supported',
    YieldExpression: 'generators are not supported',
    ThisExpression: 'this is not supported; pass the object as an argument',
    ForInStatement: 'for...in is not supported',
    ForOfStatement: 'for...of is not supported; use a for loop with an index',
    TryStatement: 'try/catch is not supported',
    ThrowStatement: 'throw is not supported',
    WithStatement: 'with is not supported',
    LabeledStatement: 'labels are not supported',
    DebuggerStatement: 'debugger is not supported; use printh or watch',
    ObjectPattern: 'destructuring is not supported',
    ArrayPattern: 'destructuring is not supported',
    AssignmentPattern: 'default parameter values are not supported',
    ChainExpression: 'optional chaining (?.) is not supported',
    ImportExpression: 'import is not supported; every .js file in the cart is included',
    ImportDeclaration: 'import is not supported; every .js file in the cart is included',
    ExportNamedDeclaration: 'export is not supported; every file is a namespace',
    ExportDefaultDeclaration: 'export is not supported; every file is a namespace',
    ExportAllDeclaration: 'export is not supported; every file is a namespace',
    MetaProperty: 'new.target is not supported',
    Super: 'super is not supported',
    ParenthesizedExpression: null
});

function unsupported(node) {
    fail(node, UNSUPPORTED[node.type] || node.type + ' is not supported');
}

/* Methods JS has and the language does not, with what to write instead. */
const MISSING_METHODS = nullProto({
    toString: 'join it to a string: "" + n',
    forEach: 'use a for loop', map: 'use a for loop', filter: 'use a for loop',
    reduce: 'use a for loop', some: 'use a for loop', every: 'use a for loop',
    find: 'use a for loop', findIndex: 'use a for loop', fill: 'use a for loop',
    concat: 'push the elements, or join strings with +',
    split: 'use indexOf and substring', trim: null,
    includes: 'use indexOf(x) != -1', charAt: 'use charCodeAt or substring',
    substr: 'use substring', startsWith: 'use indexOf',
    endsWith: null, replace: null, match: null, search: null,
    padStart: 'the text library pads numbers', padEnd: null, repeat: null,
    toFixed: 'join it to a string: "" + x', hasOwnProperty: null,
    apply: null, call: null, bind: null, keys: null, values: null, entries: null
});

/* Globals JS has that a cart does not. */
const MISSING_GLOBALS = nullProto({
    console: 'use printh()', Date: 'use date()', JSON: null, Object: null,
    Array: 'use an array literal []', Number: null, Boolean: null,
    isNaN: 'there is no NaN', isFinite: 'every number is finite',
    NaN: 'there is no NaN', Infinity: 'every number is finite',
    Symbol: null, Promise: null, setTimeout: 'count ticks in update()',
    setInterval: 'count ticks in update()', window: null, document: null,
    arguments: 'name the parameters', eval: null,
    Float32Array: 'use FixArray', Float64Array: 'use FixArray', Int16Array: 'use Int32Array',
    Uint16Array: 'use Int32Array', Int8Array: 'use Int32Array',
    Uint32Array: 'use Int32Array', Uint8ClampedArray: 'use Uint8Array',
    Map: 'use a record: o[key]', Set: 'use a record: o[key] = true',
    RegExp: null
});

/* The language's own names, reserved at the top level. */
const BUILTINS = ['Math', 'String', 'parseInt', 'Int32Array', 'Uint8Array',
                  'FixArray', 'undefined', 'deg', 'rgb'];

const BINARY = nullProto({
    '+': 'ADD', '-': 'SUB', '*': 'MUL', '/': 'DIV', '%': 'MOD',
    '&': 'BAND', '|': 'BOR', '^': 'BXOR', '<<': 'SHL', '>>': 'SHR',
    '>>>': 'USHR', '==': 'EQ', '===': 'EQ', '!=': 'NE', '!==': 'NE',
    '<': 'LT', '<=': 'LE', '>': 'GT', '>=': 'GE'
});

const FUSED = nullProto({
    '<': 'JLT', '<=': 'JLE', '>': 'JGT', '>=': 'JGE',
    '==': 'JEQ', '===': 'JEQ', '!=': 'JNE', '!==': 'JNE'
});

const FUSED_NOT = nullProto({
    '<': 'JGE', '<=': 'JGT', '>': 'JLE', '>=': 'JLT',
    '==': 'JNE', '===': 'JNE', '!=': 'JEQ', '!==': 'JEQ'
});

/* the compare-and-jumps that have a form for a local against a small
 * integer, LDL, PUSHI8 and the jump in one */
const FUSED_LOCAL = nullProto({ JLT: 'JLTLI', JLE: 'JLELI', JGT: 'JGTLI',
                                JGE: 'JGELI' });

const BINARY_REJECTED = nullProto({
    in: 'the in operator is not supported; compare o[key] with null',
    instanceof: 'instanceof is not supported; use typeof',
    '**': '** is not supported; use Math.pow',
    '??': '?? is not supported; use ||'
});

/* ---- constants --------------------------------------------------------- *
 * A constant is a JS number (a 32-bit integer), a Fix (a fraction), a
 * string, null, or { fn: Fn }. undefined means "not a constant". They
 * follow numbers.js, which is what the VM computes at run time. */

function isFn(v) { return v !== null && typeof v === 'object' && 'fn' in v; }

const truthy = num.truthy;

function toStr(v) {
    if (v === null) return 'null';
    if (isFn(v)) return 'function';
    return num.text(v);
}

/* A number written with a point (or an exponent that leaves a fraction)
 * is a fraction; otherwise an integer. */
function numLiteral(node) {
    const v = node.value;
    if (typeof v === 'bigint') fail(node, 'BigInt is not supported');
    if (/[.]/.test(node.raw) || !Number.isInteger(v)) {
        const f = num.fromReal(v);
        if (!f) fail(node, node.raw + ' is too big for a fraction, which holds ' +
                     'up to 32767.99998');
        return f;
    }
    if (v > 0xffffffff || v < -0x80000000)
        fail(node, node.raw + ' does not fit in 32 bits');
    return v | 0;
}

function sameConst(a, b) {
    if (isFn(a) && isFn(b)) return a.fn === b.fn;
    if (num.isNum(a) && num.isNum(b)) return num.equal(a, b);
    return a === b;
}

/* ---- Scope, Fn ---------------------------------------------------------- */

class Scope {
    constructor(prog, file, outer) {
        this.prog = prog;
        this.file = file;
        this.outer = outer;
        this.locals = new Map();
        this.self = null;          /* a named function expression's own name */
    }

    /* Returns a symbol, or null for a name that is not declared anywhere. */
    resolve(name, node) {
        if (this.locals.has(name))
            return { kind: 'local', slot: this.locals.get(name) };
        if (this.self && this.self.name === name)
            return { kind: 'func', fn: this.self.fn };
        for (let o = this.outer; o; o = o.outer) {
            if (o.locals.has(name))
                fail(node, name + ' is a local variable of the function ' +
                     'around this one, and functions cannot capture locals; ' +
                     'pass it as an argument or make it a global');
        }
        return this.prog.lookup(name, this.file);
    }
}

class Fn extends Scope {
    constructor(prog, file, name, node, outer) {
        super(prog, file, outer);
        this.name = name;
        this.node = node;
        this.id = prog.fns.length;
        prog.fns.push(this);
        this.arity = node ? node.params.length : 0;
        this.nlocals = 0;
        this.free = [];
        this.code = [];
        this.lines = [];
        this.lastLine = -1;
        this.line = node ? node.loc.start.line : 0;
        this.depth = 0;
        this.maxDepth = 0;
        this.live = true;
        this.fixups = [];          /* { at, fn }: operands naming a function */
        this.refs = new Set();
        this.targets = [];         /* { brk, cont } for break and continue */
        this.index = -1;           /* final number, once reachability is known */
    }

    /* -- emitting -- */

    /* Code after a return or a jump is unreachable until a label some jump
     * reaches. It is still compiled, so its errors are reported, but
     * nothing of it is emitted. */
    emit(name, operands, pops, pushes) {
        const op = this.prog.isa.op[name];
        if (!op) internal('no opcode ' + name);
        if (!this.live) return;
        if (this.line !== this.lastLine) {
            this.lines.push([this.code.length, this.line]);
            this.lastLine = this.line;
        }
        this.code.push(op.code);
        const ks = op.operands;
        for (let i = 0; i < ks.length; i++) {
            const v = operands[i];
            switch (ks[i]) {
            case 'b':
                if (v < -128 || v > 127) internal(name + ' s8 ' + v);
                this.code.push(v & 0xff); break;
            case 'B':
                if (v < 0 || v > 255) internal(name + ' u8 ' + v);
                this.code.push(v); break;
            case 'h':
            case 'H':
                this.code.push(v & 0xff, (v >> 8) & 0xff); break;
            case 'i':
                this.code.push(v & 0xff, (v >> 8) & 0xff,
                               (v >> 16) & 0xff, (v >>> 24) & 0xff); break;
            }
        }
        const p = pops === undefined ? op.pops : pops;
        const q = pushes === undefined ? op.pushes : pushes;
        if (p < 0 || q < 0) internal(name + ' needs its stack effect');
        if (this.depth < p) internal(name + ' pops an empty stack');
        this.depth += q - p;
        if (this.depth > this.maxDepth) this.maxDepth = this.depth;
        if (name === 'JMP' || name === 'RET' || name === 'RETNULL')
            this.live = false;
    }

    /* An operand naming a function, patched once functions are numbered. */
    emitFn(name, fn, pops) {
        if (!this.live) return;
        this.emit(name, [0], pops, 1);
        this.fixups.push({ at: this.code.length - 2, fn: fn });
        this.refs.add(fn);
    }

    label() { return { pos: -1, depth: null, fix: [] }; }

    reach(L, depth) {
        if (L.depth === null) L.depth = depth;
        else if (L.depth !== depth)
            internal('stack ' + depth + ' and ' + L.depth + ' meet at a label');
    }

    /* the jump's offset is its last operand, after any in front */
    jump(name, L, front = []) {
        if (!this.live) return;
        const before = this.depth;
        this.emit(name, [...front, 0]);
        /* ANDJ and ORJ keep their operand when they jump; the rest arrive
         * with what they popped gone. */
        this.reach(L, name === 'ANDJ' || name === 'ORJ' ? before
                   : before - this.prog.isa.op[name].pops);
        const at = this.code.length - 2;
        if (L.pos >= 0) this.patch(at, L.pos);
        else L.fix.push(at);
    }

    place(L) {
        if (this.live) this.reach(L, this.depth);
        else if (L.depth !== null) {
            this.depth = L.depth;
            this.live = true;
        }
        L.pos = this.code.length;
        for (const at of L.fix) this.patch(at, L.pos);
    }

    patch(at, pos) {
        const off = pos - (at + 2);
        if (off < -32768 || off > 32767)
            fail(this.node, this.name + ' is too long to jump across; split it');
        this.code[at] = off & 0xff;
        this.code[at + 1] = (off >> 8) & 0xff;
    }

    at(node) { if (node.loc) this.line = node.loc.start.line; }

    pushConst(v) {
        if (v === null) this.emit('PUSHNULL', []);
        else if (typeof v === 'number') {
            if (v >= -128 && v <= 127) this.emit('PUSHI8', [v]);
            else if (v >= -32768 && v <= 32767) this.emit('PUSHI16', [v]);
            else this.emit('PUSHI32', [v]);
        } else if (num.isFix(v))
            this.emit('PUSHF', [v.raw]);
        else if (typeof v === 'string')
            this.emit('PUSHS', [this.prog.string(v)]);
        else this.emitFn('PUSHFN', v.fn, 0);
    }

    /* -- locals -- */

    declare(name, node) {
        if (this.locals.has(name)) return;
        if (this.nlocals >= 255) fail(node, this.name + ' has too many local variables');
        this.locals.set(name, this.nlocals++);
    }

    temp() {
        if (this.free.length) return this.free.pop();
        if (this.nlocals >= 255) fail(this.node, this.name + ' has too many local variables');
        return this.nlocals++;
    }

    release(slot) { this.free.push(slot); }

    /* -- the body -- */

    compileBody() {
        const node = this.node;
        if (node.generator) fail(node, 'generators are not supported');
        if (node.async) fail(node, 'async functions are not supported');
        for (const p of node.params) {
            if (p.type !== 'Identifier') unsupported(p);
            if (this.locals.has(p.name))
                fail(p, 'parameter ' + p.name + ' is named twice');
            this.declare(p.name, p);
        }
        if (node.type === 'FunctionExpression' && node.id)
            this.self = { name: node.id.name, fn: this };
        this.hoist(node.body.body);
        this.statements(node.body.body);
        if (this.live) this.emit('RETNULL', []);
    }

    /* var is function-scoped: declare every var in the body up front. */
    hoist(list) {
        const self = this;
        function walk(n) {
            if (!n || typeof n.type !== 'string') return;
            switch (n.type) {
            case 'VariableDeclaration':
                if (n.kind !== 'var')
                    fail(n, n.kind === 'let' ? 'let is not supported; use var'
                         : 'const is only allowed at the top level of a file; use var');
                for (const d of n.declarations) {
                    if (d.id.type !== 'Identifier') unsupported(d.id);
                    self.declare(d.id.name, d);
                }
                break;
            case 'FunctionDeclaration':
                fail(n, 'functions must be declared at the top level of a file; ' +
                     'a function expression assigned to a variable works here');
                break;
            case 'FunctionExpression':
            case 'ArrowFunctionExpression':
                return;
            }
            for (const k in n) {
                if (k === 'loc') continue;
                const c = n[k];
                if (Array.isArray(c)) c.forEach(walk);
                else if (c && typeof c.type === 'string') walk(c);
            }
        }
        list.forEach(walk);
    }

    /* ---- statements ---- */

    statements(list) { for (const s of list) this.statement(s); }

    statement(s) {
        this.at(s);
        switch (s.type) {
        case 'EmptyStatement':
            return;
        case 'BlockStatement':
            return this.statements(s.body);
        case 'ExpressionStatement':
            if (s.directive) return;
            return this.effect(s.expression);
        case 'VariableDeclaration':
            for (const d of s.declarations) {
                if (!d.init) continue;
                this.at(d);
                this.value(d.init);
                this.emit('STL', [this.locals.get(d.id.name)]);
            }
            return;
        case 'IfStatement': {
            const end = this.label();
            if (s.alternate) {
                const other = this.label();
                this.cond(s.test, other, false);
                this.statement(s.consequent);
                if (this.live) this.jump('JMP', end);
                this.place(other);
                this.statement(s.alternate);
            } else {
                this.cond(s.test, end, false);
                this.statement(s.consequent);
            }
            return this.place(end);
        }
        case 'ForStatement':
            if (s.init) {
                if (s.init.type === 'VariableDeclaration') this.statement(s.init);
                else this.effect(s.init);
            }
            return this.loop(s.test, s.body, s.update);
        case 'WhileStatement':
            return this.loop(s.test, s.body, null);
        case 'DoWhileStatement': {
            const body = this.label(), cont = this.label(), brk = this.label();
            this.place(body);
            this.targets.push({ brk: brk, cont: cont });
            this.statement(s.body);
            this.targets.pop();
            this.place(cont);
            this.at(s.test);
            this.cond(s.test, body, true);
            return this.place(brk);
        }
        case 'SwitchStatement':
            return this.switchStatement(s);
        case 'BreakStatement':
        case 'ContinueStatement': {
            if (s.label) unsupported({ type: 'LabeledStatement', loc: s.loc });
            const brk = s.type === 'BreakStatement';
            for (let i = this.targets.length - 1; i >= 0; i--) {
                const L = brk ? this.targets[i].brk : this.targets[i].cont;
                if (L) return this.jump('JMP', L);
            }
            return fail(s, (brk ? 'break' : 'continue') + ' outside a loop');
        }
        case 'ReturnStatement':
            if (s.argument) {
                this.value(s.argument);
                this.emit('RET', []);
            } else this.emit('RETNULL', []);
            return;
        case 'FunctionDeclaration':
            return fail(s, 'functions must be declared at the top level of a file');
        default:
            return unsupported(s);
        }
    }

    /* Test at the bottom: one conditional jump per iteration. */
    loop(test, body, update) {
        const top = this.label(), cont = this.label(), brk = this.label();
        const check = this.label();
        const forever = !test || (this.prog.fold(test, this) !== undefined &&
                                  truthy(this.prog.fold(test, this)));
        /* The test's backward jump reaches the top, though it comes later. */
        if (this.live) top.depth = this.depth;
        if (!forever) this.jump('JMP', check);
        this.place(top);
        this.targets.push({ brk: brk, cont: cont });
        this.statement(body);
        this.targets.pop();
        this.place(cont);
        if (update) { this.at(update); this.effect(update); }
        if (forever) this.jump('JMP', top);
        else {
            this.place(check);
            this.at(test);
            this.cond(test, top, true);
        }
        this.place(brk);
    }

    switchStatement(s) {
        this.value(s.discriminant);
        const t = this.temp();
        this.emit('STL', [t]);
        const brk = this.label();
        const entry = s.cases.map(() => this.label());
        let dflt = null;
        s.cases.forEach((c, i) => {
            if (!c.test) { dflt = entry[i]; return; }
            this.at(c);
            this.emit('LDL', [t]);
            this.value(c.test);
            this.emit('EQ', []);
            this.jump('JT', entry[i]);
        });
        this.jump('JMP', dflt || brk);
        this.release(t);
        this.targets.push({ brk: brk, cont: null });
        s.cases.forEach((c, i) => {
            this.place(entry[i]);
            this.statements(c.consequent);
        });
        this.targets.pop();
        this.place(brk);
    }

    /* Jumps to L when node's truth is `when`; falls through otherwise. */
    cond(node, L, when) {
        this.at(node);
        const c = this.prog.fold(node, this);
        if (c !== undefined) {
            if (truthy(c) === when) this.jump('JMP', L);
            return;
        }
        if (node.type === 'UnaryExpression' && node.operator === '!')
            return this.cond(node.argument, L, !when);
        if (node.type === 'LogicalExpression' &&
            (node.operator === '&&' || node.operator === '||')) {
            const and = node.operator === '&&';
            if (and !== when) {
                /* a && b, jump when false; a || b, jump when true */
                this.cond(node.left, L, when);
                this.cond(node.right, L, when);
            } else {
                const skip = this.label();
                this.cond(node.left, skip, !when);
                this.cond(node.right, L, when);
                this.place(skip);
            }
            return;
        }
        /* A comparison jumps by itself; on false, by its opposite, which
         * is exact: numbers and strings are totally ordered, and a pair
         * that cannot be compared is an error either way. */
        const fused = when ? FUSED[node.operator] : FUSED_NOT[node.operator];
        if (node.type === 'BinaryExpression' && fused) {
            const l = FUSED_LOCAL[fused] && this.variable(node.left);
            const k = l && l.kind === 'local'
                ? this.prog.fold(node.right, this) : undefined;
            if (typeof k === 'number' && k >= -128 && k <= 127) {
                this.at(node);
                return this.jump(FUSED_LOCAL[fused], L, [l.slot, k]);
            }
            this.value(node.left);
            this.value(node.right);
            this.at(node);
            return this.jump(fused, L);
        }
        this.value(node);
        this.jump(when ? 'JT' : 'JF', L);
    }

    /* ---- expressions ---- */

    value(node) {
        this.at(node);
        const c = this.prog.fold(node, this);
        if (c !== undefined) return this.pushConst(c);
        switch (node.type) {
        case 'Literal':
            if (node.regex) fail(node, 'regular expressions are not supported');
            return internal('literal not folded');
        case 'Identifier':
            return this.load(this.name2sym(node), node);
        case 'ArrayExpression':
            for (const e of node.elements) {
                if (!e) fail(node, 'arrays cannot have holes');
                if (e.type === 'SpreadElement') unsupported(e);
                this.value(e);
            }
            if (node.elements.length > 65535) fail(node, 'array literal too long');
            return this.emit('NEWARR', [node.elements.length],
                             node.elements.length, 1);
        case 'ObjectExpression':
            return this.record(node);
        case 'FunctionExpression': {
            const fn = this.prog.functionExpression(node, this, this.file);
            return this.emitFn('PUSHFN', fn, 0);
        }
        case 'UnaryExpression':
            return this.unary(node);
        case 'BinaryExpression': {
            if (BINARY_REJECTED[node.operator])
                fail(node, BINARY_REJECTED[node.operator]);
            this.value(node.left);
            this.value(node.right);
            this.at(node);
            return this.emit(BINARY[node.operator], []);
        }
        case 'LogicalExpression': {
            if (BINARY_REJECTED[node.operator])
                fail(node, BINARY_REJECTED[node.operator]);
            const end = this.label();
            this.value(node.left);
            this.jump(node.operator === '&&' ? 'ANDJ' : 'ORJ', end);
            this.value(node.right);
            return this.place(end);
        }
        case 'ConditionalExpression': {
            const other = this.label(), end = this.label();
            this.cond(node.test, other, false);
            this.value(node.consequent);
            this.jump('JMP', end);
            this.place(other);
            this.value(node.alternate);
            return this.place(end);
        }
        case 'AssignmentExpression':
            return this.assign(node, true);
        case 'UpdateExpression':
            return this.update(node, true);
        case 'SequenceExpression':
            node.expressions.forEach((e, i) => {
                if (i < node.expressions.length - 1) this.effect(e);
                else this.value(e);
            });
            return;
        case 'MemberExpression':
            return this.member(node);
        case 'CallExpression':
            return this.call(node, true);
        case 'NewExpression':
            return this.newExpression(node);
        default:
            return unsupported(node);
        }
    }

    effect(node) {
        this.at(node);
        switch (node.type) {
        case 'AssignmentExpression': return this.assign(node, false);
        case 'UpdateExpression': return this.update(node, false);
        case 'CallExpression': return this.call(node, false);
        case 'SequenceExpression':
            return node.expressions.forEach((e) => this.effect(e));
        case 'LogicalExpression':
            if (node.operator === '&&' || node.operator === '||') {
                const end = this.label();
                this.cond(node.left, end, node.operator === '||');
                this.effect(node.right);
                return this.place(end);
            }
            break;
        case 'ConditionalExpression': {
            const other = this.label(), end = this.label();
            this.cond(node.test, other, false);
            this.effect(node.consequent);
            if (this.live) this.jump('JMP', end);
            this.place(other);
            this.effect(node.alternate);
            return this.place(end);
        }
        }
        this.value(node);
        this.emit('POP', []);
    }

    name2sym(node) {
        const name = node.name;
        const sym = this.resolve(name, node);
        if (sym) return sym;
        if (name in MISSING_GLOBALS) {
            const hint = MISSING_GLOBALS[name];
            fail(node, name + ' is not available' + (hint ? '; ' + hint : ''));
        }
        return fail(node, name + ' is not declared');
    }

    load(sym, node) {
        switch (sym.kind) {
        case 'local': return this.emit('LDL', [sym.slot]);
        case 'global': return this.emit('LDG', [sym.slot]);
        case 'func': return this.emitFn('PUSHFN', sym.fn, 0);
        case 'const': return this.pushConst(this.prog.constValue(sym));
        case 'native':
            return fail(node, sym.def.name + ' is a built-in function and ' +
                        'cannot be used as a value; call it');
        case 'ns':
            return fail(node, node.name + ' is a file of this cart; ' +
                        'use its names as ' + node.name + '.name');
        case 'group':
            return fail(node, node.name + ' is a group of constants; ' +
                        'use ' + node.name + '.NAME');
        case 'apins':
            return fail(node, node.name + ' is a set of built-in functions; ' +
                        'call one, as ' + node.name + '.name()');
        case 'builtin':
            if (node.name === 'undefined') return this.emit('PUSHNULL', []);
            return fail(node, node.name + ' cannot be used as a value');
        }
        return internal('symbol ' + sym.kind);
    }

    /* The symbol an assignment writes: a local, or a global of any file. */
    target(node) {
        let sym;
        if (node.type === 'Identifier') sym = this.name2sym(node);
        else sym = this.nsMember(node);
        if (sym && (sym.kind === 'local' || sym.kind === 'global')) return sym;
        if (!sym) return null;
        const what = { func: 'a function', const: 'a constant',
                       native: 'a built-in function', ns: 'a file',
                       group: 'a group of constants', builtin: 'built in' };
        return fail(node, (node.name || node.property.name) + ' is ' +
                    what[sym.kind] + ' and cannot be assigned');
    }

    store(sym) {
        this.emit(sym.kind === 'local' ? 'STL' : 'STG', [sym.slot]);
    }

    /* ns.name resolved statically, or null when node is not one. */
    nsMember(node) {
        if (node.type !== 'MemberExpression' || node.computed ||
            node.object.type !== 'Identifier')
            return null;
        const base = this.resolve(node.object.name, node.object);
        if (!base) return null;
        if (base.kind === 'ns') {
            const sym = base.file.scope.get(node.property.name);
            if (!sym)
                fail(node, node.object.name + '.js has no ' + node.property.name);
            return sym;
        }
        return null;
    }

    /* The local or global a plain name, or a file's name, is; null for
     * anything else */
    variable(node) {
        let sym = null;
        if (node.type === 'Identifier') sym = this.resolve(node.name, node);
        else if (node.type === 'MemberExpression') sym = this.nsMember(node);
        return sym && (sym.kind === 'local' || sym.kind === 'global') ? sym : null;
    }

    member(node) {
        if (node.optional) unsupported({ type: 'ChainExpression', loc: node.loc });
        const ns = this.nsMember(node);
        if (ns) return this.load(ns, node.property);
        if (node.object.type === 'Identifier') {
            const base = this.resolve(node.object.name, node.object);
            if (base && base.kind === 'group')
                fail(node, node.object.name + ' has no ' + node.property.name);
            if (base && base.kind === 'builtin' && base.name !== 'undefined')
                fail(node, node.object.name + '.' + (node.property.name || '[]') +
                     ' is not supported');
        }
        const key = this.propertyKey(node);
        /* a global's field, and a global indexed by a local, in one */
        const g = key !== 'length' && this.variable(node.object);
        if (g && g.kind === 'global') {
            if (key !== null) {
                this.at(node);
                return this.emit('GETGP', [g.slot, this.prog.string(key),
                                           this.prog.site()]);
            }
            const i = this.variable(node.property);
            if (i && i.kind === 'local') {
                this.at(node);
                return this.emit('GETGL', [g.slot, i.slot]);
            }
        }
        this.value(node.object);
        this.at(node);
        if (key === 'length') return this.emit('LEN', []);
        if (key !== null)
            return this.emit('GETPROP', [this.prog.string(key), this.prog.site()]);
        this.value(node.property);
        this.emit('GETIDX', []);
    }

    /* The key of o.name or o["name"]; null for a computed index. */
    propertyKey(node) {
        if (!node.computed) return node.property.name;
        if (node.property.type === 'Literal' && typeof node.property.value === 'string')
            return node.property.value;
        return null;
    }

    record(node) {
        const keys = [];
        for (const p of node.properties) {
            if (p.type === 'SpreadElement') unsupported(p);
            if (p.kind !== 'init') fail(p, 'getters and setters are not supported');
            if (p.method) fail(p, 'methods in a record are not supported; ' +
                               'write name: function () {...}');
            if (p.shorthand) fail(p, 'write ' + p.key.name + ': ' + p.key.name);
            if (p.computed) fail(p, 'computed keys are not supported');
            let k;
            if (p.key.type === 'Identifier') k = p.key.name;
            else if (p.key.type === 'Literal' && typeof p.key.value === 'string')
                k = p.key.value;
            else fail(p.key, 'record keys must be names or strings');
            if (keys.indexOf(k) >= 0) fail(p.key, 'key ' + k + ' appears twice');
            keys.push(k);
            this.value(p.value);
        }
        if (keys.length > 255) fail(node, 'a record has at most 255 keys');
        this.at(node);
        this.emit('NEWOBJ', [this.prog.shape(keys)], keys.length, 1);
    }

    unary(node) {
        switch (node.operator) {
        case '-': this.value(node.argument); return this.emit('NEG', []);
        case '~': this.value(node.argument); return this.emit('BNOT', []);
        case '!': this.value(node.argument); return this.emit('NOT', []);
        case 'typeof': this.value(node.argument); return this.emit('TYPEOF', []);
        case '+':
            return fail(node, 'unary + does not convert here; use parseInt for strings');
        case 'delete':
            return fail(node, 'delete is not supported; set the field to null');
        case 'void':
            return fail(node, 'void is not supported; use null');
        }
        return unsupported(node);
    }

    /* ---- assignment ---- */

    compound(node) {
        const op = node.operator.slice(0, -1);
        if (!BINARY[op] || op.length === 0 || node.operator.length < 2)
            fail(node, node.operator + ' is not supported');
        return op;
    }

    assign(node, want) {
        const left = node.left;
        if (left.type === 'ObjectPattern' || left.type === 'ArrayPattern') unsupported(left);
        const op = node.operator === '=' ? null : this.compound(node);
        const sym = this.target(left);
        if (sym) {
            if (op === null) this.value(node.right);
            else {
                const k = this.prog.fold(node.right, this);
                if (!want && sym.kind === 'local' && typeof k === 'number' &&
                    (op === '+' || op === '-')) {
                    const d = op === '+' ? k : -k;
                    if (d >= -128 && d <= 127) {
                        this.at(node);
                        return this.emit('INCL', [sym.slot, d]);
                    }
                }
                this.load(sym, left);
                this.value(node.right);
                this.at(node);
                this.emit(BINARY[op], []);
            }
            if (want) this.emit('DUP', []);
            return this.store(sym);
        }
        if (left.type !== 'MemberExpression')
            return fail(left, 'cannot assign to this');
        this.storeMember(left, want, () => {
            if (op === null) return this.value(node.right);
            this.value(node.right);
            this.at(node);
            this.emit(BINARY[op], []);
        }, op !== null);
    }

    /* o.k = v and a[i] = v. With `read`, the old value is fetched first and
     * produce() combines it with the new; with `want` the stored value is
     * left on the stack, kept in a temporary across the store. */
    storeMember(left, want, produce, read, keepOld) {
        if (left.optional) unsupported({ type: 'ChainExpression', loc: left.loc });
        const key = this.propertyKey(left);
        if (key === 'length') fail(left, 'length cannot be assigned');
        if (left.object.type === 'Identifier') {
            const base = this.resolve(left.object.name, left.object);
            if (base && (base.kind === 'group' || base.kind === 'builtin'))
                fail(left, left.object.name + '.' + key + ' cannot be assigned');
        }
        this.value(left.object);
        if (key === null) this.value(left.property);
        const site = key === null ? 0 : this.prog.site();
        const kidx = key === null ? 0 : this.prog.string(key);
        if (read) {
            this.at(left);
            if (key === null) {
                this.emit('DUP2', []);
                this.emit('GETIDX', []);
            } else {
                this.emit('DUP', []);
                this.emit('GETPROP', [kidx, site]);
            }
        }
        let t = -1;
        if (want && keepOld) {
            t = this.temp();
            this.emit('DUP', []);
            this.emit('STL', [t]);
        }
        produce();
        if (want && !keepOld) {
            t = this.temp();
            this.emit('DUP', []);
            this.emit('STL', [t]);
        }
        this.at(left);
        if (key === null) this.emit('SETIDX', []);
        else this.emit('SETPROP', [kidx, site]);
        if (want) {
            this.emit('LDL', [t]);
            this.release(t);
        }
    }

    update(node, want) {
        const d = node.operator === '++' ? 1 : -1;
        const arg = node.argument;
        const sym = this.target(arg);
        const old = want && !node.prefix;
        if (sym && sym.kind === 'local') {
            if (old) this.emit('LDL', [sym.slot]);
            this.emit('INCL', [sym.slot, d]);
            if (want && node.prefix) this.emit('LDL', [sym.slot]);
            return;
        }
        if (sym) {
            this.emit('LDG', [sym.slot]);
            if (old) this.emit('DUP', []);
            this.emit('PUSHI8', [d]);
            this.emit('ADD', []);
            if (want && node.prefix) this.emit('DUP', []);
            return this.store(sym);
        }
        if (arg.type !== 'MemberExpression')
            return fail(arg, node.operator + ' needs a variable, field or element');
        this.storeMember(arg, want, () => {
            this.emit('PUSHI8', [d]);
            this.emit('ADD', []);
        }, true, old);
    }

    /* ---- calls ---- */

    args(list) {
        for (const a of list) {
            if (a.type === 'SpreadElement') unsupported(a);
            this.value(a);
        }
        if (list.length > 255) fail(list[255], 'too many arguments');
        return list.length;
    }

    result(pushes, want) {
        if (want && !pushes) this.emit('PUSHNULL', []);
        if (!want && pushes) this.emit('POP', []);
    }

    call(node, want) {
        if (node.optional) unsupported({ type: 'ChainExpression', loc: node.loc });
        const callee = node.callee;
        const args = node.arguments;
        if (callee.type === 'Identifier') {
            const sym = this.name2sym(callee);
            switch (sym.kind) {
            case 'func': return this.userCall(sym.fn, args, node, want);
            case 'native': return this.nativeCall(sym.def, args, node, want);
            case 'apins':
                if (sym.def) return this.nativeCall(sym.def, args, node, want);
                break;
            case 'builtin': return this.builtinCall(callee.name, args, node, want);
            case 'const':
                return fail(callee, callee.name + ' is a constant, not a function');
            }
            this.load(sym, callee);
            return this.valueCall(args, node, want);
        }
        if (callee.type === 'MemberExpression') {
            const ns = this.nsMember(callee);
            if (ns && ns.kind === 'func') return this.userCall(ns.fn, args, node, want);
            if (ns) {
                this.load(ns, callee.property);
                return this.valueCall(args, node, want);
            }
            if (callee.object.type === 'Identifier' && !callee.computed) {
                const base = this.resolve(callee.object.name, callee.object);
                if (base && base.kind === 'builtin') {
                    if (base.name === 'Math')
                        return this.mathCall(callee.property.name, args, node, want);
                    if (base.name === 'String' && callee.property.name === 'fromCharCode')
                        return this.intrinsic('fromCharCode', args, node, want);
                    fail(callee, callee.object.name + '.' + callee.property.name +
                         ' is not supported');
                }
                if (base && base.kind === 'group')
                    fail(callee, callee.object.name + '.' + callee.property.name +
                         ' is a constant, not a function');
                if (base && base.kind === 'apins') {
                    const def = this.prog.api.namespaces[base.name][callee.property.name];
                    if (!def)
                        fail(callee, callee.object.name + ' has no function ' +
                             callee.property.name);
                    return this.nativeCall(def, args, node, want);
                }
            }
            const key = this.propertyKey(callee);
            const m = key === null ? null :
                this.prog.isa.methods.find((x) => x.name === key);
            if (m) {
                if (args.length < m.min || args.length > m.max)
                    fail(node, '.' + key + '() takes ' + range(m.min, m.max) +
                         ' argument' + (m.max === 1 ? '' : 's') +
                         (key === 'sort' ? ': a comparator, which the default ' +
                          'string order makes necessary' : ''));
                this.value(callee.object);
                const argc = this.args(args);
                this.at(node);
                this.emit('METHOD', [m.id, argc], argc + 1, 1);
                return this.result(1, want);
            }
            if (key !== null && key in MISSING_METHODS) {
                const hint = MISSING_METHODS[key];
                fail(callee.property, '.' + key + '() is not supported' +
                     (hint ? '; ' + hint : ''));
            }
            this.member(callee);
            return this.valueCall(args, node, want);
        }
        this.value(callee);
        return this.valueCall(args, node, want);
    }

    valueCall(args, node, want) {
        const argc = this.args(args);
        this.at(node);
        this.emit('CALLV', [argc], argc + 1, 1);
        this.result(1, want);
    }

    userCall(fn, args, node, want) {
        args.forEach((a, i) => {
            if (a.type === 'SpreadElement') unsupported(a);
            if (i < fn.arity) this.value(a);
            else this.effect(a);
        });
        for (let i = args.length; i < fn.arity; i++) this.emit('PUSHNULL', []);
        this.at(node);
        this.emitFn('CALL', fn, fn.arity);
        this.result(1, want);
    }

    nativeCall(def, args, node, want) {
        const name = node.callee.name || def.label || def.name;
        if (args.length < def.min || args.length > def.max)
            fail(node, name + '() takes ' + range(def.min, def.max) + ' argument' +
                 (def.max === 1 ? '' : 's'));
        this.prog.use(def, node);
        let argc = 0;
        args.forEach((a, i) => {
            if (a.type === 'SpreadElement') unsupported(a);
            if (i === 0 && def.parts) {
                let parts = concatParts(a, this);
                if (parts.length > MAX_PARTS) parts = [a];
                this.pushConst(parts.length);
                argc++;
                for (const p of parts) { this.value(p); argc++; }
            } else { this.value(a); argc++; }
        });
        if (argc > 255) fail(node, 'too many arguments');
        this.at(node);
        this.emit('NCALL', [def.slot, argc], argc, def.returns ? 1 : 0);
        this.result(def.returns, want);
    }

    intrinsic(name, args, node, want) {
        const x = this.prog.isa.intrinsics.find((i) => i.name === name);
        if (args.length < x.min || args.length > x.max)
            fail(node, name + '() takes ' + range(x.min, x.max) + ' argument' +
                 (x.max === 1 ? '' : 's'));
        const argc = this.args(args);
        this.at(node);
        this.emit('INTR', [x.id, argc], argc, 1);
        this.result(1, want);
    }

    builtinCall(name, args, node, want) {
        switch (name) {
        case 'parseInt': return this.intrinsic('parseInt', args, node, want);
        case 'deg':
            return fail(node, 'deg() needs a number written in the code; ' +
                        'at run time, divide by 360.0');
        case 'rgb':
            return fail(node, 'rgb() needs a colour written in the code, such as ' +
                        'rgb(0xff8800); at run time, use the colour\'s number');
        case 'Int32Array': case 'Uint8Array': case 'FixArray':
            return fail(node, 'write new ' + name + '(length)');
        }
        return fail(node, name + ' is not a function');
    }

    mathCall(name, args, node, want) {
        const one = (f) => {
            if (args.length !== 1) fail(node, 'Math.' + name + '() takes 1 argument');
            return f();
        };
        const natives = this.prog.api.natives;
        switch (name) {
        case 'floor': return this.nativeCall(natives.flr, args, node, want);
        case 'ceil': return this.nativeCall(natives.ceil, args, node, want);
        case 'round': case 'trunc':
            return this.intrinsic(name, args, node, want);
        case 'abs': return this.nativeCall(natives.abs, args, node, want);
        case 'sign': return this.intrinsic('sign', args, node, want);
        case 'sqrt': return this.nativeCall(natives.sqrt, args, node, want);
        case 'pow': return this.intrinsic('pow', args, node, want);
        case 'min': case 'max': {
            if (args.length === 0) fail(node, 'Math.' + name + '() needs an argument');
            const def = natives[name];
            this.prog.use(def, node);
            this.value(args[0]);
            for (let i = 1; i < args.length; i++) {
                this.value(args[i]);
                this.at(node);
                this.emit('NCALL', [def.slot, 2], 2, 1);
            }
            return this.result(1, want);
        }
        case 'random':
            return fail(node, 'Math.random is not supported; use rnd(), which ' +
                        'repeats from a seed');
        }
        return fail(node, 'Math.' + name + ' is not supported');
    }

    newExpression(node) {
        const c = node.callee;
        const ok = c.type === 'Identifier' &&
            (c.name === 'Int32Array' || c.name === 'Uint8Array' ||
             c.name === 'FixArray');
        const sym = ok ? this.resolve(c.name, c) : null;
        if (!sym || sym.kind !== 'builtin')
            fail(node, 'new is only for Int32Array, Uint8Array and FixArray; ' +
                 'make records with { }');
        if (node.arguments.length !== 1 ||
            node.arguments[0].type === 'ArrayExpression')
            fail(node, 'new ' + c.name + '(length) takes just a length');
        this.value(node.arguments[0]);
        this.at(node);
        this.emit(c.name === 'Int32Array' ? 'NEWI32' :
                  c.name === 'FixArray' ? 'NEWFXA' : 'NEWU8', []);
    }
}

function range(min, max) {
    return min === max ? String(min) : min + ' to ' + max;
}

/* The pieces of a concatenation, so print("score " + s) passes "score "
 * and s without building the string. A + is split only where one side is
 * known to be a string, which is exactly where JS would join. The native
 * turns each piece to text only once all are evaluated, so a piece that
 * could change an earlier one -- an array printed before a.pop() -- keeps
 * the whole as one string, joined as JS joins it. */
function concatParts(node, scope) {
    const parts = splitParts(node, scope);
    for (let i = 1; i < parts.length; i++)
        if (sideEffects(parts[i], scope)) return [node];
    return parts;
}

/* Natives that write into an array they are given. */
const WRITES_ARGUMENTS = ['date', 'mapobj'];

function sideEffects(node, scope) {
    let found = false;
    function walk(n) {
        if (found || !n || typeof n.type !== 'string') return;
        switch (n.type) {
        case 'AssignmentExpression':
        case 'UpdateExpression':
            found = true;
            return;
        case 'FunctionExpression':
            return;
        case 'CallExpression': {
            const c = n.callee;
            const sym = c.type === 'Identifier' ? scope.resolve(c.name, c) : null;
            if (!sym || !(sym.kind === 'native' || (sym.kind === 'apins' && sym.def)) ||
                WRITES_ARGUMENTS.indexOf(sym.def.name) >= 0) {
                found = true;
                return;
            }
        }
        }
        for (const k in n) {
            if (k === 'loc') continue;
            const c = n[k];
            if (Array.isArray(c)) c.forEach(walk);
            else if (c && typeof c.type === 'string') walk(c);
        }
    }
    walk(node);
    return found;
}

function splitParts(node, scope) {
    function stringy(n) {
        if (n.type === 'BinaryExpression' && n.operator === '+')
            return stringy(n.left) || stringy(n.right);
        return typeof scope.prog.fold(n, scope) === 'string';
    }
    function parts(n) {
        if (n.type === 'BinaryExpression' && n.operator === '+' &&
            scope.prog.fold(n, scope) === undefined &&
            (stringy(n.left) || stringy(n.right))) {
            const l = stringy(n.left) ? parts(n.left) : [n.left];
            const r = stringy(n.right) ? parts(n.right) : [n.right];
            return l.concat(r);
        }
        return [n];
    }
    return parts(node);
}

/* ---- Program ------------------------------------------------------------ */

class Program {
    constructor(isa, api) {
        this.isa = isa;
        this.api = api;
        this.fns = [];
        this.files = [];
        this.byNs = new Map();
        this.main = null;
        this.globals = [];
        this.strings = [];
        this.stringIndex = new Map();
        this.shapes = [];
        this.shapeIndex = new Map();
        this.sites = 0;
        this.version = 1;
        this.level = 0;
        this.errors = [];
        this.start = null;
    }

    error(e, file) {
        if (!(e instanceof CompileError)) throw e;
        const loc = e.node && e.node.loc ? e.node.loc.start : { line: 0, column: 0 };
        this.errors.push({ file: file.name, line: loc.line, column: loc.column + 1,
                           message: e.message });
    }

    guard(file, fn) {
        try { fn(); } catch (e) { this.error(e, file); }
    }

    string(s) {
        let i = this.stringIndex.get(s);
        if (i === undefined) {
            /* the engine keeps 1024 more for keys made as it runs */
            if (this.strings.length >= 65535 - 1024)
                throw new Error('too many strings');
            i = this.strings.length;
            this.strings.push(s);
            this.stringIndex.set(s, i);
        }
        return i;
    }

    shape(keys) {
        const id = keys.join('\0');
        let i = this.shapeIndex.get(id);
        if (i === undefined) {
            i = this.shapes.length;
            this.shapes.push(keys.map((k) => this.string(k)));
            this.shapeIndex.set(id, i);
        }
        return i;
    }

    site() {
        if (this.sites >= 65535) throw new Error('too many property accesses');
        return this.sites++;
    }

    use(def, node) {
        if (LEVELS[def.level] > this.level)
            fail(node, (def.label || def.name) + '() needs the cart\'s music level to be ' +
                 MUSIC[LEVELS[def.level]]);
        if (def.version > this.version) this.version = def.version;
    }

    lookup(name, file) {
        let sym = file.scope.get(name);
        if (sym) return sym;
        if (file !== this.main && this.main) {
            sym = this.main.scope.get(name);
            if (sym) return sym;
        }
        if (this.byNs.has(name)) return { kind: 'ns', file: this.byNs.get(name) };
        const api = this.api;
        if (api.consts[name])
            return { kind: 'const', api: api.consts[name] };
        if (api.groups[name]) return { kind: 'group', name: name };
        /* color is both: color(c) the pen, color.mix() and the rest a
         * namespace; called by itself, the native */
        if (api.namespaces && api.namespaces[name])
            return { kind: 'apins', name: name, def: api.natives[name] };
        if (api.natives[name]) return { kind: 'native', def: api.natives[name] };
        if (BUILTINS.indexOf(name) >= 0) return { kind: 'builtin', name: name };
        return null;
    }

    /* What a reserved name already is, for the error that refuses it */
    whatIs(name) {
        const api = this.api;
        if (this.byNs.has(name)) return 'the name of the file ' + name + '.js';
        if (api.namespaces && api.namespaces[name]) {
            const fns = Object.keys(api.namespaces[name]).sort().slice(0, 3);
            return 'the ' + name + ' system (' + fns.map((n) => name + '.' + n)
                .join(', ') + ', ...)';
        }
        if (api.natives[name]) return 'a built-in function';
        if (api.consts[name]) return 'a built-in constant';
        if (api.groups[name]) return 'a built-in group of constants';
        return 'a built-in name';
    }

    reserved(name) {
        const api = this.api;
        return api.natives[name] || api.consts[name] || api.groups[name] ||
            (api.namespaces && api.namespaces[name]) ||
            BUILTINS.indexOf(name) >= 0 || this.byNs.has(name) ||
            name in MISSING_GLOBALS;
    }

    constValue(sym) {
        if (sym.api) {
            if (sym.api.version > this.version) this.version = sym.api.version;
            return sym.api.value;
        }
        if (sym.state === 2) return sym.value;
        if (sym.state === 3) return 0;      /* refused already: one error */
        if (sym.state === 1) fail(sym.node, 'const ' + sym.name + ' refers to itself');
        sym.state = 1;
        const v = this.fold(sym.node.init, new Scope(this, sym.file, null));
        if (v === undefined) {
            sym.state = 3;
            fail(sym.node.init, 'const ' + sym.name + ' needs a value known when ' +
                 'compiling; use var');
        }
        sym.state = 2;
        sym.value = v;
        return v;
    }

    /* The constant value of node, or undefined. Never emits code. */
    fold(node, scope) {
        switch (node.type) {
        case 'Literal':
            if (node.regex) return undefined;
            if (typeof node.value === 'number' || typeof node.value === 'bigint')
                return numLiteral(node);
            if (typeof node.value === 'boolean') return node.value ? 1 : 0;
            return node.value;               /* string or null */
        case 'Identifier': {
            if (node.name === 'undefined') {
                const s = scope.resolve(node.name, node);
                return s && s.kind === 'builtin' ? null : undefined;
            }
            const sym = scope.resolve(node.name, node);
            if (!sym) return undefined;
            if (sym.kind === 'const') return this.constValue(sym);
            if (sym.kind === 'func') return { fn: sym.fn };
            return undefined;
        }
        case 'MemberExpression': {
            if (node.computed || node.object.type !== 'Identifier') return undefined;
            const base = scope.resolve(node.object.name, node.object);
            if (!base) return undefined;
            if (base.kind === 'group') {
                const c = this.api.groups[base.name][node.property.name];
                if (!c) return undefined;
                if (c.version > this.version) this.version = c.version;
                return c.value;
            }
            if (base.kind === 'ns') {
                const sym = base.file.scope.get(node.property.name);
                if (sym && sym.kind === 'const') return this.constValue(sym);
                if (sym && sym.kind === 'func') return { fn: sym.fn };
            }
            return undefined;
        }
        case 'UnaryExpression': {
            if (node.operator === 'typeof' || node.operator === 'delete' ||
                node.operator === 'void')
                return undefined;
            /* -32768.0 is in range though 32768.0 is not. */
            const lit = node.argument;
            if (node.operator === '-' && lit.type === 'Literal' &&
                lit.value === 32768 && /[.]/.test(lit.raw))
                return num.fix(-0x80000000);
            const v = this.fold(node.argument, scope);
            if (v === undefined) return undefined;
            if (node.operator === '!') return truthy(v) ? 0 : 1;
            if (!num.isNum(v)) return undefined;
            if (node.operator === '-') return num.neg(v);
            if (node.operator === '~') return num.bnot(v);
            if (node.operator === '+' && node.argument.type === 'Literal') return v;
            return undefined;
        }
        case 'BinaryExpression': {
            if (!BINARY[node.operator]) return undefined;
            const a = this.fold(node.left, scope);
            if (a === undefined) return undefined;
            const b = this.fold(node.right, scope);
            if (b === undefined) return undefined;
            const op = node.operator;
            if (op === '==' || op === '===') return sameConst(a, b) ? 1 : 0;
            if (op === '!=' || op === '!==') return sameConst(a, b) ? 0 : 1;
            if (op === '+' && (typeof a === 'string' || typeof b === 'string'))
                return toStr(a) + toStr(b);
            if (num.isNum(a) && num.isNum(b)) return num.binary(op, a, b);
            return undefined;
        }
        case 'LogicalExpression': {
            const a = this.fold(node.left, scope);
            if (a === undefined) return undefined;
            if (node.operator === '&&') return truthy(a) ? this.fold(node.right, scope) : a;
            if (node.operator === '||') return truthy(a) ? a : this.fold(node.right, scope);
            return undefined;
        }
        case 'ConditionalExpression': {
            const t = this.fold(node.test, scope);
            if (t === undefined) return undefined;
            return this.fold(truthy(t) ? node.consequent : node.alternate, scope);
        }
        case 'CallExpression': {
            const c = node.callee;
            if (c.type !== 'Identifier' || (c.name !== 'deg' && c.name !== 'rgb')) return undefined;
            const sym = scope.resolve(c.name, c);
            if (!sym || sym.kind !== 'builtin') return undefined;
            if (node.arguments.length !== 1)
                fail(node, c.name + '() takes 1 argument');
            const x = this.number(node.arguments[0], scope);
            if (x === undefined) return undefined;
            /* 0xRRGGBB to the nearest colour of the palette the cart
             * starts in */
            if (c.name === 'rgb') return nearestColour(this.palette || DEFAULT_RGB, x | 0);
            /* Degrees to turns, the unit of sin, cos and atan2. */
            return num.fromReal(x / 360);
        }
        }
        return undefined;
    }

    /* deg() takes a literal, negated or not, or a constant. */
    number(node, scope) {
        if (node.type === 'Literal' && typeof node.value === 'number') return node.value;
        if (node.type === 'Literal') return undefined;
        if (node.type === 'UnaryExpression' && node.operator === '-') {
            const v = this.number(node.argument, scope);
            return v === undefined ? undefined : -v;
        }
        const v = this.fold(node, scope);
        if (num.isFix(v)) return v.raw / 65536;
        return typeof v === 'number' ? v : undefined;
    }

    functionExpression(node, outer, file) {
        const name = node.id ? node.id.name
            : (outer ? outer.name + '.' : '') + 'function@' + node.loc.start.line;
        const fn = new Fn(this, file, name, node, outer);
        fn.compileBody();
        return fn;
    }

    /* Pass 1: every file's top-level names. */
    declare(file) {
        const scope = file.scope;
        const self = this;
        function claim(id, node) {
            const name = id.name;
            if (scope.has(name))
                fail(id, name + ' is declared twice in ' + file.name);
            if (self.reserved(name))
                fail(id, name + ' is ' + self.whatIs(name) + '; choose another ' +
                     'name for a global (a local or parameter may reuse it)');
        }
        for (const s of file.ast.body) {
            this.guard(file, () => {
                switch (s.type) {
                case 'FunctionDeclaration': {
                    claim(s.id, s);
                    const name = (file.ns ? file.ns + '.' : '') + s.id.name;
                    const fn = new Fn(this, file, name, s, null);
                    scope.set(s.id.name, { kind: 'func', fn: fn });
                    return;
                }
                case 'VariableDeclaration':
                    if (s.kind === 'let') fail(s, 'let is not supported; use var');
                    for (const d of s.declarations) {
                        if (d.id.type !== 'Identifier') unsupported(d.id);
                        claim(d.id, d);
                        if (s.kind === 'const') {
                            if (!d.init) fail(d, 'const ' + d.id.name + ' needs a value');
                            scope.set(d.id.name, { kind: 'const', name: d.id.name,
                                                   node: d, file: file, state: 0 });
                        } else {
                            if (this.globals.length >= 65535) fail(d, 'too many globals');
                            const g = { kind: 'global', slot: this.globals.length,
                                        name: (file.ns ? file.ns + '.' : '') + d.id.name,
                                        node: d, file: file, value: null };
                            this.globals.push(g);
                            scope.set(d.id.name, g);
                        }
                    }
                    return;
                case 'EmptyStatement':
                    return;
                case 'ExpressionStatement':
                    if (s.directive) return;
                    break;
                }
                if (UNSUPPORTED[s.type]) unsupported(s);
                fail(s, 'only declarations can be at the top level of a file; ' +
                     'put code that runs once in init()');
            });
        }
    }

    /* Pass 2: constants, then globals' initial values. Those not known
     * when compiling are computed by $start, in file order, before init(). */
    initialise() {
        for (const file of this.files)
            for (const sym of file.scope.values())
                if (sym.kind === 'const')
                    this.guard(file, () => this.constValue(sym));
        for (const g of this.globals) {
            if (!g.node) continue;          /* an image the loader makes */
            const init = g.node.init;
            if (!init) continue;
            this.guard(g.file, () => {
                if (init.type === 'FunctionExpression') {
                    const fn = new Fn(this, g.file, g.name, init, null);
                    fn.compileBody();
                    g.value = { fn: fn };
                    return;
                }
                const top = new Scope(this, g.file, null);
                const v = this.fold(init, top);
                if (v !== undefined) {
                    if (typeof v === 'string') this.string(v);
                    g.value = v;
                    return;
                }
                if (!this.start) {
                    this.start = new Fn(this, g.file, '$start', null, null);
                }
                const st = this.start;
                st.file = g.file;
                st.at(g.node);
                st.value(init);
                st.emit('STG', [g.slot]);
            });
        }
        if (this.start) this.start.emit('RETNULL', []);
    }

    entry(name, required) {
        const sym = this.main.scope.get(name);
        if (sym && sym.kind === 'func') return sym.fn;
        if (sym)
            this.errors.push({ file: 'main.js', line: sym.node.loc.start.line,
                               column: 1, message: name + ' must be a function' });
        else if (required)
            this.errors.push({ file: 'main.js', line: 1, column: 1,
                               message: 'main.js needs function ' + name + '()' });
        return null;
    }
}

/* ---- the chunks --------------------------------------------------------- */

function header(cart, prog) {
    const w = new Writer();
    w.u16(prog.format || FORMAT);
    w.u16(prog.version);
    w.u8(cart.rate);
    w.u8(cart.menu_button === 'game' ? 1 : 0);
    w.u8(prog.level);
    w.u8(cart.suspend === false ? 1 : 0);   /* 1: Quit keeps nothing to resume */
    w.u32(0);                      /* memory required: all of it, for now */
    for (let i = 0; i < 16; i += 2) w.u8(parseInt(cart.id.substr(i, 2), 16));
    return w.done();
}

const META_TAGS = { name: 1, author: 2, version: 3, help: 4 };

function meta(cart) {
    const w = new Writer();
    for (const k in META_TAGS) {
        if (cart[k] === undefined) continue;
        w.u8(META_TAGS[k]);
        w.str(cart[k]);
    }
    return w.done();
}

function code(prog, fns, entries) {
    const w = new Writer();
    w.u16(fns.length);
    w.u16(prog.sites);
    for (const e of entries) w.u16(e ? e.index : 0xffff);
    let offset = 0;
    for (const f of fns) {
        w.u8(f.arity);
        w.u8(f.nlocals);
        w.u16(f.maxDepth);
        w.u32(offset);
        w.u32(f.code.length);
        offset += f.code.length;
    }
    for (const f of fns) w.bytes(f.code);
    return w.done();
}

function constants(prog) {
    const w = new Writer();
    w.u16(prog.strings.length);
    for (const s of prog.strings) w.str(s);
    w.u16(prog.shapes.length);
    for (const keys of prog.shapes) {
        w.u8(keys.length);
        for (const k of keys) w.u16(k);
    }
    return w.done();
}

function globals(prog) {
    const w = new Writer();
    w.u16(prog.globals.length);
    for (const g of prog.globals) {
        const v = g.value;
        if (v === null) w.u8(0);
        else if (typeof v === 'number') { w.u8(1); w.u32(v); }
        else if (typeof v === 'string') { w.u8(2); w.u16(prog.string(v)); }
        else if (num.isFix(v)) { w.u8(4); w.u32(v.raw); }
        else if (v.img !== undefined) { w.u8(5); w.u16(v.img); }
        else { w.u8(3); w.u16(v.fn.index); }
    }
    return w.done();
}

function lines(prog, fns) {
    const w = new Writer();
    w.u16(prog.files.length);
    for (const f of prog.files) w.str(f.name);
    w.u16(prog.globals.length);
    for (const g of prog.globals) w.str(g.name);
    for (const f of fns) {
        w.str(f.name);
        w.u8(prog.files.indexOf(f.file));
        w.u16(f.lines.length);
        for (const [pc, line] of f.lines) { w.u16(pc); w.u16(line); }
    }
    return w.done();
}

/* SRC_: every source file, so the cart is its own project. */
function sources(files, deflate) {
    const w = new Writer();
    const names = Object.keys(files).sort();
    w.u16(names.length);
    for (const n of names) {
        w.str(n);
        const b = utf8(files[n]);
        w.u32(b.length);
        w.bytes(b);
    }
    return deflate(w.done());
}

/* BORD: the high-score boards, each its key (a hash of its name, which the
 * engine matches a saved table by), version, whether low wins, its format
 * and its title */
const FORMATS = ['number', 'time', 'ticks'];

function boardKey(name) {
    let h = 0x811c9dc5;
    for (const b of utf8(name)) h = Math.imul(h ^ b, 0x01000193) >>> 0;
    return h;
}

function boards(list) {
    const w = new Writer();
    w.u8(list.length);
    for (const b of list) {
        w.u32(boardKey(b.name));
        w.u16(b.version);
        w.u8(b.wins === 'low' ? 1 : 0);
        w.u8(FORMATS.indexOf(b.format));
        w.str(b.title);
    }
    return w.done();
}

/* OPTN: the options, each its key (a hash of its name, which the engine
 * matches the owner's saved value by), type, default, range, step, title,
 * choices and help */
const OPT_TYPES = ['choice', 'onoff', 'number'];

function options(list) {
    const w = new Writer();
    w.u8(list.length);
    for (const o of list) {
        w.u32(boardKey(o.name));
        w.u8(OPT_TYPES.indexOf(o.type));
        w.u32(o.default >>> 0);
        w.u32(o.min >>> 0);
        w.u32(o.max >>> 0);
        w.u32(o.step);
        w.str(o.title);
        w.u8(o.choices.length);
        for (const c of o.choices) w.str(c);
        w.str(o.help);
    }
    return w.done();
}

/* ACHV: the achievements, each its key (a hash of its name, which the
 * engine matches the owner's earned ones by), title and text */
function achievements(list) {
    const w = new Writer();
    w.u8(list.length);
    for (const a of list) {
        w.u32(boardKey(a.name));
        w.str(a.title);
        w.str(a.text);
    }
    return w.done();
}

/* ---- the cart's settings ----------------------------------------------- */

const CART_KEYS = ['id', 'name', 'author', 'version', 'help', 'rate',
                   'menu_button', 'music', 'suspend', 'boards', 'options',
                   'achievements', 'flags', 'lazy_images'];
const BOARD_KEYS = ['name', 'title', 'wins', 'format', 'version'];

const OPTION_KEYS = ['name', 'title', 'type', 'choices', 'min', 'max', 'step',
                     'default', 'help'];

function shortText(s) {
    return typeof s === 'string' && s !== '' && utf8(s).length <= 23;
}

/* A name a cart's options, achievements, flags and boards may take */
const NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;

/* Whether x is a plain record: an object, not null or an array */
function isRecord(x) {
    return x !== null && typeof x === 'object' && !Array.isArray(x);
}

/* cart.options checked, each made whole: the defaults filled in, and an
 * on/off or a choice given the range of its values */
function checkOptions(list, bad) {
    if (list === undefined) return [];
    if (!Array.isArray(list)) { bad('options must be a list'); return []; }
    if (list.length > 8) bad('a cart has at most 8 options');
    const seen = new Set();
    const whole = (v) => Number.isInteger(v) && v >= -0x80000000 && v <= 0x7fffffff;
    return list.slice(0, 8).map((o, i) => {
        const at = 'option ' + (i + 1) + ': ';
        if (!isRecord(o)) {
            bad(at + 'must be a record'); return null;
        }
        for (const k in o)
            if (OPTION_KEYS.indexOf(k) < 0) bad(at + 'unknown setting ' + k);
        if (typeof o.name !== 'string' || !NAME.test(o.name))
            bad(at + 'name must be usable in code as OPT.<name>');
        else if (seen.has(o.name)) bad(at + 'a second option called ' + o.name);
        seen.add(o.name);
        const c = Object.assign({ title: o.name, help: '' }, o);
        if (!shortText(c.title)) bad(at + 'title must be text of 1 to 23 bytes');
        if (typeof c.help !== 'string' || utf8(c.help).length > 300)
            bad(at + 'help must be text of at most 300 bytes');
        if (OPT_TYPES.indexOf(c.type) < 0) {
            bad(at + 'type must be one of ' + OPT_TYPES.join(', '));
            return null;
        }
        if (c.type !== 'choice' && o.choices !== undefined)
            bad(at + 'only a choice has choices');
        if (c.type !== 'number')
            for (const k of ['min', 'max', 'step'])
                if (o[k] !== undefined) bad(at + 'only a number has ' + k);
        if (c.type === 'choice') {
            if (!Array.isArray(c.choices) || c.choices.length < 2 ||
                c.choices.length > 8 || !c.choices.every(shortText)) {
                bad(at + 'choices must be 2 to 8 texts of 1 to 23 bytes');
                c.choices = [];
            }
            c.min = 0; c.max = Math.max(c.choices.length - 1, 0); c.step = 1;
            if (c.default === undefined) c.default = 0;
        } else if (c.type === 'onoff') {
            c.choices = []; c.min = 0; c.max = 1; c.step = 1;
            if (c.default === undefined || c.default === false) c.default = 0;
            if (c.default === true) c.default = 1;
        } else {
            c.choices = [];
            if (c.step === undefined) c.step = 1;
            if (!whole(c.min) || !whole(c.max) || c.min >= c.max)
                bad(at + 'min and max must be whole numbers, min below max');
            if (!Number.isInteger(c.step) || c.step < 1 || c.step > 0x7fffffff)
                bad(at + 'step must be a whole number of at least 1');
            if (c.default === undefined) c.default = c.min;
        }
        if (!Number.isInteger(c.default) || c.default < c.min ||
            c.default > c.max)
            bad(at + 'default must be ' + (c.type === 'choice'
                ? 'the index of one of its choices'
                : c.type === 'onoff' ? 'true or false'
                : 'a whole number from min to max'));
        return c;
    }).filter(Boolean);
}

/* cart.achievements checked: up to 32, each a name, a title and a text */
function checkAchievements(list, bad) {
    if (list === undefined) return [];
    if (!Array.isArray(list)) { bad('achievements must be a list'); return []; }
    if (list.length > 32) bad('a cart has at most 32 achievements');
    const seen = new Set();
    return list.slice(0, 32).map((a, i) => {
        const at = 'achievement ' + (i + 1) + ': ';
        if (!isRecord(a)) {
            bad(at + 'must be a record'); return null;
        }
        for (const k in a)
            if (['name', 'title', 'text'].indexOf(k) < 0)
                bad(at + 'unknown setting ' + k);
        if (typeof a.name !== 'string' || !NAME.test(a.name))
            bad(at + 'name must be usable in code as ACH.<name>');
        else if (seen.has(a.name)) bad(at + 'a second achievement called ' + a.name);
        seen.add(a.name);
        const c = Object.assign({ title: a.name, text: '' }, a);
        if (typeof c.title !== 'string' || c.title === '' || utf8(c.title).length > 31)
            bad(at + 'title must be text of 1 to 31 bytes');
        if (typeof c.text !== 'string' || utf8(c.text).length > 95)
            bad(at + 'text must be at most 95 bytes');
        return c;
    }).filter(Boolean);
}

/* cart.flags checked: the sprite flags' names by number, up to eight,
 * null for one left unnamed */
function checkFlags(list, bad) {
    if (list === undefined) return [];
    if (!Array.isArray(list)) { bad('flags must be a list of names, flag 0 first'); return []; }
    if (list.length > 8) bad('a sprite has 8 flags, so at most 8 names');
    const seen = new Set();
    return list.slice(0, 8).map((n, i) => {
        if (n === null || n === '') return null;
        if (typeof n !== 'string' || !NAME.test(n)) {
            bad('flag ' + i + ': a name must be usable in code as FLAG.<name>');
            return null;
        }
        if (seen.has(n)) bad('flag ' + i + ': a second flag called ' + n);
        seen.add(n);
        return n;
    });
}

/* cart.boards checked, with each one's defaults filled in */
function checkBoards(list, bad) {
    if (list === undefined) return [];
    if (!Array.isArray(list)) { bad('boards must be a list'); return []; }
    if (list.length > 8) bad('a cart has at most 8 boards');
    const seen = new Set();
    return list.slice(0, 8).map((b, i) => {
        const at = 'board ' + (i + 1) + ': ';
        if (!isRecord(b)) {
            bad(at + 'must be a record'); return null;
        }
        for (const k in b)
            if (BOARD_KEYS.indexOf(k) < 0) bad(at + 'unknown setting ' + k);
        if (typeof b.name !== 'string' || !NAME.test(b.name))
            bad(at + 'name must be usable in code as BOARD.<name>');
        else if (seen.has(b.name)) bad(at + 'a second board called ' + b.name);
        seen.add(b.name);
        const c = Object.assign({ title: b.name, wins: 'high', format: 'number',
                                  version: 1 }, b);
        if (typeof c.title !== 'string' || c.title === '' ||
            utf8(c.title).length > 23)
            bad(at + 'title must be text of 1 to 23 bytes');
        if (c.wins !== 'high' && c.wins !== 'low')
            bad(at + 'wins must be "high" or "low"');
        if (FORMATS.indexOf(c.format) < 0)
            bad(at + 'format must be one of ' + FORMATS.join(', '));
        if (!Number.isInteger(c.version) || c.version < 1 || c.version > 65535)
            bad(at + 'version must be a whole number from 1 to 65535');
        return c;
    }).filter(Boolean);
}

function checkCart(cart, errors) {
    function bad(message) {
        errors.push({ file: 'cart.json', line: 1, column: 1, message: message });
    }
    for (const k in cart)
        if (CART_KEYS.indexOf(k) < 0) bad('unknown setting ' + k);
    if (typeof cart.id !== 'string' || !/^[0-9a-f]{16}$/.test(cart.id))
        bad('id must be 16 lowercase hex digits, made once when the cart is created');
    if (typeof cart.name !== 'string' || cart.name === '') bad('name is required');
    for (const k of ['author', 'version', 'help'])
        if (cart[k] !== undefined && typeof cart[k] !== 'string')
            bad(k + ' must be a string');
    const c = Object.assign({ rate: 33, menu_button: 'pause', music: 'listen' }, cart);
    if (RATES.indexOf(c.rate) < 0) bad('rate must be one of ' + RATES.join(', '));
    if (c.menu_button !== 'pause' && c.menu_button !== 'game')
        bad('menu_button must be "pause" or "game"');
    if (MUSIC.indexOf(c.music) < 0) bad('music must be one of ' + MUSIC.join(', '));
    if (c.suspend !== undefined && typeof c.suspend !== 'boolean')
        bad('suspend must be true or false');
    c.lazy_images = cart.lazy_images === undefined ? [] : cart.lazy_images;
    if (!Array.isArray(c.lazy_images) ||
        c.lazy_images.some((n) => typeof n !== 'string') ||
        new Set(c.lazy_images).size !== c.lazy_images.length) {
        bad('lazy_images must be a list of image names, each once');
        c.lazy_images = [];
    }
    c.boards = checkBoards(cart.boards, bad);
    c.options = checkOptions(cart.options, bad);
    c.achievements = checkAchievements(cart.achievements, bad);
    c.flags = checkFlags(cart.flags, bad);
    return c;
}

/* ---- compile() ---------------------------------------------------------- *
 * input: { files: { 'main.js': text, 'cart.json': text, ... },
 *          isa, api: parsed .def tables, deflate: raw deflate }
 * returns { cart: Uint8Array or null, errors: [...], stats } */

/* The index of the colour in palette (0xRRGGBB each) nearest to rgb: the
 * first of any equally near */
function nearestColour(palette, rgb) {
    let best = 0, bestD = Infinity;
    palette.forEach((c, i) => {
        const dr = (c >> 16) - (rgb >> 16 & 255), dg = (c >> 8 & 255) - (rgb >> 8 & 255), db = (c & 255) - (rgb & 255);
        const d = dr * dr + dg * dg + db * db;
        if (d < bestD) { best = i; bestD = d; }
    });
    return best;
}

export function compile(input) {
    const prog = new Program(input.isa, input.api);
    const errors = prog.errors;
    let cart = {};
    if (input.files['cart.json'] === undefined)
        errors.push({ file: 'cart.json', line: 1, column: 1, message: 'missing' });
    else {
        try { cart = JSON.parse(input.files['cart.json']); }
        catch (e) {
            errors.push({ file: 'cart.json', line: 1, column: 1, message: e.message });
        }
    }
    cart = checkCart(cart, errors);
    prog.level = MUSIC.indexOf(cart.music);

    const names = Object.keys(input.files).filter((n) => /\.js$/.test(n)).sort();
    if (names.indexOf('main.js') < 0)
        errors.push({ file: 'main.js', line: 1, column: 1, message: 'missing' });
    for (const name of names) {
        const ns = name === 'main.js' ? null : name.slice(0, -3);
        const file = { name: name, ns: ns, scope: new Map(), ast: null };
        if (ns !== null && !/^[A-Za-z_$][\w$]*$/.test(ns)) {
            errors.push({ file: name, line: 1, column: 1, message:
                          'a file name must be usable as a name in code: ' +
                          'letters, digits and _' });
            continue;
        }
        try {
            file.ast = acorn.parse(input.files[name], {
                ecmaVersion: 'latest', sourceType: 'script', locations: true
            });
        } catch (e) {
            errors.push({ file: name, line: e.loc ? e.loc.line : 1,
                          column: e.loc ? e.loc.column + 1 : 1,
                          message: e.message.replace(/ \(\d+:\d+\)$/, '')
                              .replace(/^Unsyntactic (break|continue)$/,
                                       '$1 outside a loop') });
            continue;
        }
        if (ns === null) prog.main = file;
        else prog.byNs.set(ns, file);
        prog.files.push(file);
    }
    /* The cart's assets: MAP, PAL and OBJ as constant groups, and IMG as a
     * namespace of globals that the loader sets to the images. */
    const built = buildAssets(input.assets, input.inflate,
                              (str) => prog.string(str), errors,
                              cart.lazy_images);
    /* format 3 keeps maps' cells and lazy images apart, read as they are
     * used */
    prog.format = built.format3 ? 3 : FORMAT;
    prog.palette = built.palette;
    const board = {}, option = {}, ach = {};
    cart.boards.forEach((b, i) => { board[b.name] = { value: i, version: 1 }; });
    cart.options.forEach((o, i) => { option[o.name] = { value: i, version: 1 }; });
    cart.achievements.forEach((a, i) => { ach[a.name] = { value: i, version: 1 }; });
    /* FLAG.<name> is the flag as a set of one: 1 << its number, so names
     * combine with | for actors.move() and map(), and fget(n) & FLAG.x
     * tests one */
    const flag = {};
    cart.flags.forEach((n, i) => { if (n) flag[n] = { value: 1 << i, version: 1 }; });
    prog.api = Object.assign({}, prog.api, {
        groups: Object.assign({}, prog.api.groups, built.groups,
                              { BOARD: board, OPT: option, ACH: ach, FLAG: flag,
                                FONT: Object.assign({}, prog.api.groups.FONT,
                                                    built.groups.FONT) })
    });
    if (prog.byNs.has('IMG'))
        errors.push({ file: 'IMG.js', line: 1, column: 1, message:
                      'IMG names the cart\'s images; call the file something else' });
    const images = { name: 'images', ns: 'IMG', scope: new Map(), ast: null };
    built.images.forEach((name, i) => {
        const g = { kind: 'global', slot: prog.globals.length, name: 'IMG.' + name,
                    node: null, file: images, value: { img: i } };
        prog.globals.push(g);
        images.scope.set(name, g);
    });
    prog.byNs.set('IMG', images);

    /* Namespaces first, main last: the order $start initialises them in. */
    prog.files.sort((a, b) => (a.ns === null) - (b.ns === null) ||
                    (a.name < b.name ? -1 : 1));

    for (const f of prog.files) prog.declare(f);
    if (errors.length) return { cart: null, errors: errors };
    prog.initialise();
    for (const fn of prog.fns.slice()) {
        if (fn.node && fn.node.type === 'FunctionDeclaration')
            prog.guard(fn.file, () => fn.compileBody());
    }
    const entries = prog.main ? [prog.start, prog.entry('init', true),
                                 prog.entry('update', true),
                                 prog.entry('draw', false),
                                 prog.entry('ending', false)] : [];
    if (errors.length) return { cart: null, errors: errors };

    /* Keep what the entries and the globals can reach, numbered in order. */
    const keep = new Set();
    const work = entries.filter(Boolean);
    for (const g of prog.globals) if (isFn(g.value)) work.push(g.value.fn);
    while (work.length) {
        const f = work.pop();
        if (keep.has(f)) continue;
        keep.add(f);
        for (const r of f.refs) work.push(r);
    }
    const fns = prog.fns.filter((f) => keep.has(f));
    if (fns.length > 65535)
        return { cart: null, errors: [{ file: 'main.js', line: 1, column: 1,
                                        message: 'too many functions' }] };
    /* the engine's 4096 values of stack, less the 8 it keeps above */
    for (const f of fns)
        if (f.nlocals + f.maxDepth + 8 > 4096) {
            const at = f.node && f.node.loc ? f.node.loc.start
                                            : { line: 1, column: 0 };
            return { cart: null, errors: [{
                file: f.file ? f.file.name : 'main.js', line: at.line,
                column: at.column + 1,
                message: (f.name || 'the program') + ' needs more stack ' +
                         'than a game has; split it' }] };
        }
    fns.forEach((f, i) => { f.index = i; });
    for (const f of fns) {
        for (const x of f.fixups) {
            f.code[x.at] = x.fn.index & 0xff;
            f.code[x.at + 1] = x.fn.index >> 8;
        }
    }

    const srcFiles = {};
    for (const n in input.files)
        if (/\.js$/.test(n) || n === 'cart.json') srcFiles[n] = input.files[n];
    /* every text asset as written; a picture is named with no contents,
     * pbgc unpack making it again from its chunk */
    const a = input.assets || {};
    const kinds = [['palettes', 'palettes/', '.gpl'], ['maps', 'maps/', '.json'],
                   ['sounds', 'sounds/', '.json'], ['data', 'data/', '.json'],
                   ['texts', 'data/', '.txt'], ['fonts', 'fonts/', '.bdf'],
                   ['editors', 'editors/', '.html']];
    for (const [kind, dir, ext] of kinds)
        for (const n in a[kind] || {}) srcFiles[dir + n + ext] = a[kind][n];
    if (a.flags !== undefined) srcFiles['flags.json'] = a.flags;
    if (a.objects !== undefined) srcFiles['objects.json'] = a.objects;
    if (a.sheet) srcFiles['sheet.png'] = '';
    if (built.logo) srcFiles['logo.png'] = '';
    for (const n of built.images) srcFiles['images/' + n + '.png'] = '';

    const bytes = writeCart([
        { tag: 'PBG1', data: header(cart, prog) },
        { tag: 'META', data: meta(cart) },
        { tag: 'CODE', data: code(prog, fns, entries) },
        { tag: 'CNST', data: constants(prog) },
        { tag: 'GLOB', data: globals(prog) },
        { tag: 'LINE', data: lines(prog, fns) },
        ...built.chunks,
        ...(cart.boards.length ? [{ tag: 'BORD', data: boards(cart.boards) }] : []),
        ...(cart.options.length ? [{ tag: 'OPTN', data: options(cart.options) }] : []),
        ...(cart.achievements.length
            ? [{ tag: 'ACHV', data: achievements(cart.achievements) }] : []),
        { tag: 'SRC_', data: sources(srcFiles, input.deflate) }
    ]);
    let codeBytes = 0;
    for (const f of fns) codeBytes += f.code.length;
    return {
        cart: bytes, errors: [],
        groups: Object.assign({}, built.groups, { BOARD: board, OPT: option, ACH: ach, FLAG: flag,
            IMG: Object.fromEntries(built.images.map((n) => [n, {}])) }),
        /* each global's name, by its slot: the memory view's labels */
        globals: prog.globals.map((g) => g.name),
        stats: { functions: fns.length, dropped: prog.fns.length - fns.length,
                 code: codeBytes, globals: prog.globals.length,
                 strings: prog.strings.length, size: bytes.length,
                 api: prog.version }
    };
}
