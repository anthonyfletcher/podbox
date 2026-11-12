/* Reading a compiled cart back: its tables, a disassembly, and the
 * verifier. The verifier is the reference for the engine's verify.c --
 * the same checks, in the same terms -- and the test suite runs every
 * cart the compiler makes through it. */

import { readCart } from './cart.js';

class Reader {
    constructor(bytes) { this.b = bytes; this.at = 0; }
    need(n) { if (this.at + n > this.b.length) throw new Error('chunk too short'); }
    u8() { this.need(1); return this.b[this.at++]; }
    u16() { this.need(2); const v = this.b[this.at] | (this.b[this.at + 1] << 8); this.at += 2; return v; }
    u32() { const lo = this.u16(); return (lo | (this.u16() << 16)) >>> 0; }
    s32() { return this.u32() | 0; }
    str() {
        const n = this.u16();
        this.need(n);
        const s = new TextDecoder().decode(this.b.subarray(this.at, this.at + n));
        this.at += n;
        return s;
    }
}

const NONE = 0xffff;

/* Parses the playing chunks into plain tables. Throws on malformed ones. */
export function load(bytes) {
    const { chunks } = readCart(bytes);
    for (const t of ['PBG1', 'CODE', 'CNST', 'GLOB'])
        if (!chunks[t]) throw new Error('no ' + t + ' chunk');
    let r = new Reader(chunks.PBG1);
    const header = { format: r.u16(), api: r.u16(), rate: r.u8(),
                     menuButton: r.u8(), music: r.u8() };
    r.u8();
    header.memory = r.u32();
    header.id = Array.from(chunks.PBG1.subarray(r.at, r.at + 8),
                           (x) => x.toString(16).padStart(2, '0')).join('');

    r = new Reader(chunks.CODE);
    const nfns = r.u16();
    const sites = r.u16();
    const entries = { start: r.u16(), init: r.u16(), update: r.u16(), draw: r.u16() };
    if (header.format >= 2) entries.ending = r.u16();
    const fns = [];
    for (let i = 0; i < nfns; i++)
        fns.push({ arity: r.u8(), nlocals: r.u8(), maxstack: r.u16(),
                   offset: r.u32(), length: r.u32() });
    const base = r.at;
    for (const f of fns) {
        if (base + f.offset + f.length > chunks.CODE.length)
            throw new Error('function code runs past CODE');
        f.code = chunks.CODE.subarray(base + f.offset, base + f.offset + f.length);
    }

    r = new Reader(chunks.CNST);
    const strings = [];
    for (let n = r.u16(); n > 0; n--) strings.push(r.str());
    const shapes = [];
    for (let n = r.u16(); n > 0; n--) {
        const keys = [];
        for (let k = r.u8(); k > 0; k--) keys.push(r.u16());
        shapes.push(keys);
    }

    r = new Reader(chunks.GLOB);
    const globals = [];
    for (let n = r.u16(); n > 0; n--) {
        const tag = r.u8();
        if (tag === 0) globals.push({ tag: 'null' });
        else if (tag === 1) globals.push({ tag: 'int', value: r.s32() });
        else if (tag === 2) globals.push({ tag: 'str', value: r.u16() });
        else if (tag === 3) globals.push({ tag: 'fn', value: r.u16() });
        else if (tag === 4) globals.push({ tag: 'fix', value: r.s32() });
        else if (tag === 5) globals.push({ tag: 'img', value: r.u16() });
        else throw new Error('global with unknown tag ' + tag);
    }

    let names = null;
    if (chunks.LINE) {
        r = new Reader(chunks.LINE);
        const files = [];
        for (let n = r.u16(); n > 0; n--) files.push(r.str());
        const gnames = [];
        for (let n = r.u16(); n > 0; n--) gnames.push(r.str());
        for (const f of fns) {
            f.name = r.str();
            f.file = files[r.u8()];
            f.lines = [];
            for (let n = r.u16(); n > 0; n--) f.lines.push([r.u16(), r.u16()]);
        }
        names = { files: files, globals: gnames };
    }
    return { header: header, sites: sites, entries: entries, fns: fns,
             strings: strings, shapes: shapes, globals: globals, names: names };
}

