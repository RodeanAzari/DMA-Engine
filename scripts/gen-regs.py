#!/usr/bin/env python3
"""Generate the C header, SV package and register doc from spec/dmaeng.yaml.

Run from the repo root:  python scripts/gen-regs.py
Output is deterministic (no timestamps) so CI can regenerate and diff.
"""

import os
import yaml

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SPEC = os.path.join(ROOT, "spec", "dmaeng.yaml")
BANNER = "GENERATED - DO NOT EDIT. Source: spec/dmaeng.yaml"


def write(relpath, lines):
    path = os.path.join(ROOT, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # newline="\n": keep LF endings on Windows, matches .gitattributes
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def gen_c(spec):
    o = [f"/* {BANNER} */", "#ifndef DMAENG_REGS_H", "#define DMAENG_REGS_H", ""]

    o.append("/* Constants */")
    for name, val in spec["constants"].items():
        o.append(f"#define DMAENG_{name} 0x{val:X}U")

    o += ["", "/* Register offsets (BAR0) */"]
    for reg in spec["registers"]:
        o.append(f"#define DMAENG_REG_{reg['name']} 0x{reg['offset']:02X}")

    o += ["", "/* Register bit fields */"]
    for reg in spec["registers"]:
        for fld in reg.get("fields", []):
            o.append(f"#define DMAENG_{reg['name']}_{fld['name']} (1U << {fld['bit']})")

    d = spec["descriptor"]
    o += ["", "/* Descriptor field offsets (32 bytes, little-endian) */"]
    for fld in d["fields"]:
        o.append(f"#define DMAENG_DESC_OFF_{fld['name'].upper()} 0x{fld['offset']:02X}")

    o += ["", "/* Descriptor flags */"]
    for flag in d["flags"]:
        o.append(f"#define DMAENG_DESC_{flag['name']} (1U << {flag['bit']})")

    o += ["", "/* Error codes */"]
    for err in spec["error_codes"]:
        o.append(f"#define DMAENG_ERR_{err['name']} 0x{err['value']:X}")

    o += ["", "#endif /* DMAENG_REGS_H */"]
    write("include/dmaeng_regs.h", o)


def gen_sv(spec):
    o = [f"// {BANNER}", "package dmaeng_pkg;", ""]

    for name, val in spec["constants"].items():
        o.append(f"  localparam int unsigned {name} = 32'h{val:X};")

    o.append("")
    for reg in spec["registers"]:
        o.append(f"  localparam logic [11:0] REG_{reg['name']} = 12'h{reg['offset']:03X};")

    o.append("")
    for reg in spec["registers"]:
        for fld in reg.get("fields", []):
            o.append(f"  localparam int {reg['name']}_{fld['name']}_BIT = {fld['bit']};")

    o.append("")
    for flag in spec["descriptor"]["flags"]:
        o.append(f"  localparam int DESC_{flag['name']}_BIT = {flag['bit']};")

    o.append("")
    for err in spec["error_codes"]:
        o.append(f"  localparam logic [3:0] ERR_{err['name']} = 4'h{err['value']:X};")

    o += ["", "endpackage"]
    write("rtl/dmaeng_pkg.sv", o)


def gen_md(spec):
    o = [f"<!-- {BANNER} -->", "# Register map (BAR0)", "",
         "| Offset | Name | Access | Reset | Description |",
         "|---|---|---|---|---|"]
    for reg in spec["registers"]:
        o.append(f"| `0x{reg['offset']:02X}` | `{reg['name']}` | {reg['access']} "
                 f"| `0x{reg['reset']:X}` | {reg['desc']} |")

    o += ["", "## Register fields", "",
          "| Register | Bit | Name | Description |", "|---|---|---|---|"]
    for reg in spec["registers"]:
        for fld in reg.get("fields", []):
            o.append(f"| `{reg['name']}` | {fld['bit']} | `{fld['name']}` | {fld['desc']} |")

    d = spec["descriptor"]
    o += ["", f"## Descriptor ({d['size']} bytes, little-endian)", "",
          "| Offset | Name | Width (bytes) | Description |", "|---|---|---|---|"]
    for fld in d["fields"]:
        o.append(f"| `0x{fld['offset']:02X}` | `{fld['name']}` | {fld['width']} | {fld['desc']} |")

    o += ["", "### Descriptor flags", "",
          "| Bit | Name | Description |", "|---|---|---|"]
    for flag in d["flags"]:
        o.append(f"| {flag['bit']} | `{flag['name']}` | {flag['desc']} |")

    o += ["", "## Error codes", "",
          "| Code | Name | Description |", "|---|---|---|"]
    for err in spec["error_codes"]:
        o.append(f"| `0x{err['value']:X}` | `{err['name']}` | {err['desc']} |")

    write("docs/register-map.md", o)


def main():
    with open(SPEC) as f:
        spec = yaml.safe_load(f)
    gen_c(spec)
    gen_sv(spec)
    gen_md(spec)


if __name__ == "__main__":
    main()
