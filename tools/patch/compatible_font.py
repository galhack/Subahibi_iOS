"""Build a local epilogue font while preserving every base-font mapping.

Requires fontTools. No game or font data is included in this module. The
returned extra_replacements describe equal-length changes in SCN strings;
this function creates only the derived font and never edits the scenarios.
"""

from __future__ import annotations

from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import tempfile

from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.ttLib import TTFont


_MAGIC = b"BurikoCompiledScriptVer1.00\0"
_ONE_OPERAND = {0, 1, 2, 3, 8, 9, 10, 0x19, 0x3F}
_CONFLICT_CODEPOINT = 0x4E29
_CONFLICT_BYTES = b"\x81\x4c"
_POSTSCRIPT_NAME = "SubahibiAfterstoryCHS"


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def scenario_strings(path: str | Path) -> list[tuple[int, bytes]] | None:
    """Return SCN_STR references as (absolute file offset, raw text).

    Non-scenario files return None. Code operands and source-file markers
    are excluded, so an integer containing 81 4C is never patched as text.
    """
    path = Path(path)
    with path.open("rb") as stream:
        if stream.read(len(_MAGIC)) != _MAGIC:
            return None
        stream.seek(0)
        data = stream.read()
    if len(data) < 0x24:
        raise ValueError(f"{path}: truncated scenario header")

    def word(offset: int) -> int:
        return struct.unpack_from("<I", data, offset)[0]

    base = 0x1C + word(0x1C)
    if base < 0x24 or base > len(data) or base % 4:
        raise ValueError(f"{path}: invalid scenario header size")
    position = base
    string_start = len(data) - base
    references = []
    while position - base < string_start:
        if position + 4 > len(data):
            raise ValueError(f"{path}: truncated scenario token")
        token = word(position)
        operands = 1 if token in _ONE_OPERAND else 2 if token == 0x7F else 3 if token == 0x7B else 0
        next_position = position + 4 + operands * 4
        if next_position > len(data):
            raise ValueError(f"{path}: truncated scenario operands")
        if token in (3, 0x7F, 0x7B):
            relative = word(position + (12 if token == 0x7B else 4))
            if relative >= len(data) - base:
                raise ValueError(f"{path}: scenario string offset outside file")
            string_start = min(string_start, relative)
            if token == 3:
                references.append(base + relative)
        position = next_position
    if position - base != string_start:
        raise ValueError(f"{path}: code does not end at the string table")
    result = []
    for offset in references:
        if offset < position:
            raise ValueError(f"{path}: scenario string points into code")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError(f"{path}: unterminated scenario string")
        result.append((offset, data[offset:end]))
    return result


def _text_offsets(text: bytes, target: bytes):
    """Find a two-byte code only at the engine's Shift-JIS boundaries."""
    offset = 0
    while offset < len(text):
        lead = text[offset]
        wide = (0x81 <= lead <= 0x9F or 0xE0 <= lead <= 0xFC) and offset + 1 < len(text)
        if wide and text[offset : offset + 2] == target:
            yield offset
        offset += 2 if wide else 1


def _scan_scenarios(directories):
    used_pairs = set()
    extra = []
    scenarios = references = 0
    visited = set()
    for directory in directories:
        for path in sorted(Path(directory).rglob("*")):
            if not path.is_file() or path.resolve() in visited:
                continue
            visited.add(path.resolve())
            strings = scenario_strings(path)
            if strings is None:
                continue
            scenarios += 1
            references += len(strings)
            offsets = set()
            for file_offset, text in strings:
                # Also exclude cross-character pairs when selecting a new
                # code: the reservation is stricter than a display lookup.
                used_pairs.update((text[i] << 8) | text[i + 1] for i in range(len(text) - 1))
                if path.name.lower() == "extra":
                    offsets.update(file_offset + i for i in _text_offsets(text, _CONFLICT_BYTES))
            if offsets:
                extra.append({"file": str(path), "offsets": sorted(offsets), "source_sha256": _sha(path)})
    return used_pairs, extra, scenarios, references