/* One instruction at pc: { op, args, next }. */
export function decode(isa, code, pc) {
    const op = isa.ops[code[pc]];
    if (!op) throw new Error('unknown opcode ' + code[pc] + ' at ' + pc);
    if (pc + op.size > code.length) throw new Error(op.name + ' runs past the end at ' + pc);
    const args = [];
    let at = pc + 1;
    for (const k of op.operands) {
        switch (k) {
        case 'b': args.push((code[at] << 24) >> 24); at += 1; break;
        case 'B': args.push(code[at]); at += 1; break;
        case 'h': args.push(((code[at] | (code[at + 1] << 8)) << 16) >> 16); at += 2; break;
        case 'H': args.push(code[at] | (code[at + 1] << 8)); at += 2; break;
        case 'i': args.push(code[at] | (code[at + 1] << 8) | (code[at + 2] << 16) |
                            (code[at + 3] << 24)); at += 4; break;
        }
    }
    return { op: op, args: args, next: at };
}

const JUMPS = { JMP: 1, JF: 1, JT: 1, ANDJ: 1, ORJ: 1, JLT: 1, JLE: 1,
                JGT: 1, JGE: 1, JEQ: 1, JNE: 1, JLTLI: 1, JLELI: 1, JGTLI: 1,
                JGELI: 1 };

/* where a jump lands: its offset is its last operand */
const target = (d) => d.next + d.args[d.args.length - 1];

/* Stack effect of one instruction, resolving the operand-dependent ones. */
function effect(prog, api, ins) {
    const name = ins.op.name;
    switch (name) {
    case 'CALL': {
        const f = prog.fns[ins.args[0]];
        if (!f) throw new Error('CALL of function ' + ins.args[0] + ', which does not exist');
        return [f.arity, 1];
    }
    case 'CALLV': return [ins.args[0] + 1, 1];
    case 'NCALL': {
        const n = api.bySlot[ins.args[0]];
        if (!n) throw new Error('native slot ' + ins.args[0] + ' is not in this API');
        const argc = ins.args[1];
        if (n.parts) {
            if (argc < 1) throw new Error(n.name + ' with no parts count');
        } else if (argc < n.min || argc > n.max)
            throw new Error(n.name + ' called with ' + argc + ' arguments');
        return [argc, n.returns ? 1 : 0];
    }
    case 'NEWARR': return [ins.args[0], 1];
    case 'NEWOBJ': {
        const s = prog.shapes[ins.args[0]];
        if (!s) throw new Error('shape ' + ins.args[0] + ' does not exist');
        return [s.length, 1];
    }
    case 'METHOD': return [ins.args[1] + 1, 1];
    case 'INTR': return [ins.args[1], 1];
    }
    return [ins.op.pops, ins.op.pushes];
}

/* Throws on the first fault, naming the function and pc. */
export function verify(isa, api, prog) {
    const n = prog.fns.length;
    for (const k in prog.entries) {
        const e = prog.entries[k];
        if (e !== NONE && e >= n) throw new Error('entry ' + k + ' is not a function');
    }
    for (const g of prog.globals) {
        if (g.tag === 'str' && g.value >= prog.strings.length) throw new Error('global string out of range');
        if (g.tag === 'fn' && g.value >= n) throw new Error('global function out of range');
    }
    for (const s of prog.shapes)
        for (const k of s) if (k >= prog.strings.length) throw new Error('shape key out of range');
    prog.fns.forEach((f, fi) => {
        try { verifyFn(isa, api, prog, f); }
        catch (e) { throw new Error('function ' + (f.name || fi) + ': ' + e.message); }
    });
}

