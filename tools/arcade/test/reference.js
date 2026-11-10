/* The reference for differential tests: runs a cart's main.js in Node with
 * the language's semantics put back, by rewriting operators into calls:
 * integers wrap at 32 bits and divide by truncating, fractions are 16.16
 * (numbers.js), equality is strict with undefined as null, booleans are 1
 * and 0, and a condition is tested with the language's truthiness. What
 * it prints is what the VM must print.
 *
 * Not modelled, so tests avoid them: a fraction as a switch case or an
 * indexOf() argument, and a sort comparator returning a fraction. */

import * as acorn from 'acorn';
import vm from 'node:vm';
import * as num from '../src/numbers.js';

const WRAPPED = { '+': '$add', '-': '$sub', '*': '$mul', '/': '$div',
                  '%': '$mod', '>>>': '$ushr', '==': '$eq', '===': '$eq',
                  '!=': '$ne', '!==': '$ne', '<': '$lt', '<=': '$le',
                  '>': '$gt', '>=': '$ge' };

function rewrite(src) {
    const ast = acorn.parse(src, { ecmaVersion: 'latest', sourceType: 'script' });

    function children(node) {
        const out = [];
        for (const k in node) {
            const c = node[k];
            if (Array.isArray(c)) c.forEach((x) => x && typeof x.type === 'string' && out.push(x));
            else if (c && typeof c.type === 'string') out.push(c);
        }
        return out.sort((a, b) => a.start - b.start);
    }

    /* node's source with each child replaced by its rewrite, or by
     * swap(child) when that returns a string. */
    function splice(node, swap) {
        let at = node.start, s = '';
        for (const c of children(node)) {
            const r = swap && swap(c);
            s += src.slice(at, c.start) + (typeof r === 'string' ? r : gen(c));
            at = c.end;
        }
        return s + src.slice(at, node.end);
    }

    const test = (n) => '$t(' + gen(n) + ')';

    function gen(n) {
        switch (n.type) {
        case 'Literal':
            if (typeof n.value === 'boolean') return n.value ? '1' : '0';
            if (typeof n.value === 'number' && (/[.]/.test(n.raw) || !Number.isInteger(n.value)))
                return '$fx(' + num.fromReal(n.value).raw + ')';
            return splice(n);
        case 'BinaryExpression':
            if (WRAPPED[n.operator])
                return WRAPPED[n.operator] + '(' + gen(n.left) + ', ' + gen(n.right) + ')';
            return '(' + splice(n) + ')';
        case 'LogicalExpression':
            return (n.operator === '&&' ? '$and(' : '$or(') + gen(n.left) +
                ', () => (' + gen(n.right) + '))';
        case 'UnaryExpression':
            if (n.operator === '-' && n.argument.type === 'Literal' &&
                n.argument.value === 32768 && /[.]/.test(n.argument.raw))
                return '$fx(-2147483648)';
            if (n.operator === '-') return '$neg(' + gen(n.argument) + ')';
            if (n.operator === '!') return '$not(' + gen(n.argument) + ')';
            if (n.operator === '~') return '$bnot(' + gen(n.argument) + ')';
            if (n.operator === 'typeof') return '$typeof(' + gen(n.argument) + ')';
            return '(' + splice(n) + ')';
        case 'AssignmentExpression': {
            const op = n.operator.slice(0, -1);
            if (n.operator !== '=' && WRAPPED[op]) {
                const t = gen(n.left);
                return '(' + t + ' = ' + WRAPPED[op] + '(' + t + ', ' + gen(n.right) + '))';
            }
            return '(' + splice(n) + ')';
        }
        case 'UpdateExpression': {
            const t = gen(n.argument), d = n.operator === '++' ? 1 : -1;
            return '$upd(() => ' + t + ', (v) => ' + t + ' = v, ' + d + ', ' +
                (n.prefix ? 1 : 0) + ')';
        }
        case 'MemberExpression':
            /* the VM stops at a property or index of a number or null, where
             * JavaScript would give undefined */
            if (n.computed)
                return '$o(' + gen(n.object) + ')[$k(' + gen(n.property) + ')]';
            return '$o(' + gen(n.object) + ').' + n.property.name;
        case 'IfStatement': case 'WhileStatement': case 'DoWhileStatement':
        case 'ForStatement': case 'ConditionalExpression':
            return splice(n, (c) => (c === n.test ? test(c) : undefined));
        case 'SequenceExpression':
            return '(' + splice(n) + ')';
        case 'CallExpression':
            if (n.callee.type === 'MemberExpression' && !n.callee.computed &&
                n.callee.object.type === 'Identifier' && n.callee.object.name === 'Math')
                return '$Math.' + n.callee.property.name + '(' +
                    n.arguments.map(gen).join(', ') + ')';
            return splice(n);
        }
        return splice(n);
    }
    return gen(ast);
}

