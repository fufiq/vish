// Common VM protocol codec by schema (`vish schema`): one reader and one writer for
// all messages, without code for each type - as writer/reader on reflection in VM
// (include/vish/serial.hpp). .
//
//
// Frame: u32 body length; body u32 message type number, then field in order. Numbers
// - little-endian; string - u32 length + UTF-8; vector - u32 length + elements;
// variant - u32 number + value; node - u32 (~0 - no).

export type TypeExpr =
  string | readonly ['vector', TypeExpr] | readonly ['variant', ...TypeExpr[]];

export interface Schema {
  version: number;
  client: TypeExpr;
  server: TypeExpr;
  node_kinds: readonly string[];
  types: Readonly<
    Partial<Record<string, readonly (readonly [string, TypeExpr])[]>>
  >;
}

// The value of the alternative aggregate carries the name of its type in `type`.
export type Value =
  | number
  | bigint
  | boolean
  | string
  | null
  | readonly Value[]
  | { readonly [field: string]: Value };

const NO_NODE = 0xffffffff;
const utf8 = new TextDecoder();
const toUtf8 = new TextEncoder();

export class Reader {
  #view: DataView;
  #bytes: Uint8Array;
  #at = 0;

  constructor(
    readonly schema: Schema,
    bytes: Uint8Array,
  ) {
    this.#bytes = bytes;
    this.#view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  get atEnd(): boolean {
    return this.#at >= this.#bytes.byteLength;
  }

  #need(n: number): void {
    if (this.#at + n > this.#bytes.byteLength) {
      throw new Error('протокол: неожиданный конец кадра');
    }
  }

  #u32(): number {
    this.#need(4);
    const v = this.#view.getUint32(this.#at, true);
    this.#at += 4;
    return v;
  }

  read(t: TypeExpr): Value {
    if (typeof t !== 'string') {
      if (t[0] === 'vector') {
        const n = this.#u32();
        const out: Value[] = [];
        for (let i = 0; i < n; ++i) out.push(this.read(t[1]));
        return out;
      }
      const index = this.#u32();
      const alt = t.at(index + 1);
      if (alt === undefined) {
        throw new Error(`протокол: вариант ${String(index)} неизвестен`);
      }
      return this.read(alt);
    }
    switch (t) {
      case 'bool':
      case 'u8': {
        this.#need(1);
        const v = this.#view.getUint8(this.#at++);
        return t === 'bool' ? v !== 0 : v;
      }
      case 'u32':
        return this.#u32();
      case 'node': {
        const v = this.#u32();
        return v === NO_NODE ? null : v;
      }
      case 'u64':
      case 'i64': {
        this.#need(8);
        const v =
          t === 'u64'
            ? this.#view.getBigUint64(this.#at, true)
            : this.#view.getBigInt64(this.#at, true);
        this.#at += 8;
        // Audits and other counters fit in double; otherwise - bigint
        return v <= BigInt(Number.MAX_SAFE_INTEGER) &&
          v >= BigInt(Number.MIN_SAFE_INTEGER)
          ? Number(v)
          : v;
      }
      case 'f64': {
        this.#need(8);
        const v = this.#view.getFloat64(this.#at, true);
        this.#at += 8;
        return v;
      }
      case 'string': {
        const n = this.#u32();
        this.#need(n);
        const s = utf8.decode(this.#bytes.subarray(this.#at, this.#at + n));
        this.#at += n;
        return s;
      }
    }
    const fields = this.schema.types[t];
    if (!fields) throw new Error(`протокол: тип ${t} неизвестен`);
    const out: Record<string, Value> = { type: t };
    for (const [name, ft] of fields) out[name] = this.read(ft);
    return out;
  }
}

export class Writer {
  #bytes = new Uint8Array(256);
  #view = new DataView(this.#bytes.buffer);
  #at = 0;

  constructor(readonly schema: Schema) {}

  get bytes(): Uint8Array {
    return this.#bytes.subarray(0, this.#at);
  }