def _reserve_code(occupied, used_pairs):
    # Search the patch's compatible DBCS region first, then other GBK codes
    # whose lead byte the engine treats as a two-byte Shift-JIS character.
    leads = list(range(0x99, 0xA0)) + list(range(0xE0, 0xFD)) + list(range(0x81, 0x99))
    for lead in leads:
        for trail in range(0x40, 0xFF):
            raw = bytes((lead, trail))
            if (lead << 8 | trail) in used_pairs:
                continue
            try:
                character = raw.decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(character) == 1 and ord(character) not in occupied:
                return raw, ord(character)
    raise ValueError("No unused Shift-JIS-compatible GBK code is available")


def _outline(font, glyph_name):
    glyphs = font.getGlyphSet()
    pen = DecomposingRecordingPen(glyphs)
    glyphs[glyph_name].draw(pen)
    return hashlib.sha256(repr(pen.value).encode("ascii")).hexdigest()


def _layout_metrics(font):
    fields = {
        "head": ("unitsPerEm", "xMin", "yMin", "xMax", "yMax"),
        "OS/2": ("usWinAscent", "usWinDescent", "sTypoAscender", "sTypoDescender", "sTypoLineGap", "fsType"),
        "hhea": ("ascent", "descent", "lineGap", "advanceWidthMax", "minLeftSideBearing", "minRightSideBearing", "xMaxExtent"),
    }
    return {tag: {key: getattr(font[tag], key) for key in keys} for tag, keys in fields.items() if tag in font}


