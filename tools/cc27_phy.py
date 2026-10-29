#!/usr/bin/env python3
"""Generate a CC27xx PHY setting (C source) from a TI radio configuration JSON.

The inputs are SysConfig's: the PHY definitions of the SimpleLink Low Power
F3 SDK (source/ti/devices/radioconfig/.meta/config/rcl/*_cc27xx.json), the
register database (config/rcl_common/rcl_registers.json) and the board's PA
table (config/rcl_common/boards_<device>.json). The output is what SysConfig
would generate for the LRF layer (ti/drivers/rcl/LRF.h), done more simply:
one LRF_Config per sub-PHY (feature), its register list a single segment with
every register the PHY sets (the common values merged in, as TI's generator
does in rcl_phy_handler.js, createRegisterData()), applied whole by
LRF_setupRadio(). The segment holds:

- the swParam block (Par_Write_32bit): swConfig, no TX power limit table,
  the TX power table, the FCFG's application trims,
- the hardware registers (HW_Write_16bit_sparse, offsets from 0x40080000),
- the cores' RAM variables (SW_Write_16bit_sparse, offsets from 0x40090000:
  the rcl_registers.json addresses less 0x10000),
The trim fields are written 0: LRF_setupRadio() ORs the FCFG's trims into
them after the segment (LRF_applyTrim()), as with TI's output.

--from cc23xx ports a CC23xx PHY (TI has proprietary ones there only, same
LRF radio): its fields by name into the CC27xx registers (a field moved to
another register of the module follows: DEMDSBU's DSBUAVGLENGTH is in
DEMDSBU2), then adapted as TI's NESB PHY of both families differ
(port_adapt()).

--gfsk: TX as GFSK instead of the PHY's own shaping (TI's MSK 250 kbps PHY:
fixed DTX taps for MOD0.SYMSHP 2, a mapping of its own, received by FSK
receivers as rubbish): the NESB PHY's BT 0.5 shape (the LRF computes the
taps for the deviation), MOD0.SYMSHP 0, the NESB PHY's MODSYMMAP0.
"""

import argparse
import json
import math
import os
import re
import sys

DEFAULT_SDK = os.path.expanduser("~/Src/simplelink-lowpower-f3-sdk")
RADIOCONFIG = "source/ti/devices/radioconfig/.meta/config"

# LRF_RegionOperation (LRF.h)
HW_WRITE_16BIT_SPARSE = 3
SW_WRITE_16BIT_SPARSE = 7
PAR_WRITE_32BIT = 10

LRF_RAM_OFFSET = 0x10000  # PBE_RAM_BASE_ADDR - LRF_BASE_ADDR
MAX_REG_CONFIG_LEN = 1024


def force_list(x):
    return x if isinstance(x, list) else [x]


def c_name(s: str) -> str:
    return re.sub(r"[^0-9a-zA-Z_]", "_", s)


def struct_name(prefix: str, name: str) -> str:
    """C name of a PHY struct (LRF_swConfig1Mbps -> <prefix>_swConfig1Mbps)"""
    return f"{prefix}_{name.removeprefix('LRF_')}"


def load_json(sdk: str, *path: str):
    with open(os.path.join(sdk, RADIOCONFIG, *path)) as f:
        return json.load(f)


BOARD_SCRIPT = re.compile(
    r'^\(String\("getBoardRfDesignNames\(\)"\)\.search\(/(.+?)/\) == -1\) \? (.+?) : (.+)$'
)


def board_script(text: str, device: str):
    """The PHY scripts that depend on the board only (TI's
    `(String("getBoardRfDesignNames()").search(/RE/) == -1) ? A : B`), for
    device (e.g. CC2755P10); None for the others"""
    m = BOARD_SCRIPT.match(text.strip())
    if not m:
        return None
    value = m.group(3) if re.search(m.group(1), device) else m.group(2)
    return value.strip('"')


