#!/usr/bin/env python3
"""SV parse + round-trip coverage over the Ibex corpus — the gate for the
raw-capture-debt sweep.

Two modes:
  expand (default) — force-include uvm_macros.svh + dv_macros.svh so `uvm_*`
    / `DV_*` macros EXPAND. This is the honest yardstick: it parses the real
    code the macros generate, not the opaque macro tokens.
  leave            — on_undefined='leave' (macros kept as text). Inflates the
    parse number (a `uvm_component_utils` token is trivially skipped) but is
    useful to isolate non-UVM grammar work.

Run from repo root:  python tools/sv_coverage.py [--leave] [show_fails]
UVM via $UVM_HOME or a couple of known local paths.
"""
import rawast, glob, sys, os

BASE = "/Users/serge/verilog2ttl/ibex"
LOWRISC = BASE + "/vendor/lowrisc_ip"
INC = [BASE + "/rtl",
       LOWRISC + "/ip/prim/rtl",
       LOWRISC + "/dv/sv/dv_utils",
       LOWRISC + "/dv/sv/dv_lib",
       LOWRISC + "/dv/sv/csr_utils"]


def find_uvm():
    if os.environ.get("UVM_HOME"):
        return os.environ["UVM_HOME"]
    for p in ("/Users/serge/src/uvm-verilator/src",
              "/Users/serge/verification_generator/output/verilator/uvm/src/src"):
        if os.path.exists(p + "/uvm_macros.svh"):
            return p
    return None


# Full macro config: the SV grammar no longer tolerates un-expanded `\`MACRO`
# tokens (they belong to the preprocessor), so the harness must define
# everything a real Ibex build supplies. `prim_assert.sv` defines `\`ASSERT`
# etc.; `dv_fcov_macros.svh` defines `\`DV_FCOV_SIGNAL`; and the *_ADDR /
# BOOT_ADDR macros are build-config `+define+`s given representative bare-hex
# values (they appear inside literals, e.g. `32'h\`BOOT_ADDR`).
EXPAND_PREDEF = ('`include "uvm_macros.svh"\n`include "dv_macros.svh"\n'
                 '`include "prim_assert.sv"\n`include "dv_fcov_macros.svh"\n'
                 '`define __FILE__ "f"\n`define __LINE__ 0\n'
                 '`define BOOT_ADDR 80000000\n`define DM_ADDR 1a110000\n'
                 '`define DM_ADDR_MASK fffff000\n'
                 '`define DEBUG_MODE_HALT_ADDR 800\n'
                 '`define DEBUG_MODE_EXCEPTION_ADDR 808\n')

# One merged grammar: the Preprocessor enters at PP_FILE; SV parse uses the
# default start. No separate preprocessor grammar.
sv = rawast.Grammar("systemverilog")


def measure(label, files, expand, show_fails=0):
    inc = list(INC)
    predef = ""
    if expand:
        uvm = find_uvm()
        if uvm:
            inc.append(uvm)
        predef = EXPAND_PREDEF
        # Cross-subtree include dirs a real ibex build puts on -I (cosim DPI,
        # per-agent seq_lib, the `common` dir). With these every dv/uvm file
        # resolves its `includes, so on_missing_include="error" is clean.
        extra = ([BASE + "/dv/cosim"]
                 + glob.glob(BASE + "/dv/uvm/**/seq_lib", recursive=True)
                 + glob.glob(BASE + "/dv/uvm/**/common", recursive=True))
        inc += [d for d in extra if os.path.isdir(d)]
    pok = rok = total = 0
    fails = []
    for f in files:
        total += 1
        try:
            src = rawast.Preprocessor(
                sv, predefined=predef, include_paths=inc,
                on_undefined="leave").process_file(f)   # on_missing_include=error (default)
            a = sv.parse_string(src)
            pok += 1
        except Exception as e:  # noqa: BLE001
            fails.append((f.split("/ibex/")[-1], str(e)[:55])); continue
        try:
            if a == sv.parse_string(sv.save(a).decode()):
                rok += 1
        except Exception:  # noqa: BLE001
            pass
    pct = 100 * pok // total if total else 0
    print(f"{label:10} parse {pok}/{total} ({pct}%)  round-trip {rok}/{total}")
    for sh, why in fails[:show_fails]:
        print(f"    PARSE-FAIL {sh} :: {why}")
    return pok, rok, total


if __name__ == "__main__":
    args = sys.argv[1:]
    expand = "--leave" not in args
    sf = next((int(a) for a in args if a.isdigit()), 0)
    mode = "EXPAND macros" if expand else "LEAVE macros"
    if expand and not find_uvm():
        print("(UVM not found — set $UVM_HOME; falling back to leave-ish)")
    print(f"=== mode: {mode} ===")
    measure("RTL", sorted(glob.glob(BASE + "/rtl/*.sv")), expand, sf)
    measure("dv/uvm", [f for f in glob.glob(BASE + "/dv/uvm/**/*.sv",
                                            recursive=True) if "tpl" not in f],
            expand, sf)
