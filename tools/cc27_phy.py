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
"""

import argparse
import json
import math
import os
import re

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


def merge_features(phy: dict, registers: dict) -> dict:
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
                f"    .txShape = &{struct_name(prefix, s['txShape'])},",
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
    used |= {phy["rcl_structs"][s]["txShape"] for s in used}

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
    args = parser.parse_args()

    path = args.phy
    if not os.path.exists(path):
        path = os.path.join(args.sdk, RADIOCONFIG, "rcl", args.phy + ".json")
    with open(path) as f:
        phy = json.load(f)["phy_def"]
    registers = load_json(args.sdk, "rcl_common", "rcl_registers.json")[args.family]["registers"]
    board = load_json(args.sdk, "rcl_common", f"boards_{args.device}.json")["boards"][args.board]
    prefix = args.prefix or "phy_" + c_name(phy["variant"]["abbreviation"])
    source = os.path.basename(path)

    resolve_board_scripts(phy, args.device.upper())
    store = merge_features(phy, registers)
    with open(os.path.join(args.outdir, prefix + ".c"), "w") as f:
        f.write(gen_c(phy, store, board, prefix, source, args.board))
    with open(os.path.join(args.outdir, prefix + ".h"), "w") as f:
        f.write(gen_h(phy, prefix, source))


if __name__ == "__main__":
    main()
