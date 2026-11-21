# Binding record v1

## Scope

A FHETCH trace names its data by address. A program traced by its author reads
data that the trace does not compute: the author's constants, and the inputs
and evaluation keys a third party supplies later, without access to the program
or its trace. The **binding record** is the table that says where each of those
objects lives in the trace. It is written while the program is traced, travels
with the program, and is read only by the server that runs it.

Each object in it has a **label** and a **kind**, and for each limb of each of
its big polynomials, the trace address that holds it. A third party's upload is
an NBBF file plus a label index (see nbbf's label index specification) using the
same labels. **Bind** joins the two and yields, for every trace address it
fills, the NBBF block that fills it.

Terms: a *big polynomial* is a ring element modulo Q, or Q·P for evaluation
keys, held in RNS form; a *limb* is its `n` residues modulo one prime, stored in
NBBF as one block. One trace address holds one limb.

## Labels

A label is `<kind>:<name>`, using the label index's characters and length
limits.

| Kind | Code | Prefix | Named by | Examples |
| :---- | :---- | :---- | :---- | :---- |
| input | 0 | `input:` | program author | `input:x` |
| output | 1 | `output:` | program author | `output:result` |
| key | 2 | `key:` | system | `key:relin`, `key:relin:3`, `key:galois:5` |
| constant | 3 | `const:` | author or system | `const:weights`, `const:boot:0`, `const:op:17` |

- An author-chosen name is non-empty and contains no `:`. Key names are all
  the system's. A constant's system name always contains one (`boot:0`), so it
  never collides with an author's.
- `key:relin` is the relinearization key for the `s^2` component of a
  ciphertext. `key:relin:<degree>`, 3 or more, relinearizes its `s^degree` component: a
  key set made for relinearizing ciphertexts of degree above 2 has one per
  degree, from `key:relin` up. `key:galois:<g>` is the automorphism key for
  Galois element `g`. No other key name is valid.
- `const:boot:<n>` and `const:op:<n>` are constants the system captures (from
  bootstrapping, and from inside other operations), numbered from 0 in capture
  order within each prefix.
- Numbers (`<degree>`, `<g>`, `<n>`) are decimal without leading zeros, so one object has
  one label.
- A label's prefix matches its object's kind.

## Object layout

Which big polynomial of an object is number 0, 1, … is defined here, in terms of
the CKKS scheme rather than any library's data structures, so producers built on
different libraries number the same object the same way. The binding record and
bind treat the numbers as opaque; producers and the program's tracing must agree
on them.

| Object | `poly_count` | Big polynomial `i` |
| :---- | :---- | :---- |
| Ciphertext (input, output) with elements `c_0 … c_k`, decrypting as `Σ c_j·s^j` | `k + 1` | `c_i` |
| Key-switching key (`key:relin`, `key:relin:<degree>`, `key:galois:<g>`) with `d` digits | `2d` | `i = 2j`: `b_j`; `i = 2j + 1`: `a_j` |
| Constant | 1 | the constant |

- A key-switching key is `d` pairs `(b_j, a_j)`, each shaped like a ciphertext:
  `b_j = −a_j·s + e_j + (digit j's gadget term)`. The pairs are listed in digit
  order, each `b` before its `a` — row by row, with the `b` part playing the role
  of `c_0`.
- Digit `j` is the `j`-th partition of Q's primes, in Q's order, under hybrid key
  switching with the program's digit count. A key built with another partition
  does not bind: its big polynomials miss limbs the record needs.
- A key's big polynomials are over Q·P; a ciphertext's and a constant's over the
  primes of Q its level keeps.

## Conventions

As NBBF: little-endian, natural alignment, reserved fields written as zero and
ignored on read. Addresses are the trace's own addresses, as written in the
FHETCH trace.

## File layout

The file sits next to the trace as `<program>.nbbr`.

| Offset | Size | Type | Field | Value |
| :---- | :---- | :---- | :---- | :---- |
| 0 | 4 | `char[4]` | `magic` | `"NBBR"` |
| 4 | 2 | `u16` | `version` | 1 |
| 6 | 2 | `u16` | `flags` | 0 (reserved) |
| 8 | 8 | `u64` | `sentinel` | `0x0123456789ABCDEF` |
| 16 | 4 | `u32` | `ring_dim` | Ring dimension `n` |
| 20 | 2 | `u16` | `num_moduli` | `M` |
| 22 | 2 | `u16` | reserved | 0 |
| 24 | 4 | `u32` | `num_objects` | `O` |
| 28 | 4 | `u32` | `num_runs` | `R` |
| 32 | 4 | `u32` | `label_pool_len` | Bytes in the label pool |
| 36 | 4 | `u32` | reserved | 0 |
| 40 | 8M | `u64[M]` | `moduli` | Every prime the program uses |
| 40+8M | 8M | `u64[M]` | `roots` | The root of each prime, as NBBF defines it |
| 40+16M | 20·O | — | `objects` | See Objects |
| … | — | — | padding | Zeros to the next multiple of 8 |
| … | 16·R | — | `runs` | See Runs |
| … | `label_pool_len` | `u8[]` | `label_pool` | Label bytes, no terminators |

The file ends after the label pool.

## Objects

| Offset | Size | Type | Field |
| :---- | :---- | :---- | :---- |
| 0 | 4 | `u32` | `label_offset` — from the start of `label_pool` |
| 4 | 2 | `u16` | `label_len` |
| 6 | 1 | `u8` | `kind` — code from the Labels table |
| 7 | 1 | `u8` | reserved |
| 8 | 2 | `u16` | `poly_count` — big polynomials in the object |
| 10 | 2 | `u16` | reserved |
| 12 | 4 | `u32` | `first_run` — index into `runs` |
| 16 | 4 | `u32` | `run_count` |

## Runs

A run maps consecutive limbs of one big polynomial to consecutive addresses.

| Offset | Size | Type | Field |
| :---- | :---- | :---- | :---- |
| 0 | 2 | `u16` | `big_poly` — the big polynomial's number within its object |
| 2 | 2 | `u16` | `first_modulus` — index into this file's `moduli` |
| 4 | 2 | `u16` | `limb_count` |
| 6 | 2 | `u16` | reserved |
| 8 | 8 | `u64` | `first_address` |

Limb `first_modulus + i` of big polynomial `big_poly` is at address
`first_address + i`, for `i < limb_count`.

Runs only make the table small. A writer walks each object's limbs in
(big polynomial, modulus index) order and starts a new run whenever the next
limb belongs to another big polynomial, its address is not the previous one
plus 1, or its modulus index is not the previous one plus 1; in the worst case
every run has one limb. A limb the trace
never reads lies in no run.

## Validation

A reader MUST check:

1. `magic`, `sentinel` (byte-swapped: reject with a distinct error) and
   `version`; the file size matches the layout.
2. No prime appears twice in `moduli`; `roots[k]` is non-zero and valid for
   `moduli[k]` (as NBBF's check 10) whenever a run uses modulus `k`.
3. Every object: `kind <= 3`; its label is within the pool, well-formed, and its
   prefix matches `kind`; `poly_count >= 1`; `first_run + run_count <= R`.
4. No two objects have the same label; objects' run ranges do not overlap, and
   together they cover every run.
5. Every run: `big_poly < poly_count` of its object;
   `first_modulus + limb_count <= M`; `limb_count >= 1`;
   `first_address + limb_count - 1` does not overflow `u64`.
6. No (object, big polynomial, modulus) appears in two runs, and no address
   appears in two runs.

## Bind

`bind(record, kind, nbbf, label_index)` takes one upload of one kind and yields
a list of `(address, block)` pairs, or fails naming the label. The label index
is validated against the NBBF file first, as its specification requires.

It fails when:

- `ring_dim` of a block it uses differs from the record's;
- for a modulus the record needs, the upload's `roots` entry differs from the
  record's;
- an object of `kind` in the record has no entry of the same label in the label
  index;
- an entry's `poly_count` differs from the object's;
- a run's limb is missing from the upload: no block of that big polynomial whose
  modulus *value* equals the record's;
- a block it uses is not in evaluation domain, bit-reversed order;
- `kind` is input and the label index has a label the record lacks, or has a
  label whose prefix is not `input:`.

It ignores, and reports:

- for keys and constants, labels that name no object of `kind` in the record;
- limbs the upload has that no run needs, such as a ciphertext at a higher
  level than the program reads.

Moduli are matched by value: a block's `modulus_index` refers to its own file's
`moduli`, which may list primes in another order or list others besides.

Kind `output` is never bound; a result is written the other way (Result).

## Result

`write_result(record, output_metadata, residues, dir)` writes a run's outputs as
`result.nbbf`, `result.nbli` and `result.nbom`. `residues(address, modulus)` is
the server's: the `ring_dim` residues it computed at that trace address, in
`[0, modulus)`, evaluation domain, bit-reversed order.

- Objects follow record order; each output's big polynomials are contiguous,
  the first at the next free `poly_index`.
- Every limb of every run becomes one block of its big polynomial under its
  modulus, in evaluation domain, bit-reversed order.
- The NBBF file declares only the moduli the outputs use, in the record's order,
  each with the record's root.
- The label index names each output object; `result.nbom` holds the output
  metadata entry of each, in the same order.

It fails when an output has no output metadata entry, or a limb's residues are
not `ring_dim` many or not below their modulus. No result file is written then.

## Output metadata

What a third party needs, besides the residues, to rebuild each output as a
CKKS ciphertext. Every CKKS implementation tracks an equivalent of each field.
Level is not stored: it follows from which primes the output's blocks use.
Domain and order are not stored: the blocks declare them.

The tracing writes `<program>.nbom` next to the trace, one entry per output; the
server delivers it with every result as `result.nbom`. Conventions as above.

| Offset | Size | Type | Field | Value |
| :---- | :---- | :---- | :---- | :---- |
| 0 | 4 | `char[4]` | `magic` | `"NBOM"` |
| 4 | 2 | `u16` | `version` | 1 |
| 6 | 2 | `u16` | `flags` | 0 (reserved) |
| 8 | 8 | `u64` | `sentinel` | `0x0123456789ABCDEF` |
| 16 | 4 | `u32` | `num_entries` | `N` |
| 20 | 4 | `u32` | `label_pool_len` | Bytes in the label pool |
| 24 | 24N | — | `entries` | See below |
| 24+24N | `label_pool_len` | `u8[]` | `label_pool` | Label bytes |

| Offset | Size | Type | Field |
| :---- | :---- | :---- | :---- |
| 0 | 4 | `u32` | `label_offset` |
| 4 | 2 | `u16` | `label_len` |
| 6 | 2 | `u16` | reserved |
| 8 | 8 | `f64` | `scaling_factor` — the scale `Δ'` the ciphertext's message is multiplied by, IEEE 754 binary64 |
| 16 | 4 | `u32` | `scale_degree` — `d`, the number of factors of the base scale it carries (1 for a fresh or rescaled ciphertext) |
| 20 | 4 | `u32` | `slots` — CKKS slots encoded, at most `n/2` |

A reader MUST check magic, sentinel (byte-swapped: distinct error), version and
exact file size; that every label lies within the pool, is a valid `output:`
label, and is unique. Reserved fields are ignored on read.

## Versioning

As for NBBF.