def resolve_board_scripts(phy: dict, device: str) -> None:
    """Apply the board-dependent scripts of a PHY in place: struct fields
    (rcl_structs) and the register fields TI updates (rcl_register_field, as
    updateRegisterData() in rcl_phy_handler.js); other scripts keep the
    static values"""
    for struct in phy["rcl_structs"].values():
        for key, field in struct.items():
            if isinstance(field, dict) and "script" in field:
                value = board_script(field["script"]["#text"], device)
                if value is not None:  # numCoeff: gen_structs() counts the coefficients
                    struct[key] = value
    for item in force_list(phy["rcl_registers"]):
        updates = {}
        for field in force_list(item.get("rcl_register_field", [])):
            value = field.get("value")
            if "script" in field:
                value = board_script(field["script"]["#text"], device)
            if value is None:
                continue
            for feature in field.get("feature_filter", "common").split(","):
                updates.setdefault(field["name"].upper(), {})[feature] = str(value)
        for setting in item["rcl_register_field_settings"]:
            for mod in setting["data"]:
                for path, field_data in mod["reg"].items():
                    update = updates.get(path.upper())
                    if update is None:
                        continue
                    feature = setting["feature_filter"]
                    field_data["value"] = update.get(feature, update.get("common", field_data["value"]))


def resolve_field(registers: dict, reg_path: str, field_name: str):
    """(reg_path, register, field) for a field of another family's
    register: the same register, else the only register of the module with
    a field of that name; None if there is none"""
    ri = registers.get(reg_path)
    if ri is not None and field_name in ri:
        return reg_path, ri, ri[field_name]
    mod = reg_path.split(".")[0] + "."
    found = [(k, r) for k, r in registers.items() if k.startswith(mod) and field_name in r]
    if len(found) != 1:
        return None
    return found[0][0], found[0][1], found[0][1][field_name]


def merge_features(phy: dict, registers: dict, ported: bool = False, quiet: bool = False) -> dict:
    """Register values per feature: {feature: {reg: [addr, value, mask]}}.

    "common" holds the registers of all features; a feature's own fields
    override the common values of the same register (the other fields of that
    register keep their common values). The trim fields stay 0.
    """
    store: dict = {}
    for item in force_list(phy["rcl_registers"]):
        for setting, feature in (
            (s, f) for s in item["rcl_register_field_settings"] for f in s["feature_filter"].split(",")
        ):
            regs = store.setdefault(feature, {})
            for mod in setting["data"]:
                for path, field_data in mod["reg"].items():
                    if field_data["value"] == "":
                        continue
                    mod_name, reg_name, field_name = path.upper().split(".")
                    reg_path = f"{mod_name}.{reg_name}"
                    if ported:
                        found = resolve_field(registers, reg_path, field_name)
                        if found is None:
                            if not quiet:
                                print(f"warning: {path}: no such field here, left out", file=sys.stderr)
                            continue
                        reg_path, ri, fi = found
                    else:
                        ri = registers[reg_path]
                        fi = ri[field_name]
                    pos, width = int(fi["pos"]), int(fi["width"])
                    mask = ((1 << width) - 1) << pos
                    value = (int(field_data["value"], 0) << pos) & mask
                    if reg_path not in regs:
                        common = store.get("common", {}).get(reg_path)
                        regs[reg_path] = [int(ri["addr"], 16), 0]
                        if common is not None and feature != "common":
                            regs[reg_path][1] = common[1]
                    if fi["type"] == "trim":
                        continue
                    reg = regs[reg_path]
                    reg[1] = (reg[1] & ~mask) | value
    return store


# The reference pair for --from: TI's NESB PHY of both families
PORT_REFERENCE = "nesb_1m_2m"
PORT_REFERENCE_FEATURE = "1_mbps"
# Differences by an offset: the RSSI offset (CC27xx: 1 dB more)
PORT_OFFSET_REGS = {"RFE_COMMON_RAM.PHYRSSIOFFSET"}


def load_phy(sdk: str, name: str) -> dict:
    path = name if os.path.exists(name) else os.path.join(sdk, RADIOCONFIG, "rcl", name + ".json")
    with open(path) as f:
        return json.load(f)["phy_def"]


def reference_table(sdk: str, family: str, device: str, registers: dict, ported: bool) -> dict:
    phy = load_phy(sdk, f"{PORT_REFERENCE}_{family}")
    resolve_board_scripts(phy, device)
    store = merge_features(phy, registers, ported, quiet=True)
    return {k: v[1] for k, v in {**store["common"], **store[PORT_REFERENCE_FEATURE]}.items()}