function str(v, depth) {
    /* JS's NaN, from charCodeAt past the end, is the language's null. */
    if (v === null || v === undefined || Number.isNaN(v)) return 'null';
    if (typeof v === 'number' || num.isFix(v)) return num.text(v);
    if (typeof v === 'string') return v;
    if (typeof v === 'function') return 'function';
    if (Array.isArray(v) || ArrayBuffer.isView(v)) {
        if ((depth || 0) > 4) return '...';
        return Array.from(v, (e) => e === null || e === undefined ? '' : str(e, (depth || 0) + 1)).join(',');
    }
    return '[object Object]';
}

function need(v, what) {
    if (!num.isNum(v)) throw new Error(what + ' needs numbers');
    return v;
}

function arith(op) {
    return (a, b) => {
        const r = num.binary(op, need(a, op), need(b, op));
        if (r === undefined) throw new Error('division by zero');
        return r;
    };
}

function compare(op) {
    return (a, b) => {
        if (num.isNum(a) && num.isNum(b)) return num.binary(op, a, b);
        if (typeof a === 'string' && typeof b === 'string')
            return (op === '<' ? a < b : op === '<=' ? a <= b : op === '>' ? a > b : a >= b) ? 1 : 0;
        throw new Error(op + ' needs two numbers or two strings');
    };
}

function isqrt(v) {
    if (v <= 0n) return 0n;
    let x = BigInt(Math.floor(Math.sqrt(Number(v))));
    while (x * x > v) x--;
    while ((x + 1n) * (x + 1n) <= v) x++;
    return x;
}

function makeContext(out) {
    const n = (v) => (v === undefined || Number.isNaN(v) ? null : v);
    const eq = (a, b) => {
        a = n(a); b = n(b);
        if (num.isNum(a) && num.isNum(b)) return num.equal(a, b);
        return a === b;
    };
    return {
        $fx: (raw) => num.fix(raw),
        $add: (a, b) => (typeof a === 'string' || typeof b === 'string')
            ? str(a) + str(b) : arith('+')(a, b),
        $sub: arith('-'), $mul: arith('*'), $div: arith('/'), $mod: arith('%'),
        $ushr: (a, b) => (a >>> b) | 0,
        $eq: (a, b) => (eq(a, b) ? 1 : 0),
        $ne: (a, b) => (eq(a, b) ? 0 : 1),
        $lt: compare('<'), $le: compare('<='), $gt: compare('>'), $ge: compare('>='),
        $neg: (a) => num.neg(need(a, '-')),
        $bnot: (a) => num.bnot(need(a, '~')),
        $not: (a) => (num.truthy(a) ? 0 : 1),
        $t: (a) => num.truthy(a),
        $and: (a, b) => (num.truthy(a) ? b() : a),
        $or: (a, b) => (num.truthy(a) ? a : b()),
        $k: (i) => (num.isFix(i) ? num.floor(i) : i),
        $o: (v) => {
            if (v === null || v === undefined || num.isNum(v))
                throw new Error('a property or index of a ' + (num.isNum(v) ? 'number' : 'null'));
            return v;
        },
        $typeof: (v) => (num.isNum(v) ? 'number' : typeof v === 'string' ? 'string'
                         : typeof v === 'function' ? 'function' : 'object'),
        $upd: (get, set, d, prefix) => {
            const old = get();
            const v = typeof old === 'string' ? old + d : arith('+')(old, d);
            set(v);
            return prefix ? v : old;
        },
        $Math: {
            abs: (x) => (num.isFix(x) ? num.fix(Math.abs(x.raw)) : Math.abs(x) | 0),
            min: (...a) => a.reduce((x, y) => (num.binary('<', y, x) ? y : x)),
            max: (...a) => a.reduce((x, y) => (num.binary('>', y, x) ? y : x)),
            sign: (x) => (num.isFix(x) ? Math.sign(x.raw) : Math.sign(x)),
            floor: num.floor, ceil: num.ceil, round: num.round, trunc: num.trunc,
            sqrt: (x) => (num.isFix(x) ? num.fix(Number(isqrt(BigInt(x.raw) << 16n)))
                          : Number(isqrt(BigInt(x)))),
            pow: (a, b) => {
                const r = num.pow(a, b);
                if (r === undefined) throw new Error('Math.pow needs a whole exponent');
                return r;
            }
        },
        deg: (v) => num.fromReal((num.isFix(v) ? v.raw / 65536 : v) / 360),
        printh: (v) => out.push(str(v)),
        parseInt: (s, r) => {
            if (num.isNum(s) && r === undefined) return num.trunc(s);
            const v = parseInt(s, r);
            return Number.isNaN(v) ? null : v | 0;
        },
        String: { fromCharCode: (c) => String.fromCharCode(num.trunc(c) & 0xff) },
        Int32Array: Int32Array, Uint8Array: Uint8Array,
        undefined: undefined
    };
}

/* Runs src's init() then `ticks` update()s; returns { out, error }. */
export function runReference(src, ticks) {
    const out = [];
    const ctx = makeContext(out);
    vm.createContext(ctx);
    try {
        vm.runInContext(rewrite(src) +
            '\ninit();\nfor (var $t0 = 0; $t0 < ' + (ticks || 0) + '; $t0++) update();',
            ctx, { timeout: 10000 });
        return { out: out, error: null };
    } catch (e) {
        return { out: out, error: e.message };
    }
}
