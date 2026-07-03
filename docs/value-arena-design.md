# Value arena — id-based AST representation (design proposal)

**Status:** design proposal; the production AST is `shared_ptr<Value>`.

**Implemented so far (branch `explore/builder-seam`):**
- `Builder` interface + `SharedPtrBuilder` (`src/builder.{hpp,cpp}`) — value/
  begin/end + checkpoint/rollback + record/replay, unit-tested.
- **Shadow validation harness** (`RAWAST_SHADOW_CHECK`, off by default): drives a
  parallel `SharedPtrBuilder` through the whole driver and compares to the
  authoritative `Frame` result. **0 divergences** across the C++ suite (440
  cases), Python suite (194), the full ~11.4k-file LEF/DEF/TCL/GDSII corpus, and
  Ibex SV (RTL 30/30 + dv/uvm 109/109). Four cutover bugs were found+fixed via
  the harness (Repeat rollback, repeat+ min, ctor-pre-seeded constants,
  negative-lookahead leaks). The event placement is *proven*; the cutover
  (builder authoritative, Frame value logic removed, cache on record/replay) is
  unblocked but not yet done.
- `register_usage` dropped from the parse hot path (per-child back-ref index
  nothing read; ~5% on Ibex).

Two refinements from later in the design discussion are reflected in the roadmap
(§0) and the builder seam (§5) but are **not yet rewritten into the encoding
sections (§1–§4)** — those still describe the original standalone tag-in-id
design. The pending foundation revision:
- (a) **For use as dagland's L0**, the id model should move from tag-in-id to a
  **generational-index node schema** with kind/span as columns (drop immediates;
  every node has its own identity so cross-level links + provenance work). rawast
  joins dagland's shared arena framework rather than owning a private arena.
- (b) **Construction is via the builder seam (§5)**, which supersedes
  build-then-freeze.
- (c) **An intermediate model is the recommended near-term step** (§0.5): an
  arena-allocated `Value` tree — same class hierarchy, region-arena allocation,
  raw non-owning pointers, flat dicts — delivered as an `ArenaValueBuilder`
  through the seam. Most of the speed/memory win, a fraction of the rewrite.

These narrow the §10 open decisions (spans become persistent + per-node).

## Motivation

The current AST is a `shared_ptr<Value>` graph. For large inputs (multi-MB
gate netlists → millions of nodes) that costs a control block + atomic
refcount + a pointer per node, plus pointer-chasing on every walk. The goal:
a compact, cache-friendly, arena-backed representation where a value is a
small tagged integer **id**, identical small values and all strings are
interned, and the type is readable from the id without a dereference.

Round-trip (`a == parse(save(a))`) and the construction toolkit
(build/edit LEF/DEF in Python) remain first-class — both shape the design.

---

## 0. Roadmap — the builder seam

Construction goes through a pluggable **builder** (§5): the parse driver emits
events and the chosen builder materializes the representation. This **supersedes
the earlier build-then-freeze plan** — the `ArenaBuilder` builds the arena
directly, so there is no `shared_ptr` intermediate, no separate freeze pass, and
no peak-memory spike.

1. **Extract the builder seam.** Route every value/container emit in the driver
   through a `Builder&`. Ship `SharedPtrBuilder` reproducing today's behavior;
   **validate `driver → SharedPtrBuilder == today` on the full corpus.** A pure,
   behavior-preserving, corpus-gated refactor — the one step that touches the
   driver (front-loads the risk instead of deferring it).
2. **`ArenaBuilder`** (builds the arena, §3) + `checkpoint/rollback` for PEG
   backtracking. Validate `parse(ArenaBuilder) → save → reparse == original` on
   the full corpus.
3. **`PythonBuilder`** (native dict/list — back-compat for existing consumers)
   and **host builders** (dagland builds its L0 columns directly).
4. *(measure-gated)* template-specialize the hot built-in builders if the
   per-event virtual cost shows up in a profile.

---

## 0.5 The intermediate model — arena-allocated `Value` tree (recommended first)

Between today's `shared_ptr<Value>` and the id-columnar arena there is a step
that captures most of the win at a fraction of the risk: **keep the exact
`Value` hierarchy and tree shape, change only allocation, ownership, and the
dict container.**

