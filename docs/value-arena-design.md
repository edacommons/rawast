# The universal engine — representation-agnostic parse + save

**TARGET (decided 2026-07-04): a universal engine, not any single
representation.** The engine core never touches a concrete value type; it
speaks exactly two interfaces, and a **representation = a {Builder, Accessor}
implementation pair**:

```
Builder   (write): typed value events, begin/end, checkpoint/rollback, record/replay
Accessor  (read):  kind, dict_get, items, scalar getters
```

Every engine operation composes from the pair:
- parse                 = grammar walk → Builder
- save                  = grammar walk reads Accessor
- transforms (opchain)  = Accessor → Builder pipe
- equality / round-trip = Accessor × Accessor
- save's scratch nodes  = the representation's own Builder

`shared_ptr<Value>` (the reference pair), an id-columnar arena (dagland L0),
native Python, host structures — all plug-ins of equal standing. The engine is
universal over representations of ONE value model (dict/array/scalars — what
grammars bind to); that fixed contract is what keeps the interfaces small.
The id-arena sections below (§1–§4) describe ONE future plug-in, not the
target; the former "intermediate model" (§0.5) is a fallback plug-in, not a
roadmap step.

**Implemented so far (main + branch `feat/builder-cutover`):**
- `Builder` + `SharedPtrBuilder`, proven by the shadow harness (0 divergences
  across suites, the 11.4k corpus, Ibex, the broad SV sweep), then **cutover
  landed**: the Builder is the authoritative sink; Frame is structure-only;
  cache on record/replay (`381d7dd`).
- Save dispatch layer borrows `const Value*` (~3–6% save win, `bd4b18f`).
- `register_usage` dropped from the parse hot path (~5%, shipped v0.1.11).

**Remaining representation leaks to close (the universal-engine roadmap):**
1. **Typed leaf events** — `Builder::value(ValuePtr)` still passes a concrete
   value; terminals should emit payloads (`string_`, `int_`, `uint_`, `real_`,
   `bool_`, `null_`) and the builder materialises.
2. **Interning behind the Builder** — `pool.intern` in the driver is a
   representation concern (sharing strategy), not an engine concern.
3. **Opaque `Recording`** — the cache token becomes builder-defined.
4. **Subparse hand-off** — sub-parse product flows builder-to-builder.
5. **`Accessor` + save routed through it** (`SharedPtrAccessor` first,
   corpus-gated); the opchain re-nest becomes an Accessor→Builder rewrite.
6. **`compact_opchain`** as an Accessor→Builder pass (the last ValuePtr-bound
   transform).
7. Perf guard: template the hot walks on the interface (compile-time, zero
   dispatch), virtual form for host plug-ins; measure before assuming cost.

The id-arena plugs in afterwards as just another pair — it validates the
interfaces rather than defining them. For its schema, the standing decision:
generational-index node schema with kind/span columns (dagland-native), NOT
the tag-in-id encoding §1–§4 still describe.

---

## THE TARGET API (main target, decided 2026-07-04)

Driving scenario: an app compiles with rawast and wants a **different AST
representation per format** — DEF into an id-arena, TCL into plain Values,
SV straight into the app's own IR.

### The two primitives — everything else is sugar

For a custom representation, parse's "result" is whatever the builder
accumulated (possibly a side effect on the app's store — e.g. dagland's
builder writing into its graph). So the primitives return only success, and
the builder/accessor own the product:

```cpp
tl::expected<void, ParseError> Grammar::parse_into(Stream&, Builder&);
tl::expected<void, SaveError>  Grammar::save_from(std::ostream&, Accessor&);
```

### A representation is a bundle (traits), not just a builder

```cpp
// concept RepresentationLike:
//   R::Builder    — models BuilderLike  (typed events, checkpoint/rollback, Recording)
//   R::Accessor   — models AccessorLike (kind, dict_get, items, scalar getters)
//   R::Document   — the owning result   (ValuePtr | Arena | app handle | void)
struct SharedPtrRepr { using Builder = SharedPtrBuilder; /* … */ using Document = ValuePtr; };
struct ArenaRepr     { /* id-columnar */                 using Document = Arena;    };
```

### Call-site sugar

```cpp
auto ast   = g.parse(stream);                 // default SharedPtrRepr → ValuePtr (today's API, unchanged)
auto arena = g.parse_as<ArenaRepr>(stream);   // → Arena
MyIrBuilder b(my_module);                     // app-defined representation
g.parse_into(stream, b);                      // product lives in my_module
```

### Policy vs mechanism

Which format gets which representation is the APP's one-line-per-call-site
policy; rawast owns only the mechanism (any pair, uniformly). No
format→representation registry in the library.

### Cross-representation conversion is free

`convert = accessorA → builderB` is ONE generic function, not per-pair code:
parse DEF into an arena, materialise a subtree as Python, lower into the
app's IR — all the same pipe.

### Template vs virtual — resolved

- **Virtual `Builder`/`Accessor` classes are the ABI**: `parse_into`/
  `save_from` take them; the driver stays in the .cpp; app representations
  inherit — no template exposure. Parse events are per-value (coarse), so
  virtual cost is noise on the build side.
- **Shipped representations get the fast path**: engine internals templated
  (constrained by the same concepts) and explicitly instantiated INSIDE the
  library — `parse_as<ArenaRepr>` pays zero dispatch without header-exposing
  the driver.
- Watch the save-side `Accessor` (reads are much hotter than build events):
  measure the virtual form on the corpus before deciding app accessors need
  a template path. Don't build that door until someone knocks.

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

**Refinement (2026-07-04, implemented in part):** save's *dispatch* layer
(can_consume / values_equal / discriminators) now borrows `const Value*`
(~3–6% save win; shipped with the cutover branch). The *queue/consume* layer
stays `ValuePtr`-owning on purpose: the opchain re-nest (`rebuild_cascade`)
stores its input, so a borrowed queue would need re-owning anyway. And for
the §0.5 intermediate model the `Accessor` turns out to be UNNECESSARY —
arena-allocated `Value`s can enter save as **non-owning aliasing
`shared_ptr`s** (empty control block → copies skip the atomics, near-free).
The `ArenaValueBuilder` hands the engine those, and the whole save engine
works unchanged. The full `Accessor` is needed only for a representation
that is not the `Value` hierarchy at all (the id-columnar arena / dagland
L0).

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