  #room(n: number): void {
    if (this.#at + n <= this.#bytes.byteLength) return;
    const grown = new Uint8Array(
      Math.max(this.#bytes.byteLength * 2, this.#at + n),
    );
    grown.set(this.#bytes);
    this.#bytes = grown;
    this.#view = new DataView(grown.buffer);
  }

  #u32(v: number): void {
    this.#room(4);
    this.#view.setUint32(this.#at, v, true);
    this.#at += 4;
  }

  write(t: TypeExpr, v: Value): void {
    if (typeof t !== 'string') {
      if (t[0] === 'vector') {
        if (!Array.isArray(v)) throw new Error('протокол: ожидался массив');
        const items = v as readonly Value[];
        this.#u32(items.length);
        for (const x of items) this.write(t[1], x);
        return;
      }
      const index = variantIndex(this.schema, t, v);
      this.#u32(index);
      this.write(t.slice(1)[index] ?? 'string', v);
      return;
    }
    switch (t) {
      case 'bool':
      case 'u8':
        this.#room(1);
        this.#view.setUint8(this.#at++, Number(v));
        return;
      case 'u32':
        this.#u32(Number(v));
        return;
      case 'node':
        this.#u32(v === null ? NO_NODE : Number(v));
        return;
      case 'u64':
      case 'i64':
        this.#room(8);
        if (t === 'u64')
          this.#view.setBigUint64(this.#at, BigInt(v as number), true);
        else this.#view.setBigInt64(this.#at, BigInt(v as number), true);
        this.#at += 8;
        return;
      case 'f64':
        this.#room(8);
        this.#view.setFloat64(this.#at, Number(v), true);
        this.#at += 8;
        return;
      case 'string': {
        const b = toUtf8.encode(v as string);
        this.#u32(b.byteLength);
        this.#room(b.byteLength);
        this.#bytes.set(b, this.#at);
        this.#at += b.byteLength;
        return;
      }
    }
    const fields = this.schema.types[t];
    if (!fields) throw new Error(`протокол: тип ${t} неизвестен`);
    const obj = v as Readonly<Partial<Record<string, Value>>>;
    for (const [name, ft] of fields) {
      const x = obj[name];
      if (x === undefined) throw new Error(`протокол: нет поля ${t}.${name}`);
      this.write(ft, x);
    }
  }
}

// An alternative option for the value: the aggregate is according to `type`, the string
// is "string", the number is the first numerical alternative.
function variantIndex(
  schema: Schema,
  t: readonly ['variant', ...TypeExpr[]],
  v: Value,
): number {
  const alts = t.slice(1);
  const named = (v as { type?: unknown } | null)?.type;
  const i = alts.findIndex((a) =>
    typeof named === 'string'
      ? a === named && a in schema.types
      : typeof v === 'string'
        ? a === 'string'
        : typeof a === 'string' && !(a in schema.types) && a !== 'string',
  );
  if (i < 0)
    throw new Error('протокол: значение не подходит ни к одной альтернативе');
  return i;
}

// Message frame: u32 length + body.
export function frame(schema: Schema, t: TypeExpr, v: Value): Uint8Array {
  const w = new Writer(schema);
  w.write('u32', 0);
  w.write(t, v);
  const out = w.bytes;
  new DataView(out.buffer, out.byteOffset).setUint32(
    0,
    out.byteLength - 4,
    true,
  );
  return out;
}

// parsing stream of bytes per frame: accumulates the tail to the whole frame.
export class FrameSplitter {
  #pending = new Uint8Array(0);

  push(chunk: Uint8Array, onBody: (body: Uint8Array) => void): void {
    const all = new Uint8Array(this.#pending.byteLength + chunk.byteLength);
    all.set(this.#pending);
    all.set(chunk, this.#pending.byteLength);
    const view = new DataView(all.buffer);
    let at = 0;
    while (all.byteLength - at >= 4) {
      const n = view.getUint32(at, true);
      if (all.byteLength - at - 4 < n) break;
      onBody(all.subarray(at + 4, at + 4 + n));
      at += 4 + n;
    }
    this.#pending = all.slice(at);
  }
}

export function decode(schema: Schema, t: TypeExpr, body: Uint8Array): Value {
  const r = new Reader(schema, body);
  const v = r.read(t);
  if (!r.atEnd) throw new Error('протокол: лишние байты в кадре');
  return v;
}
