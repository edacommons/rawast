#!/usr/bin/env python3
"""SV parse + round-trip coverage over the Ibex corpus — the gate for the
raw-capture-debt sweep. Run from project root: python tools/sv_coverage.py"""
import rawast, glob, sys

BASE = "/Users/serge/verilog2ttl/ibex"
INC = [BASE + "/rtl",
       BASE + "/vendor/lowrisc_ip/ip/prim/rtl",
       BASE + "/vendor/lowrisc_ip/dv/sv/dv_utils",
       BASE + "/vendor/lowrisc_ip/dv/sv/dv_lib",
       BASE + "/vendor/lowrisc_ip/dv/sv/csr_utils"]
sv_pp = rawast.Grammar("sv_preprocessor")
sv = rawast.Grammar("systemverilog")


def measure(label, files, show_fails=0):
    pok = rok = total = 0
    fails = []
    for f in files:
        total += 1
        try:
            src = rawast.Preprocessor(sv_pp, predefined="", include_paths=INC,
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
    sf = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    measure("RTL", sorted(glob.glob(BASE + "/rtl/*.sv")), sf)
    measure("dv/uvm", [f for f in glob.glob(BASE + "/dv/uvm/**/*.sv",
                                            recursive=True) if "tpl" not in f], sf)