| today | intermediate | removes |
|---|---|---|
| `make_shared<Value>()` per node | bump-allocate from one region arena | per-node `malloc` + control block |
| `shared_ptr<Value>` edges | raw `Value*` into the arena (non-owning) | atomic refcount traffic |
| `DictValue` = `std::map` | flat sorted `vector<pair>` | a red-black-tree node **per dict entry** |
| node-by-node teardown | whole-arena free | destructor storm |

Why this is the sweet spot:
- The measured large-file cost is **node count × per-node overhead** (the 42 MB
  gate netlist doesn't parse in minutes on the current model). Primitives are
  already interned — non-reuse is *not* the problem; allocator and refcount
  overhead are. This removes exactly those without touching `type()`/`data()`,
  vtable dispatch, save, or any tree-walking consumer.
- **Lifetime**: the arena owns every node; Python holds the arena wrapper
  alive, accessors borrow raw pointers. Interning still works (one arena node,
  many raw pointers to it). Constraint: no per-node free — parse→use→drop, and
  the edit toolkit must allocate into the arena too.
- **Delivery = `ArenaValueBuilder`** through the §5 seam (no parser changes),
  shadow-validated against `SharedPtrBuilder` for free (0 divergences ⇒ the
  arena tree is identical, just cheaper memory).
- The id-columnar arena (§1–§3) stays the *final* target for dagland-L0
  (compactness, mmap, cross-level ids); this step doesn't block it — same seam,
  a later builder.

---

## 1. The id

**32-bit, tag in the low 4 bits** (low-bit tag → arithmetic-shift sign
extension is free for immediates).

```
bits:   [ payload : 28 ][ tag : 4 ]

tag(id)        = id & 0xF
index(id)      = id >> 4                 // unsigned — pool indices / offsets
imm_int(id)    = (int32_t)id >> 4        // arithmetic — signed immediate
make(tag, pl)  = ((uint32_t)pl << 4) | tag
```

- `id == 0` is **invalid** — it is `tag 0 (SPECIAL), payload 0`, and doubles
  as the container-run terminator.
- 4-bit tag = 16 kinds (11 used, 5 reserved). 28-bit payload → immediates
  ±134M, pools/offsets up to ~268M — ample for a single document.
- This codec is the **only** code that knows the bit layout. One module,
  exhaustively unit-tested (sign extension and the `0`/zero-int interplay are
  the classic bug sites).

---

## 2. Tags

| tag | name | payload | backing pool |
|----:|------|---------|--------------|
| 0 | `SPECIAL` | 0 = invalid (`id 0`), 1 = null, 2 = false, 3 = true, 4 = undefined | — (immediate) |
| 1 | `INT` | the signed value (arith-shift) | — (immediate) |
| 2 | `STRING` | index into `strings` (interned) | `strings` |
| 3 | `INT_BIG` | index — ints beyond ±134M | `int64s` |
| 4 | `UINT` | index | `uint64s` |
| 5 | `REAL` | index | `reals` |
| 6 | `LIST` | offset into `children` | `children` |
| 7 | `DICT` | offset into `children` | `children` |
| 8 | `LOCATED` | index into `occurrences` (`{value_id, span}`) | `occurrences` |
| 9 | `JUMP` | continuation offset (run control entry, build-form only) | — |
| 10–15 | reserved | | |

Bool is a `SPECIAL` sub-code (payloads 2 = false, 3 = true), not its own tag —
so the payload's **low bit is the boolean value**:
`bool_value(id) = (id >> 4) & 1`, and `is_bool(id) = tag(id) == SPECIAL &&
(payload >> 1) == 1`. Costs one extra compare on `is_bool` versus a dedicated
tag, but spends no tag slot.

The set is **pinned to the `ValueType` model** (Null/Undef/Bool/Int/UInt/
Real/String/Array/Dict) plus immediate variants, `LOCATED`, and the `JUMP`
control entry. Every tag is a maintenance commitment across save / equals /
accessor / freeze — add one only when the value *model* grows.

### Encodings of the singletons (for reference)

```
invalid    = 0                  // SPECIAL, payload 0
null       = make(SPECIAL, 1)   // = 16
false      = make(SPECIAL, 2)   // = 32   (payload low bit = 0)
true       = make(SPECIAL, 3)   // = 48   (payload low bit = 1)
undefined  = make(SPECIAL, 4)   // = 64
int 0      = make(INT, 0)       // = 1    (nonzero — never collides with id 0)
```

---

## 3. Storage — the `Document`

One arena owns everything; an id is meaningful only inside its `Document`.

```cpp
struct Span        { uint32_t start, end; };        // byte offsets
struct Occurrence  { uint32_t value_id; Span span; };

struct Document {
    std::vector<std::string> strings;     // interned value pool
    std::vector<int64_t>     int64s;      // INT_BIG
    std::vector<uint64_t>    uint64s;     // UINT
    std::vector<double>      reals;       // REAL
    std::vector<uint32_t>    children;    // THE container pool (id runs)
    std::vector<Occurrence>  occurrences; // LOCATED
    uint32_t                 root;        // top id

    // build-time only:
    std::unordered_map<std::string_view, uint32_t> intern;  // string dedup
};
```

### Containers share one pool

`LIST` and `DICT` both index `children`; the **tag decides interpretation**,
the storage is identical:

- `LIST` at offset *o*: `children[o], children[o+1], … , 0`  (items, then the
  `0` terminator).
- `DICT` at offset *o*: `name, val, name, val, … , 0`  (pairs; terminates when
  a **name** slot is `0`).

Nesting works because child ids point at other runs in the same pool. A
container is a single `tag + offset` id — no separate length is stored.

### Reserved index 0

Index 0 is left unused in every pool, so `id == 0` stays uniquely invalid and
`int 0` (`make(INT,0) = 2`) never collides with it.

### `LOCATED` (optional source location)

`LOCATED` wraps a bare value with a span: `occurrences[i] = {value_id, span}`.
Two occurrences of the same value share the inner `value_id` but get distinct
`LOCATED` ids (distinct spans). The inner `value_id` is **never** itself a
`LOCATED` (no chaining).

### `JUMP` (mutable build form only)

While editing, a full container run can continue elsewhere: a `JUMP` entry
redirects the scan to its `index`. This makes append O(1) amortized without
relocation. **`freeze`/`compact` removes all `JUMP`s** — the frozen form is
contiguous and jump-free. If the AST is write-once, `JUMP` is never produced.

---

## 4. The `Arena` class — owner and single API

`Document` (§3) is the data; the **`Arena`** is the class that owns it and is
the one API surface for everything id↔value. ids are only meaningful relative
to an arena, so routing every id operation through it is also how invariant #4
("an id never escapes its document") is *enforced* rather than hoped for.

**The id codec stays free and stateless** (`id.hpp`): `tag()`, `make()`,
`imm_int()` are pure bit ops on the id, need no arena, and are unit-tested in
isolation. The arena *uses* the codec; it does not contain it.

```cpp
class Arena {
  // build
  Id intern(std::string_view);        // -> STRING (deduped)
  Id make_int(int64_t);               // immediate, or INT_BIG pool on overflow
  Id make_real(double);
  Id make_list(std::span<const Id>);  // copies a run into `children` + 0 term
  Id make_dict(std::span<const Pair>);
  Id locate(Id value, Span);          // -> LOCATED (value must be bare)

  // access (resolve via codec + pools)
  Tag              tag(Id) const;
  int64_t          as_int(Id) const;
  std::string_view as_str(Id) const;
  RunView          items(Id) const;   // LIST/DICT iteration
  Id               dict_get(Id, Id name) const;
  Id               peel(Id) const;    // strip LOCATED
  Span             span_of(Id) const;

  // whole-tree
  bool equals(Id, Id) const;          // peels LOCATED; dict order-free
  void save(std::ostream&, Id) const;
  Id   clone(Id);                     // deep copy into this arena
  void compact();                     // strip JUMPs, re-intern, contiguous runs
};
```

### Bare ids inside, handles outside

Internally — in `children`, the pools, container runs — values are **bare
4-byte ids** (the whole point). At the **public boundary** (C++ and Python)
the API hands out a lightweight **handle `{Arena*, Id}`** so callers write
`h.as_int()` and the handle knows which arena it belongs to. The Python
accessor *is* this handle. Bare ids never leave the arena's own storage.

This also closes the one real footgun: **using an id from arena A against
arena B is silent corruption.** Handles carrying the arena make that a type
error; a debug-build arena-id/generation tag catches the rest.

### It is the new parse result

`Grammar::parse` returns an `Arena` (owning the AST) in place of a
`shared_ptr<Value>`; `save` takes `(arena, root)`. The same class serves both
directions — `freeze` fills it, the construction toolkit edits it — so one
owner spans the whole lifecycle.

### One class, or Builder → Document?

Start with **one `Arena`**, mutable, with `compact()/freeze()` that normalizes
(strips `JUMP`s, re-interns, contiguous runs); the "frozen" state is just a
compacted arena. Promote to a split (mutable `Builder` → immutable `Document`)
only if you want the type system to forbid mutating a frozen arena.

### Pool representation is hidden — defer the layout

Because every consumer goes through the arena's index API, the **pool
representation is an implementation detail behind the class.** Ship with plain
`std::vector`s and `reserve()` each pool exactly at `freeze` (the tree size is
known then → *zero* reallocations on the parse path). If incremental mutation
or a streaming Phase 2 makes reallocation spikes (peak-memory / latency) bite,
swap the hot pools (`children`, `strings`) to **segmented / paged** storage —
no `vector` recopy, stable, slightly slower indexing — **without touching a
single consumer.** save / equality / accessor / codec are unaffected.

---

## 5. Builder seam

The parse driver does **not** know what an AST *is*. It emits construction events
to a `Builder`, and the chosen builder decides what to materialize — a SAX-style
sink, not a fixed DOM. This is the single seam through which rawast can target
*any* representation, including a **host's**: dagland passes in a builder that
writes straight into its L0 columns during the parse, with no rawast-specific
intermediate.

```cpp
struct Builder {                          // virtual → host-pluggable at runtime
  // scalars (Span = source range of this value)
  virtual void null_(Span)                  = 0;
  virtual void bool_(bool, Span)            = 0;
  virtual void int_(int64_t, Span)          = 0;
  virtual void uint_(uint64_t, Span)        = 0;
  virtual void real_(double, Span)          = 0;
  virtual void string_(std::string_view, Span) = 0;
  // containers
  virtual void begin_list(Span)             = 0;
  virtual void begin_dict(Span)             = 0;
  virtual void key(std::string_view, Span)  = 0;   // before each dict value
  virtual void end()                        = 0;
  // engine quirks, kept explicit so no representation is privileged
  virtual void subparse(/* captured region re-parsed */) = 0;
  virtual void name_marker(/* opchain / fixed-schema */) = 0;
  // backtracking — mirrors the stream's mark / reject
  virtual Checkpoint checkpoint()           = 0;
  virtual void       rollback(Checkpoint)   = 0;
  virtual void       commit(Checkpoint)     = 0;
};
```

**Builders:** `SharedPtrBuilder` (today's tree), `ArenaBuilder` (§3 columns),
`PythonBuilder` (native dict/list), `CountBuilder` (sizing/validation, no build),
host builders (dagland L0). **Compile-time** choice = template the driver on the
builder (zero dispatch, code per builder); **runtime** choice = the virtual
interface above (one binary, host-pluggable). The per-event cost is per-*value*,
not per-byte — far coarser than the hot loop — so the virtual seam is the primary
one; template-specialize only if a profile says so.

### Backtracking is the hard part — and it is contained here

PEG tries an alternative, emits events, and on failure must undo them. The
builder owns that via `checkpoint/rollback` tied to the stream's `mark/reject`:

- `SharedPtrBuilder.rollback` — drop the in-progress values (automatic).
- `ArenaBuilder.rollback` — truncate the columns / run-pool back to the
  checkpoint length.

So the transactional-arena difficulty lives in **one** builder, isolated and
unit-testable, instead of smeared through the driver. This also **subsumes the
old freeze pass** — the `ArenaBuilder` *is* the lowering, done during the parse;
`compact()` (§4) remains only for post-edit normalization of a *mutated* arena.

### The save dual — the `Accessor`

`Builder` makes the *parse* side representation-agnostic. Save is the dual but a
different shape: it is **grammar-driven random-access read** of the
representation (walk the grammar, pull the matching field / elements / scalar),
not a linear push. So the save-side adapter is an **`Accessor`** — read getters
over a representation (`kind`, `dict_get`, `array_items`, `as_string`) — *not* a
sink. Today `Grammar::save` is hard-wired to `ValuePtr`; routing it through an
`Accessor` (with a `SharedPtrAccessor` reproducing current behavior, corpus-
gated) is the symmetric refactor.

Each representation therefore provides a **pair**: a `Builder` (parse constructs
it) and an `Accessor` (save serializes it). The `Accessor` is a *future* step —
needed only when a second representation (the arena) must round-trip; `ValuePtr`
save is unchanged until then.

---

## 6. Operations

```
resolve(doc, id):
    switch tag(id):
      SPECIAL  -> invalid / null / false / true / undefined (from payload;
                  bool value = (payload & 1) when payload in {2,3})
      INT      -> imm_int(id)
      STRING   -> doc.strings[index(id)]
      INT_BIG  -> doc.int64s[index(id)]      (UINT/REAL analogous)
      LIST/DICT-> a run view at doc.children[index(id) ...]  (scan to 0,
                  following JUMP entries in the build form)
      LOCATED  -> resolve(doc, occurrences[index(id)].value_id)
                  (+ .span available on the side)

len(list)  = scan the run to the terminator   // O(n); cache if it bites

save(doc, id):
    walk by id, dispatch on tag, PEEL LOCATED (span ignored),
    scan runs to the 0 terminator.

equals(doc, a, b):
    PEEL LOCATED on both first (span-insensitive)
    by tag:
      immediates (SPECIAL/INT)      -> id compare
      STRING                        -> intern index compare (id compare)
      INT_BIG/UINT/REAL             -> value compare
      LIST                          -> same length + elementwise equals
      DICT                          -> ORDER-INDEPENDENT: compare as
                                       name->value (matches today's std::map)
```

---

## 7. Invariants (load-bearing — write tests first)

1. `id == 0` = invalid / run terminator; index 0 unused in every pool.
2. `equals` **ignores spans** (peels `LOCATED`); `DICT` equality is
   **order-independent**. (Both are required for `parse(save(x)) == x`: spans
   move when text is re-emitted, and today's dict is a sorted map.)
3. `LOCATED` never wraps `LOCATED`.
4. The `Document` owns all values; an id must never escape its `Document`.
5. Interning: strings always; immediates dedup for free (identical value →
   identical id); whole-subtree hash-consing is **optional** and is **off**
   wherever per-occurrence spans are wanted (a shared subtree can carry only
   one location).

---

## 8. Value vs occurrence (why interning and spans coexist)

Interning dedups by **value**; source location distinguishes by
**occurrence**. They are different axes, so they live in different layers:

- **Value** (interned, shared, location-free) — the int, the string. One id
  per distinct value.
- **Occurrence** (`LOCATED`, per-appearance, carries the span, references a
  value) — the tree is made of these where location is needed.

Two equal-but-differently-placed `0`s are two `LOCATED` ids over one immediate
`INT 0`. This is the rustc `Symbol` / `Ident = (Symbol, Span)` split,
generalized and made opt-in per node.

---

## 9. Module layout

```
id.hpp            Tag enum + codec. The ONLY place that knows the bit layout.
document.{hpp,cpp} Document, pools, builder ops (intern, append_*, make_list/
                   dict), compact().
freeze.cpp        shared_ptr Value tree -> Document lowering (Phase 1; also the
                   Phase-2 diff oracle).
save_doc.cpp      save walking a Document (peels LOCATED, scans runs).
equal_doc.cpp     span-insensitive, order-independent-dict equality.
bindings          Python accessor: handle = (doc, id); lazy resolve to
                   int/str/bool/None or a sub-accessor; .span on LOCATED;
                   equality peels location.
```

`freeze` is the keystone of the low-risk path: it builds and proves the entire
`Document` stack against the *current* parser, so the corpus stays green while
the new representation is validated independently.

---

## 10. Open decisions (these change §2–§3; settle before coding)

1. **Span granularity** — every node, or only containers/statements? Every
   node ⇒ every slot is a `LOCATED` (immediates stop saving a slot). Some
   nodes ⇒ bare immediates/value-ids where no location is needed, `LOCATED`
   only where it is. *Lean: container/statement-level — errors point there
   anyway, immediates survive.*
2. **Span lifetime** — persistent (tooling/LSP) or transient (parse-error
   only)? Decides whether `occurrences`/`LOCATED` exist in the saved form or
   `freeze` strips them. (Preprocessed input also needs a source-map through
   macro expansion — the `Stream` owner slot — separate from this.)
3. **32- vs 64-bit id** — 32 is the compactness win but caps immediates at
   ±134M and pools at ~268M; 64 removes both and allows NaN-boxing reals, at
   2× per-slot. *Lean: 32, with `INT_BIG`/`REAL` pools as the overflow path —
   unless a profile shows wide literals dominate.*

Everything else — low-bit codec, bool folded under `SPECIAL` (payloads 2/3,
low bit = value), the tag set pinned to the value model, one shared container
pool with `0`-terminated runs, value↔occurrence split, build-then-freeze, the
§7 invariants — is settled.