def build_font(base_font, epilogue_font, scenario_dirs, output):
    """Create an epilogue-compatible TTF and return its validation record.

    The base font's cmap, glyph order, outlines and metrics are preserved.
    Epilogue-only mappings are appended. The epilogue's conflicting U+4E29
    glyph receives a new unused DBCS code; only extra's three text locations
    need to be changed to that code by the caller. Both inputs are read only.
    """
    base_path, epilogue_path, output = map(Path, (base_font, epilogue_font, output))
    if output.resolve() in (base_path.resolve(), epilogue_path.resolve()):
        raise ValueError("The derived font must not overwrite an input font")
    base_sha, epilogue_sha = _sha(base_path), _sha(epilogue_path)
    base = TTFont(base_path, recalcTimestamp=False, recalcBBoxes=False)
    epilogue = TTFont(epilogue_path, recalcTimestamp=False, recalcBBoxes=False)
    temporary = None
    try:
        if any("glyf" not in font or "gvar" in font for font in (base, epilogue)):
            raise ValueError("Compatibility font generation requires static TrueType outlines")
        if base["head"].unitsPerEm != epilogue["head"].unitsPerEm:
            raise ValueError("Base and epilogue font units must match")
        for tag in ("fpgm", "prep", "cvt "):
            first = base.getTableData(tag) if tag in base else None
            second = epilogue.getTableData(tag) if tag in epilogue else None
            if first != second:
                raise ValueError(f"Font hinting table {tag!r} differs; cannot safely copy glyph programs")
        if "vmtx" in base and "vmtx" not in epilogue:
            raise ValueError("The epilogue font lacks required vertical metrics")
        old_cmap, new_cmap = base.getBestCmap().copy(), epilogue.getBestCmap()
        if _CONFLICT_CODEPOINT not in new_cmap:
            raise ValueError("The epilogue font lacks the expected U+4E29 mapping")
        additions = sorted(set(new_cmap) - set(old_cmap))
        used, extra, scenarios, references = _scan_scenarios(scenario_dirs)
        raw_code, codepoint = _reserve_code(set(old_cmap) | set(new_cmap), used)
        original_order = base.getGlyphOrder().copy()
        original_metrics = _layout_metrics(base)
        old_records = {
            cp: (_outline(base, name), base["hmtx"][name], base["vmtx"][name] if "vmtx" in base else None)
            for cp, name in old_cmap.items()
        }
        order, copied, visiting = original_order.copy(), {}, set()

        def copy_glyph(source):
            if source in visiting:
                raise ValueError(f"Cyclic composite glyph {source}")
            if source in copied:
                return copied[source]
            target = "afterstory_" + source
            suffix = 0
            while target in base["glyf"].glyphs:
                suffix += 1
                target = f"afterstory_{source}_{suffix}"
            visiting.add(source)
            glyph = deepcopy(epilogue["glyf"][source])
            if glyph.isComposite():
                for component in glyph.components:
                    component.glyphName = copy_glyph(component.glyphName)
            visiting.remove(source)
            base["glyf"][target] = glyph
            base["hmtx"][target] = deepcopy(epilogue["hmtx"][source])
            if "vmtx" in base:
                base["vmtx"][target] = deepcopy(epilogue["vmtx"][source])
            order.append(target)
            copied[source] = target
            return target

        mappings = {cp: copy_glyph(new_cmap[cp]) for cp in additions}
        mappings[codepoint] = copy_glyph(new_cmap[_CONFLICT_CODEPOINT])
        for table in base["cmap"].tables:
            if table.isUnicode() and table.format != 14:
                table.cmap.update(mappings)
        base.setGlyphOrder(order)
        for record in base["name"].names:
            if record.nameID in (1, 3, 4, 6):
                record.string = _POSTSCRIPT_NAME.encode(record.getEncoding())
        for name_id in (1, 3, 4, 6):
            base["name"].setName(_POSTSCRIPT_NAME, name_id, 3, 1, 0x409)
            base["name"].setName(_POSTSCRIPT_NAME, name_id, 1, 0, 0)
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
        base.save(temporary)
        with TTFont(temporary, recalcTimestamp=False) as result:
            cmap = result.getBestCmap()
            for cp, (outline, horizontal, vertical) in old_records.items():
                if cp not in cmap or _outline(result, cmap[cp]) != outline or result["hmtx"][cmap[cp]] != horizontal:
                    raise ValueError(f"Base glyph U+{cp:04X} changed during font generation")
                if vertical is not None and result["vmtx"][cmap[cp]] != vertical:
                    raise ValueError(f"Base vertical metrics U+{cp:04X} changed")
            if _layout_metrics(result) != original_metrics:
                raise ValueError("Base layout metrics changed during font generation")
            for target_cp, source_cp in [(cp, cp) for cp in additions] + [(codepoint, _CONFLICT_CODEPOINT)]:
                if (_outline(result, cmap[target_cp]) != _outline(epilogue, new_cmap[source_cp]) or
                        result["hmtx"][cmap[target_cp]] != epilogue["hmtx"][new_cmap[source_cp]]):
                    raise ValueError(f"Epilogue glyph U+{source_cp:04X} was not copied correctly")
            cmap_count, glyph_count = len(cmap), result["maxp"].numGlyphs
        if _sha(base_path) != base_sha or _sha(epilogue_path) != epilogue_sha:
            raise ValueError("An input font changed during generation")
        temporary.replace(output)
        temporary = None
        return {
            "font_sha256": _sha(output), "font_bytes": output.stat().st_size,
            "postscript_name": _POSTSCRIPT_NAME,
            "base_font_sha256": base_sha, "epilogue_font_sha256": epilogue_sha,
            "base_cmap_count": len(old_cmap), "compatible_cmap_count": cmap_count,
            "compatible_glyph_count": glyph_count, "added_cmap_entries": len(additions),
            "added_glyph_records": len(order) - len(original_order),
            "shadow_code": {"gbk_hex": raw_code.hex(), "unicode": f"U+{codepoint:04X}"},
            "extra_replacements": extra,
            "validation": {
                "old_outline_changes": 0, "old_horizontal_metric_changes": 0,
                "old_vertical_metric_changes": 0, "old_layout_metrics_unchanged": True,
                "epilogue_glyphs_match": True, "input_fonts_unchanged": True,
                "inspected_scenarios": scenarios, "inspected_string_references": references,
            },
        }
    finally:
        base.close()
        epilogue.close()
        if temporary is not None:
            temporary.unlink(missing_ok=True)
