/* The language's numbers, as the VM computes them: the compiler folds
 * constants with these, and the test reference runs carts with them. The
 * engine's vm.c implements the same rules separately, so the differential
 * tests compare two implementations.
 *
 * An integer is a JS number holding an int32. A fraction is a Fix: 16.16
 * fixed point, its raw int32 in .raw. A literal written with a point is a
 * fraction. Integer with integer is integer arithmetic, wrapping at 32
 * bits; anything with a fraction is fixed-point, and wraps the same way.
 * Bitwise operators truncate a fraction to an integer first, as JS's do. */

export class Fix {
    constructor(raw) { this.raw = raw | 0; }
    toString() { return text(this); }
    /* For JS's own conversions in the reference: typed-array stores and
     * bitwise operators, both of which truncate. */
    valueOf() { return trunc(this); }
}

export function fix(raw) { return new Fix(raw); }
export function isFix(v) { return v instanceof Fix; }
export function isNum(v) { return typeof v === 'number' || v instanceof Fix; }

const B16 = 16n;
const MASK32 = (1n << 32n) - 1n;

function wrap(big) {
    let x = big & MASK32;
    if (x >= 1n << 31n) x -= 1n << 32n;
    return Number(x);
}

/* The 64-bit scaled value: an integer n is n * 65536. */
function scaled(v) {
    return v instanceof Fix ? BigInt(v.raw) : BigInt(v) << B16;
}

/* Literal or folded fraction from a JS number; undefined if out of range. */
export function fromReal(x) {
    const raw = Math.round(x * 65536);
    if (raw > 0x7fffffff || raw < -0x80000000) return undefined;
    return fix(raw);
}

export function trunc(v) {
    if (!(v instanceof Fix)) return v;
    return Math.trunc(v.raw / 65536);
}

export function floor(v) {
    return v instanceof Fix ? v.raw >> 16 : v;
}

export function ceil(v) {
    return v instanceof Fix ? wrap((BigInt(v.raw) + 65535n) >> B16) : v;
}

export function round(v) {
    return v instanceof Fix ? wrap((BigInt(v.raw) + 32768n) >> B16) : v;
}

export function truthy(v) {
    if (v instanceof Fix) return v.raw !== 0;
    return !(v === 0 || v === null || v === undefined || v === '');
}

/* Four decimal places, rounded, trailing zeros dropped. */
export function text(v) {
    if (!(v instanceof Fix)) return String(v);
    const neg = v.raw < 0;
    let m = BigInt(v.raw);
    if (neg) m = -m;
    let ip = m >> B16;
    let d = ((m & 0xffffn) * 10000n + 32768n) >> B16;
    if (d === 10000n) { ip += 1n; d = 0n; }
    let ds = d.toString().padStart(4, '0').replace(/0+$/, '');
    const body = ip.toString() + (ds ? '.' + ds : '');
    return (neg && (ip !== 0n || d !== 0n) ? '-' : '') + body;
}

export function equal(a, b) {
    return scaled(a) === scaled(b);
}

/* a op b for two numbers; undefined for division by zero. */
export function binary(op, a, b) {
    if (typeof a === 'number' && typeof b === 'number') {
        switch (op) {
        case '+': return (a + b) | 0;
        case '-': return (a - b) | 0;
        case '*': return Math.imul(a, b);
        case '/': return b === 0 ? undefined : b === -1 ? (-a) | 0 : (a / b) | 0;
        case '%': return b === 0 ? undefined : b === -1 ? 0 : (a % b) | 0;
        }
    }
    switch (op) {
    case '&': return trunc(a) & trunc(b);
    case '|': return trunc(a) | trunc(b);
    case '^': return trunc(a) ^ trunc(b);
    case '<<': return trunc(a) << trunc(b);
    case '>>': return trunc(a) >> trunc(b);
    case '>>>': return (trunc(a) >>> trunc(b)) | 0;
    case '<': return scaled(a) < scaled(b) ? 1 : 0;
    case '<=': return scaled(a) <= scaled(b) ? 1 : 0;
    case '>': return scaled(a) > scaled(b) ? 1 : 0;
    case '>=': return scaled(a) >= scaled(b) ? 1 : 0;
    }
    const x = scaled(a), y = scaled(b);
    switch (op) {
    case '+': return fix(wrap(x + y));
    case '-': return fix(wrap(x - y));
    case '*':
        /* An integer times a fraction is exact; two fractions lose the
         * low 16 bits of the product, rounding down. */
        if (!(a instanceof Fix)) return fix(wrap(BigInt(a) * y));
        if (!(b instanceof Fix)) return fix(wrap(x * BigInt(b)));
        return fix(wrap((x * y) >> B16));
    case '/': {
        if (y === 0n) return undefined;
        const n = x << B16;
        return fix(wrap(n / y));          /* BigInt division truncates */
    }
    case '%':
        if (y === 0n) return undefined;
        return fix(wrap(x % y));
    }
    return undefined;
}

export function neg(a) {
    return a instanceof Fix ? fix(wrap(-BigInt(a.raw))) : (-a) | 0;
}

export function bnot(a) { return ~trunc(a); }

/* Math.pow: a whole exponent; the same squaring as the VM. */
export function pow(a, e) {
    if (e instanceof Fix) {
        if (e.raw & 0xffff) return undefined;
        e = e.raw >> 16;
    }
    if (!(a instanceof Fix)) {
        if (e < 0) return a === 1 ? 1 : a === -1 ? (e & 1 ? -1 : 1) : 0;
        let r = 1, x = a;
        for (; e; e >>>= 1, x = Math.imul(x, x)) if (e & 1) r = Math.imul(r, x);
        return r;
    }
    let n = e < 0 ? -e : e, r = fix(65536), x = a;
    for (; n; n >>>= 1, x = binary('*', x, x)) if (n & 1) r = binary('*', r, x);
    return e < 0 ? binary('/', fix(65536), r) : r;
}