function verifyFn(isa, api, prog, f) {
    const code = f.code;
    if (f.arity > f.nlocals) throw new Error('more parameters than locals');
    /* Instruction starts, in one linear pass. */
    const start = new Uint8Array(code.length + 1);
    const ins = [];
    for (let pc = 0; pc < code.length;) {
        start[pc] = 1;
        const d = decode(isa, code, pc);
        d.pc = pc;
        ins[pc] = d;
        pc = d.next;
    }
    /* Operands, then stack heights by flow from pc 0. */
    for (const d of ins) {
        if (!d) continue;
        const a = d.args;
        switch (d.op.name) {
        case 'LDL': case 'STL': case 'INCL':
            if (a[0] >= f.nlocals) throw new Error('local ' + a[0] + ' out of range at ' + d.pc); break;
        case 'LDG': case 'STG':
            if (a[0] >= prog.globals.length) throw new Error('global out of range at ' + d.pc); break;
        case 'GETGL':
            if (a[0] >= prog.globals.length || a[1] >= f.nlocals)
                throw new Error('global or local out of range at ' + d.pc); break;
        case 'GETGP':
            if (a[0] >= prog.globals.length || a[1] >= prog.strings.length || a[2] >= prog.sites)
                throw new Error('global or property out of range at ' + d.pc); break;
        case 'JLTLI': case 'JLELI': case 'JGTLI': case 'JGELI':
            if (a[0] >= f.nlocals) throw new Error('local ' + a[0] + ' out of range at ' + d.pc); break;
        case 'PUSHS':
            if (a[0] >= prog.strings.length) throw new Error('string out of range at ' + d.pc); break;
        case 'PUSHFN':
            if (a[0] >= prog.fns.length) throw new Error('function out of range at ' + d.pc); break;
        case 'GETPROP': case 'SETPROP':
            if (a[0] >= prog.strings.length || a[1] >= prog.sites)
                throw new Error('property operand out of range at ' + d.pc); break;
        case 'METHOD': {
            const m = isa.methods[a[0]];
            if (!m || a[1] < m.min || a[1] > m.max)
                throw new Error('method or its arguments out of range at ' + d.pc);
            break;
        }
        case 'INTR': {
            const x = isa.intrinsics[a[0]];
            if (!x || a[1] < x.min || a[1] > x.max)
                throw new Error('intrinsic or its arguments out of range at ' + d.pc);
            break;
        }
        }
        if (JUMPS[d.op.name]) {
            const t = target(d);
            if (t < 0 || t >= code.length || !start[t])
                throw new Error('jump at ' + d.pc + ' lands mid-instruction or outside');
        }
    }
    const height = new Int32Array(code.length).fill(-1);
    const work = [[0, 0]];
    let max = 0;
    function reach(pc, h) {
        if (pc >= code.length) throw new Error('falls off the end');
        if (height[pc] === -1) { height[pc] = h; work.push([pc, h]); }
        else if (height[pc] !== h)
            throw new Error('stack height ' + h + ' and ' + height[pc] + ' meet at ' + pc);
    }
    height[0] = 0;
    while (work.length) {
        const [pc, h] = work.pop();
        const d = ins[pc];
        const [pops, pushes] = effect(prog, api, d);
        if (h < pops) throw new Error(d.op.name + ' at ' + pc + ' pops an empty stack');
        const name = d.op.name;
        if (name === 'RET' || name === 'RETNULL') {
            if (h - pops !== 0) throw new Error('returns with ' + (h - pops) + ' left on the stack at ' + pc);
            continue;
        }
        const after = h - pops + pushes;
        if (after > max) max = after;
        if (JUMPS[name]) {
            const t = target(d);
            /* ANDJ and ORJ keep their operand when they jump. */
            reach(t, name === 'ANDJ' || name === 'ORJ' ? h : after);
            if (name !== 'JMP') reach(d.next, after);
        } else reach(d.next, after);
    }
    if (max > f.maxstack)
        throw new Error('needs ' + max + ' stack slots, declares ' + f.maxstack);
}

/* The listing pbgc dis prints. */
export function disassemble(isa, api, prog) {
    const out = [];
    const names = prog.fns.map((f, i) => f.name || 'fn' + i);
    prog.fns.forEach((f, fi) => {
        out.push('');
        out.push('function ' + fi + ' ' + names[fi] + '  arity ' + f.arity +
                 '  locals ' + f.nlocals + '  stack ' + f.maxstack);
        const lines = new Map(f.lines || []);
        for (let pc = 0; pc < f.code.length;) {
            const d = decode(isa, f.code, pc);
            let text = d.op.name;
            const a = d.args;
            switch (d.op.name) {
            case 'PUSHS': text += ' ' + JSON.stringify(prog.strings[a[0]]); break;
            case 'PUSHF': text += ' ' + (a[0] / 65536); break;
            case 'PUSHFN': case 'CALL': text += ' ' + names[a[0]]; break;
            case 'NCALL': text += ' ' + (api.bySlot[a[0]] || { name: '?' + a[0] }).name + ' ' + a[1]; break;
            case 'GETPROP': case 'SETPROP': text += ' .' + prog.strings[a[0]]; break;
            case 'GETGP': text += ' ' + a[0] + ' .' + prog.strings[a[1]]; break;
            case 'METHOD': text += ' .' + isa.methods[a[0]].name + ' ' + a[1]; break;
            case 'INTR': text += ' ' + isa.intrinsics[a[0]].name + ' ' + a[1]; break;
            case 'NEWOBJ': text += ' {' + prog.shapes[a[0]].map((k) => prog.strings[k]).join(',') + '}'; break;
            default:
                if (JUMPS[d.op.name])
                    text += (a.length > 1 ? ' ' + a.slice(0, -1).join(' ') : '') + ' -> ' + target(d);
                else if (a.length) text += ' ' + a.join(' ');
            }
            const ln = lines.has(pc) ? String(lines.get(pc)).padStart(5) : '     ';
            out.push(ln + '  ' + String(pc).padStart(5) + '  ' + text);
            pc = d.next;
        }
    });
    return out.join('\n').slice(1);
}