def port_adapt(store: dict, features: list, ref_from: dict, ref_to: dict, registers: dict) -> dict:
    """Adapt a ported PHY (store, in CC27xx registers) as TI's reference PHY
    differs between the families (ref_from: the CC23xx one in CC27xx
    registers, ref_to: the CC27xx one); returns each sub-PHY's whole table
    (as the store, without "common"):

    - the registers only the CC27xx reference sets (BAUDCOMP, DEMD2XB0,
      MODCTRL, MODPRECTRL, PATRIM...) are added, its values;
    - the registers the PHY sets as the CC23xx reference has them take the
      CC27xx reference's value (the analog ones: LNA, MISC0, PA0);
    - the RSSI offset moves as the reference's;
    - DEMMISC2.PDIFIQCONJEN set: I and Q swapped on CC27xx (RCL-489, every
      CC27xx PHY sets it).
    """
    out = {}
    for feature in features:
        regs = {k: list(v) for k, v in {**store.get("common", {}), **store.get(feature, {})}.items()}
        out[feature] = regs
        for reg_path, value in ref_to.items():
            addr = int(registers[reg_path]["addr"], 16)
            cur = regs.get(reg_path)
            if reg_path in PORT_OFFSET_REGS and cur is not None and reg_path in ref_from:
                cur[1] = (cur[1] + value - ref_from[reg_path]) & 0xFFFF
            elif reg_path not in ref_from:
                regs.setdefault(reg_path, [addr, value])
            elif cur is not None and cur[1] == ref_from[reg_path]:
                cur[1] = value
        demmisc2 = regs.setdefault("LRFDMDM.DEMMISC2", [int(registers["LRFDMDM.DEMMISC2"]["addr"], 16), 0])
        fi = registers["LRFDMDM.DEMMISC2"]["PDIFIQCONJEN"]
        demmisc2[1] |= 1 << int(fi["pos"])
    return out


GFSK_SHAPE = "LRF_shapeBaseGfsk05"


def make_gfsk(phy: dict, store: dict, sdk: str, family: str, device: str, registers: dict) -> None:
    """--gfsk: the TX shape, MOD0.SYMSHP and MODSYMMAP0 of the reference
    NESB PHY (of the target family) in every swConfig and sub-PHY"""
    ref = load_phy(sdk, f"{PORT_REFERENCE}_{family}")
    phy["rcl_structs"][GFSK_SHAPE] = ref["rcl_structs"][GFSK_SHAPE]
    for struct in phy["rcl_structs"].values():
        if struct["attr"]["type"] == "LRF_SwConfig":
            struct["txShape"] = GFSK_SHAPE
    ref_regs = reference_table(sdk, family, device, registers, False)
    symshp = registers["LRFDRFE.MOD0"]["SYMSHP"]
    symshp_mask = ((1 << int(symshp["width"])) - 1) << int(symshp["pos"])
    for regs in store.values():
        if "LRFDRFE.MOD0" in regs:
            regs["LRFDRFE.MOD0"][1] &= ~symshp_mask
        addr = int(registers["LRFDMDM.MODSYMMAP0"]["addr"], 16)
        regs["LRFDMDM.MODSYMMAP0"] = [addr, ref_regs["LRFDMDM.MODSYMMAP0"]]


def features_of(phy: dict) -> list:
    """The sub-PHYs: the first feature group (BLE's coded TX rate, the second
    group, is left out)"""
    groups = force_list(phy.get("feature_group", []))
    return [f["name"] for f in force_list(groups[0]["feature"])] if groups else ["common"]


def full_table(store: dict, feature: str) -> list:
    regs = dict(store.get("common", {}))
    regs.update(store.get(feature, {}))
    return sorted((v[0], v[1], k) for k, v in regs.items())


def sw_param(phy: dict, feature: str) -> dict:
    fields = {}
    for item in force_list(phy["rcl_registers"]):
        for struct in force_list(item.get("rcl_struct", [])):
            filters = struct.get("feature_filter", feature).split(",")
            if struct["name"] == "LRF_SwParam" and (feature in filters or "common" in filters):
                fields.update({f["name"]: f["value"] for f in struct["rcl_struct_field"]})
    return fields


def shape_ref(prefix: str, name: str) -> str:
    """A swConfig's txShape: "0" for none (the DTX taps of the setup)"""
    return "NULL" if name == "0" else f"&{struct_name(prefix, name)}"


