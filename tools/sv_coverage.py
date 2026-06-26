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


EXPAND_PREDEF = ('`include "uvm_macros.svh"\n`include "dv_macros.svh"\n'
                 # prim_assert.sv's ifdef guards pick the standard macros in a
                 # normal (non-synthesis/non-yosys) build — include them
                 # directly so `ASSERT(...)` expands to real assert property
                 # instead of an opaque, undefined macro-use.
                 '`include "prim_assert_standard_macros.svh"\n'
                 '`define __FILE__ "f"\n`define __LINE__ 0\n')

sv_pp = rawast.Grammar("sv_preprocessor")
sv = rawast.Grammar("systemverilog")


def measure(label, files, expand, show_fails=0):
    inc = list(INC)
    predef = ""
    if expand:
        uvm = find_uvm()
        if uvm:
            inc.append(uvm)
        predef = EXPAND_PREDEF
    pok = rok = total = 0
    fails = []
    for f in files:
        total += 1
        try:
            src = rawast.Preprocessor(sv_pp, predefined=predef, include_paths=inc,
                                      on_undefined="leave").process_file(f)
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
