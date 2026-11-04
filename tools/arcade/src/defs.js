/* The instruction set and the native API, read from the engine's own .def
 * files so the compiler cannot drift from the VM. Takes the files' text
 * rather than paths, so the browser IDE can pass in copies it inlined. */

function args(s) {
    return s.split(',').map(function (a) { return a.trim(); });
}

function each(text, macro, fn) {
    const re = new RegExp('^\\s*' + macro + '\\(([^)]*)\\)', 'mg');
    let m;
    while ((m = re.exec(text)) !== null)
        fn(args(m[1]));
}

const OPERAND_SIZE = { b: 1, B: 1, h: 2, H: 2, i: 4 };

export function parseOpcodes(text) {
    const ops = [];
    const byName = {};
    each(text, 'OP', function (a) {
        const operands = a[1].replace(/"/g, '');
        let size = 1;
        for (const k of operands) size += OPERAND_SIZE[k];
        const op = {
            name: a[0], code: ops.length, operands: operands, size: size,
            pops: parseInt(a[2], 10), pushes: parseInt(a[3], 10)
        };
        ops.push(op);
        byName[op.name] = op;
    });
    const methods = [];
    each(text, 'MTH', function (a) {
        methods.push({ name: a[0], id: methods.length,
                       min: +a[1], max: +a[2] });
    });
    const intrinsics = [];
    each(text, 'INTRINSIC', function (a) {
        intrinsics.push({ name: a[0], id: intrinsics.length,
                          min: +a[1], max: +a[2] });
    });
    return { ops: ops, op: byName, methods: methods, intrinsics: intrinsics };
}

export function parseApi(text) {
    const natives = Object.create(null);
    const bySlot = [];
    each(text, 'API', function (a) {
        const n = {
            name: a[0], slot: +a[1], min: +a[2], max: +a[3],
            returns: a[4] === 'VAL', parts: a[5] === 'PARTS',
            cost: a[6], level: a[7], version: +a[8]
        };
        natives[n.name] = n;
        bySlot[n.slot] = n;
    });
    each(text, 'API_ALIAS', function (a) {
        natives[a[0]] = natives[a[1]];
    });
    /* API_NS(ns): a native ns_name is ns.name in a cart, and only that */
    const namespaces = Object.create(null);
    each(text, 'API_NS', function (a) {
        const ns = Object.create(null);
        for (const name of Object.keys(natives)) {
            if (!name.startsWith(a[0] + '_')) continue;
            const def = natives[name];
            def.label = a[0] + '.' + name.slice(a[0].length + 1);
            ns[name.slice(a[0].length + 1)] = def;
            delete natives[name];
        }
        namespaces[a[0]] = ns;
    });
    const consts = Object.create(null);
    each(text, 'API_CONST', function (a) {
        consts[a[0]] = { value: +a[1], version: +a[2] };
    });
    const groups = Object.create(null);
    each(text, 'API_CONSTG', function (a) {
        if (!groups[a[0]]) groups[a[0]] = Object.create(null);
        groups[a[0]][a[1]] = { value: +a[2], version: +a[3] };
    });
    return { natives: natives, bySlot: bySlot, consts: consts, groups: groups,
             namespaces: namespaces };
}