def gen_structs(phy: dict, prefix: str, used: set) -> list:
    """TX shapes and swConfigs of the PHY (rcl_structs), as named in it"""
    out = []
    structs = phy["rcl_structs"]
    for name, s in structs.items():
        kind = s["attr"]["type"]
        if kind == "LRF_TxShape" and name in used:
            coeff = s["coeff"]
            n = len(coeff.split(","))
            out += [
                f"static const LRF_TxShape {struct_name(prefix, name)} = {{",
                f"    .scale = {s['scale']}, .numCoeff = {n},",
                f"    .coeff = {{{coeff}}},",
                "};",
                "",
            ]
    for name, s in structs.items():
        if s["attr"]["type"] == "LRF_SwConfig" and name in used:
            out += [
                f"static const LRF_SwConfig {struct_name(prefix, name)} = {{",
                f"    .rxIntFrequency = {s['rxIntFrequency']},",
                f"    .rxFrequencyOffset = {s['rxFrequencyOffset']},",
                f"    .txFrequencyOffset = {s['txFrequencyOffset']},",
                f"    .modFrequencyDeviation = {s['modFrequencyDeviation']},",
                f"    .txShape = {shape_ref(prefix, s['txShape'])},",
                f"    .bwIndex = {s['bwIndex']},",
                f"    .bwIndexDither = {s['bwIndexDither']},",
                "};",
                "",
            ]
    return out


def gen_pa_table(board: dict, prefix: str) -> list:
    """The board's PA table, low to high power (convPaTableSettingToString(),
    srfs8_util_cc27xx.js)"""
    entries = board["freq_range"][0]["pa_table_entry"]
    rows = []
    for e in sorted(entries, key=lambda e: float(e["tx_power"])):
        half_db = math.ceil(float(e["tx_power"]) * 2)
        v = int(e["value"], 16)
        rows.append(
            f"        {{ .power = {{ .fraction = {half_db & 1}, .dBm = {half_db >> 1} }},"
            f" .tempCoeff = {(v >> 16) & 0xFF},"
            f" .value = {{ .ibBoost = {v & 3}, .ib = {(v >> 2) & 0x3F}, .gain = {(v >> 8) & 7},"
            f" .mode = {(v >> 11) & 3}, .rtrimTxCompCtl = 0, .pa20dBmEsdCtl = 0,"
            f" .noIfampRfLdoBypass = {(v >> 15) & 1} }} }},  // {e['tx_power']} dBm")
    return [
        f"// {board['description']} ({board['rf_design']})",
        f"static const LRF_TxPowerTable {prefix}_txPowerTable = {{",
        f"    .numEntries = {len(rows)},",
        "    .powerTable = {",
        *rows,
        "    },",
        "};",
        "",
    ]


def gen_segment(table: list, swp: dict, prefix: str, name: str) -> list:
    hw = [r for r in table if r[0] < LRF_RAM_OFFSET]
    sw = [r for r in table if r[0] >= LRF_RAM_OFFSET]

    body = []

    def region(kind: int, start: int, count: int, comment: str) -> None:
        body.append(f"    0x{(start << 16) | (kind << 12) | (count - 1):08X},  // {comment}")

    region(PAR_WRITE_32BIT, 0, 4, "swParam: swConfig, txPowerLimitTable, txPowerTable, trimDef")
    body.append(f"    (uint32_t)&{struct_name(prefix, swp['swConfig'])},")
    body.append("    0,")
    body.append(f"    (uint32_t)&{prefix}_txPowerTable,")
    body.append("    (uint32_t)&((const fcfg_t *)FCFG_BASE)->appTrims,")

    region(HW_WRITE_16BIT_SPARSE, 0, len(hw), "hardware registers")
    for addr, value, reg in hw:
        body.append(f"    0x{addr:04X}{value:04X},  // {reg}")

    region(SW_WRITE_16BIT_SPARSE, 0, len(sw), "core RAM")
    for addr, value, reg in sw:
        body.append(f"    0x{addr - LRF_RAM_OFFSET:04X}{value:04X},  // {reg}")


    length = sum(1 for line in body if line.lstrip().startswith(("0x", "0,", "(uint32_t)")))
    if length >= MAX_REG_CONFIG_LEN:
        raise SystemExit(f"{name}: {length} words, too long")
    return [
        f"static const uint32_t {name}[] = {{",
        f"    0x{length:08X},  // segment: {length} words, every feature",
        *body,
        "};",
        "",
    ]


def image_ref(image: dict) -> dict:
    """The image a PHY loads: its "variable" and "header", as SysConfig's
    default (header) mode ("data" can name another file: NESB's MCE is
    ble5_phac there, the plain ble5 image in TI's output)
    """
    return {"variable": image["variable"], "header": image["header"]}


