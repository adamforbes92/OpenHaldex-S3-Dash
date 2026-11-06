#!/usr/bin/env python3
"""Turn the DBC files in dbc/ into src/net/dbc_tables.cpp: const tables in flash.

The dash has no way to update a filesystem image over the air, only the app, so
the tables are compiled in rather than loaded from LittleFS. Each signal costs 8
bytes plus its name; message and signal order follow the DBC, messages sorted by
id so the firmware can binary-search them.

Reserved "Free_*" fillers are dropped. Everything else in the DBC is kept.

    python tools/make_dbc_tables.py          regenerate, then check the result

Only the Python standard library is needed. Re-run it whenever dbc/ changes; the
output is committed, so a normal build needs neither Python nor this script.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "net", "dbc_tables.cpp")
TABLES = [("kVwPq", "vw_pq"), ("kVwMqb", "vw_mqb")]

BO_LINE = re.compile(r"^BO_ (\d+) (\w+): (\d+)")
SG_LINE = re.compile(
    r'^ SG_ (\w+)\s*(\S*)\s*: *(\d+)\|(\d+)@(\d)([+-]) \(([^,]+),([^)]+)\) \[[^\]]*\] "([^"]*)"'
)

MUX_PLAIN = 0xFF
MUX_SELECTOR = 0xFE

# The MQB file spells its units out in full, and the glass has room for 9 letters.
UNIT_NAMES = {
    "Unit_DegreCelsi": "C",
    "Unit_KiloMeterPerHour": "km/h",
    "Unit_MinutInver": "rpm",
    "Unit_PerCent": "%",
    "Unit_Bar": "bar",
    "Unit_MilliBar": "mbar",
    "Unit_Volt": "V",
    "Unit_Amper": "A",
    "Unit_MilliSecon": "ms",
    "Unit_Secon": "s",
    "Unit_Minut": "min",
    "Unit_Hours": "h",
    "Unit_Day": "day",
    "Unit_Month": "month",
    "Unit_Meter": "m",
    "Unit_MilliMeter": "mm",
    "Unit_KiloMeter": "km",
    "Unit_Liter": "l",
    "Unit_MilliLiter": "ml",
    "Unit_MicroLiter": "ul",
    "Unit_KiloWatt": "kW",
    "Unit_Hertz": "Hz",
    "Unit_NewtoMeter": "Nm",
    "Unit_DegreOfArc": "deg",
    "Unit_DegreeOfArc": "deg",
    "Unit_DegreOfArcPerSecon": "deg/s",
    "Unit_MeterPerSeconSquar": "m/s2",
    "Unit_ForceOfGravi": "g",
    "Unit_KiloGram": "kg",
    "Unit_GramPerLiter": "g/l",
    "Unit_BarPerSecon": "bar/s",
}


def short_unit(unit):
    unit = UNIT_NAMES.get(unit, unit)
    if unit.startswith("Unit_"):
        unit = unit[5:]
    unit = "".join(c if 32 <= ord(c) < 127 and c not in '"\\' else "?" for c in unit)
    return unit[:9]


def parse_dbc(path):
    """-> [(id, is_extended, name, dlc, [signal, ...])], sorted by id."""
    messages = []
    current = None
    with open(path, encoding="latin1") as dbc:
        for line in dbc:
            bo = BO_LINE.match(line)
            if bo:
                raw = int(bo.group(1))
                current = None
                if bo.group(2) == "VECTOR__INDEPENDENT_SIG_MSG" or (raw & 0x1FFFFFFF) == 0x1FFFFFFF:
                    continue  # the DBC editor's placeholder for unassigned signals
                extended = (raw & 0x80000000) != 0
                dlc = int(bo.group(3))
                if dlc > 8:
                    sys.exit("%s: %s has %d bytes: this decoder is classic CAN only" % (path, bo.group(2), dlc))
                current = (raw & 0x1FFFFFFF, extended, bo.group(2), dlc, [])
                messages.append(current)
                continue
            if current is None:
                continue
            sg = SG_LINE.match(line)
            if sg:
                name, mux_text, start, length, order, sign, factor, offset, unit = sg.groups()
                if name.startswith("Free_"):
                    continue
                if sign != "+":
                    sys.exit("%s: %s is signed: add signed support to dbc.cpp first" % (path, name))
                mux = MUX_PLAIN
                if mux_text == "M":
                    mux = MUX_SELECTOR
                elif re.fullmatch(r"m\d+", mux_text):
                    mux = int(mux_text[1:])
                    if mux >= MUX_SELECTOR:
                        sys.exit("%s: %s has mux value %d, too big for one byte" % (path, name, mux))
                current[4].append(
                    {
                        "name": name,
                        "start": int(start),
                        "length": int(length),
                        "big": order == "0",
                        "factor": float(factor),
                        "offset": float(offset),
                        "unit": short_unit(unit),
                        "mux": mux,
                    }
                )
    messages.sort(key=lambda m: (m[0], m[1]))
    for first, second in zip(messages, messages[1:]):
        if first[0] == second[0]:
            sys.exit("%s: id 0x%X appears twice" % (path, first[0]))
    return messages


class Pool:
    """Shared strings and scales, so a name used twice is only stored once."""

    def __init__(self):
        self.names = {}
        self.names_size = 0
        self.scales = {}
        self.units = {"": 0}

    def name(self, text):
        if text not in self.names:
            self.names[text] = self.names_size
            self.names_size += len(text) + 1
        return self.names[text]

    def unit(self, text):
        return self.units.setdefault(text, len(self.units))

    def scale(self, factor, offset, unit):
        return self.scales.setdefault((factor, offset, self.unit(unit)), len(self.scales))


def c_float(value):
    text = "%.9g" % value
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "f"


def generate():
    pool = Pool()
    tables = []
    for symbol, base in TABLES:
        messages = parse_dbc(os.path.join(ROOT, "dbc", base + ".dbc"))
        tables.append((symbol, base, messages))

    body = []
    for symbol, base, messages in tables:
        signal_rows = []
        message_rows = []
        for ident, extended, name, dlc, signals in messages:
            message_rows.append(
                "    {0x%X, %d, %d, %d, %d, %d},  // %s"
                % (ident, pool.name(name), len(signal_rows), len(signals), dlc, 1 if extended else 0, name)
            )
            for s in signals:
                signal_rows.append(
                    "    {%d, %d, %d, %d, %d, %d},  // %s"
                    % (
                        pool.name(s["name"]),
                        pool.scale(s["factor"], s["offset"], s["unit"]),
                        s["start"],
                        s["length"],
                        1 if s["big"] else 0,
                        s["mux"],
                        s["name"],
                    )
                )
        body.append("const Signal %sSignals[] = {\n%s\n};\n" % (symbol, "\n".join(signal_rows)))
        body.append("const Message %sMessages[] = {\n%s\n};\n" % (symbol, "\n".join(message_rows)))

    if pool.names_size > 65535 or len(pool.scales) > 65535:
        sys.exit("names or scales no longer fit a 16-bit offset")

    out = [
        "// Generated by tools/make_dbc_tables.py from dbc/*.dbc. Do not edit by hand.",
        "// The DBC data comes from comma.ai's opendbc (MIT): see dbc/SOURCE.md.",
        '#include "net/dbc.h"',
        "",
        "namespace dbc {",
        "namespace {",
        "",
        "const char kNames[] =",
    ]
    out.extend('    "%s\\0"' % name for name in pool.names)
    out[-1] += ";"
    out.append("")
    out.append("const char *const kUnits[] = {")
    out.extend('    "%s",' % unit for unit in pool.units)
    out.append("};")
    out.append("")
    out.append("const Scale kScales[] = {")
    out.extend(
        "    {%s, %s, %d}," % (c_float(factor), c_float(offset), unit) for (factor, offset, unit) in pool.scales
    )
    out.append("};")
    out.append("")
    out.extend(body)
    out.append("}  // namespace")
    out.append("")
    for symbol, base, messages in tables:
        out.append(
            'const Table %s = {"%s", %sMessages, %d, %sSignals, kScales, kUnits, kNames};'
            % (symbol, base, symbol, len(messages), symbol)
        )
    out.append("")
    out.append("}  // namespace dbc")
    out.append("")
    with open(OUT, "w", newline="\n", encoding="ascii") as cpp:
        cpp.write("\n".join(out))

    total_signals = sum(len(m[4]) for t in tables for m in t[2])
    total_messages = sum(len(t[2]) for t in tables)
    flash = (
        total_signals * 8
        + total_messages * 12
        + len(pool.scales) * 12
        + pool.names_size
        + sum(len(u) + 1 + 4 for u in pool.units)
    )
    print(
        "%d messages, %d signals, %d scales, %d units, %d bytes of names: about %d KB of flash"
        % (total_messages, total_signals, len(pool.scales), len(pool.units), pool.names_size, flash // 1024)
    )
    return tables


# ------------------------------------------------------------------ the check
# The same arithmetic as dbc.cpp, run against signals whose decode the controller
# already does by hand and has proved on a car. If the tables or the bit maths
# disagree with those, this fails before anything reaches a dashboard.
def decode(signal, data):
    if signal["big"]:
        value = 0
        bit = signal["start"]
        for _ in range(signal["length"]):
            value = (value << 1) | ((data[bit // 8] >> (bit % 8)) & 1)
            bit = bit + 15 if bit % 8 == 0 else bit - 1
    else:
        value = (int.from_bytes(data, "little") >> signal["start"]) & ((1 << signal["length"]) - 1)
    return value * signal["factor"] + signal["offset"]


def check(tables):
    by_table = {base: {m[0]: m for m in messages} for _, base, messages in tables}

    def signal(base, ident, name):
        for s in by_table[base][ident][4]:
            if s["name"] == name:
                return s
        sys.exit("check: %s 0x%X has no signal %s" % (base, ident, name))

    frame = bytes(range(17, 17 + 64, 8))  # eight different bytes
    expected = [
        # table, id, signal, what OpenHaldexC6_can.cpp computes for the same frame
        ("vw_pq", 0x420, "CoolantTemp__4_1__Cluster_2_", frame[4] * 0.75 - 48),
        ("vw_pq", 0x420, "OilTemperature_4_1", frame[3] - 60),
        ("vw_pq", 0x280, None, ((frame[3] << 8) | frame[2]) * 0.25),
        ("vw_mqb", 0x640, "MO_Coolant_Temp", frame[3] * 0.75 - 48),
        ("vw_mqb", 0x640, "MO_Oil_Temp", frame[2] - 60),
        ("vw_mqb", 0x0A8, None, ((frame[7] << 8) | frame[6]) * 0.25),
    ]
    for base, ident, name, want in expected:
        if name is None:  # engine speed: found by position, its name differs per file
            start = 16 if base == "vw_pq" else 48
            match = [s for s in by_table[base][ident][4] if s["start"] == start and s["length"] == 16]
            if len(match) != 1:
                sys.exit("check: %s 0x%X has no 16-bit signal at bit %d" % (base, ident, start))
            s = match[0]
        else:
            s = signal(base, ident, name)
        got = decode(s, frame)
        if abs(got - want) > 0.001:
            sys.exit("check: %s 0x%X %s gave %s, expected %s" % (base, ident, s["name"], got, want))

    for _, base, messages in tables:
        for ident, _, name, dlc, signals in messages:
            for s in signals:
                if not s["big"] and s["start"] + s["length"] > 64:
                    sys.exit("check: %s %s %s runs off the end of the frame" % (base, name, s["name"]))
    print("check: table decode matches the controller's own decode")


if __name__ == "__main__":
    check(generate())