def gen_c(phy: dict, store: dict, board: dict, prefix: str, source: str, board_name: str) -> str:
    images = {i["role"]: image_ref(i) for i in force_list(phy["rcl_lrf_fw_image"])}
    features = features_of(phy)
    swps = {f: sw_param(phy, f) for f in features}
    used = {p["swConfig"] for p in swps.values()}
    used |= {phy["rcl_structs"][s]["txShape"] for s in used} - {"0"}

    out = [
        f"// Generated by tools/cc27_phy.py from {source} and the {board_name} PA",
        "// table (TI SimpleLink Low Power F3 SDK): do not edit",
        "",
        "#include <ti/devices/DeviceFamily.h>",
        "#include DeviceFamily_constructPath(inc/hw_fcfg.h)",
        "#include DeviceFamily_constructPath(inc/hw_memmap.h)",
        "#include <ti/drivers/rcl/LRF.h>",
        "",
    ]
    for image in images.values():
        out.append(f"#include DeviceFamily_constructPath({image['header']})")
    out += ["", f'#include "{prefix}.h"', ""]
    out += gen_structs(phy, prefix, used)
    out += gen_pa_table(board, prefix)

    for feature in features:
        fname = f"{prefix}_{c_name(feature)}"
        out += gen_segment(full_table(store, feature), swps[feature], prefix, f"{fname}_regs")
        out += [
            f"static const LRF_RegConfigList {fname}_regConfigList = {{",
            "    .numEntries = 1,",
            f"    .entries = {{(LRF_ConfigWord *){fname}_regs}},",
            "};",
            "",
            f"const LRF_Config {fname} = {{",
        ]
        for role in ("pbe", "mce", "rfe"):
            image = images.get(f"{role}_image")
            value = f"(const LRF_TOPsmImage *){image['variable']}" if image else "NULL"
            out.append(f"    .{role}Image = {value},")
        out += [f"    .regConfigList = &{fname}_regConfigList,", "};", ""]
    return "\n".join(out)


def gen_h(phy: dict, prefix: str, source: str) -> str:
    out = [
        f"// Generated by tools/cc27_phy.py from {source}: do not edit",
        "",
        "#pragma once",
        "",
        "#include <ti/drivers/rcl/LRF.h>",
        "",
    ]
    for feature in features_of(phy):
        out.append(f"extern const LRF_Config {prefix}_{c_name(feature)};")
    return "\n".join(out) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("phy", help="PHY JSON name (e.g. nesb_1m_2m_cc27xx) or path")
    parser.add_argument("--sdk", default=DEFAULT_SDK)
    parser.add_argument("--family", default="cc27xx")
    parser.add_argument("--device", default="cc2755p10", help="for the board PA tables")
    parser.add_argument("--board", default="LP_EM_CC2755P10")
    parser.add_argument("--prefix", help="C symbol and file prefix (default: phy_<abbreviation>)")
    parser.add_argument("--outdir", default=".", help="where <prefix>.c and <prefix>.h go")
    parser.add_argument("--from", dest="from_family", help="the PHY's family, to port it (cc23xx)")
    parser.add_argument("--gfsk", action="store_true", help="TX as GFSK (BT 0.5, the NESB shape)")
    args = parser.parse_args()

    phy = load_phy(args.sdk, args.phy)
    all_registers = load_json(args.sdk, "rcl_common", "rcl_registers.json")
    registers = all_registers[args.family]["registers"]
    board = load_json(args.sdk, "rcl_common", f"boards_{args.device}.json")["boards"][args.board]
    prefix = args.prefix or "phy_" + c_name(phy["variant"]["abbreviation"])
    source = os.path.basename(args.phy if args.phy.endswith(".json") else args.phy + ".json")

    resolve_board_scripts(phy, args.device.upper())
    store = merge_features(phy, registers, ported=args.from_family is not None)
    if args.from_family:
        ref_from = reference_table(args.sdk, args.from_family, args.device.upper(), registers, True)
        ref_to = reference_table(args.sdk, args.family, args.device.upper(), registers, False)
        store = port_adapt(store, features_of(phy), ref_from, ref_to, registers)
        source += f" (ported from {args.from_family})"
    if args.gfsk:
        make_gfsk(phy, store, args.sdk, args.family, args.device.upper(), registers)
        source += ", GFSK"
    with open(os.path.join(args.outdir, prefix + ".c"), "w") as f:
        f.write(gen_c(phy, store, board, prefix, source, args.board))
    with open(os.path.join(args.outdir, prefix + ".h"), "w") as f:
        f.write(gen_h(phy, prefix, source))


if __name__ == "__main__":
    main()
