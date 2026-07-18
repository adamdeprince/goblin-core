#!/usr/bin/env python3
"""Generate Set-command SBE C++ headers by cloning checked-in templates.

SbeTool needs a JRE; this keeps ordinary builds header-only without Java.
Re-run after editing the templates or the id/name table below.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / "sbe" / "generated" / "goblin_sbe"


def write_header(path: Path, content: str) -> None:
    path.write_text("\n".join(line.rstrip() for line in content.splitlines()) + "\n")


def clone(
    src_name: str,
    dst_name: str,
    template_id: int,
    *,
    renames: list[tuple[str, str]] | None = None,
) -> None:
    src = (GEN / f"{src_name}.h").read_text()
    text = src
    # Class / include guards first (whole-word class name).
    text = text.replace(f"_{src_name.upper()}_CXX_H_", f"_{dst_name.upper()}_CXX_H_")
    text = re.sub(rf"\b{src_name}\b", dst_name, text)
    # templateId() return value
    text = re.sub(
        r"(sbeTemplateId\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()(\d+)(\);)",
        rf"\g<1>{template_id}\3",
        text,
        count=1,
    )
    for old, new in renames or []:
        text = text.replace(old, new)
    write_header(GEN / f"{dst_name}.h", text)
    print(f"  {src_name}.h -> {dst_name}.h (id={template_id})")


def main() -> None:
    print("Generating set SBE headers...")

    # Key-only
    clone("HLen", "SCard", 104)
    clone("HLen", "SMembers", 107)

    # key + member
    clone(
        "HExists",
        "SIsMember",
        105,
        renames=[
            ("Field", "Member"),
            ("field", "member"),
            ("FIELD", "MEMBER"),
        ],
    )

    # members group + key  (HDel: fields group + key)
    for name, tid in (("SAdd", 102), ("SRem", 103), ("SMIsMember", 106)):
        clone(
            "HDel",
            name,
            tid,
            renames=[
                ("Fields", "Members"),
                ("fields", "members"),
                ("Field", "Member"),
                ("field", "member"),
                ("FIELDS", "MEMBERS"),
                ("FIELD", "MEMBER"),
            ],
        )

    # count + key
    clone("LPop", "SPop", 108)
    clone("LPop", "SRandMember", 109)

    # source + destination + member  (HSetNx: key + field + value)
    clone(
        "HSetNx",
        "SMove",
        110,
        renames=[
            ("putKey", "putSource"),
            ("getKey", "getSource"),
            ("Key", "Source"),  # careful - may over-rename
        ],
    )
    # Fix HSetNx renames more carefully for SMove
    smove = (GEN / "SMove.h").read_text()
    # Undo over-aggressive Key->Source in places that should stay as message bits
    # Re-clone with targeted renames instead.
    base = (GEN / "HSetNx.h").read_text()
    text = base
    text = text.replace("_HSETNX_CXX_H_", "_SMOVE_CXX_H_")
    text = re.sub(r"\bHSetNx\b", "SMove", text)
    text = re.sub(
        r"(sbeTemplateId\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()(\d+)(\);)",
        r"\g<1>110\3",
        text,
        count=1,
    )
    # key -> source, field -> destination, value -> member (API names)
    replacements = [
        ("putKey", "putSource"),
        ("getKey", "getSource"),
        ("keyMetaAttribute", "sourceMetaAttribute"),
        ("keyId()", "sourceId()"),
        ("keySinceVersion", "sourceSinceVersion"),
        ("keyInActingVersion", "sourceInActingVersion"),
        ("keyEncodingOffset", "sourceEncodingOffset"),
        ("keyLength", "sourceLength"),
        ("putField", "putDestination"),
        ("getField", "getDestination"),
        ("fieldMetaAttribute", "destinationMetaAttribute"),
        ("fieldId()", "destinationId()"),
        ("fieldSinceVersion", "destinationSinceVersion"),
        ("fieldInActingVersion", "destinationInActingVersion"),
        ("fieldEncodingOffset", "destinationEncodingOffset"),
        ("fieldLength", "destinationLength"),
        ("putValue", "putMember"),
        ("getValue", "getMember"),
        ("valueMetaAttribute", "memberMetaAttribute"),
        ("valueId()", "memberId()"),
        ("valueSinceVersion", "memberSinceVersion"),
        ("valueInActingVersion", "memberInActingVersion"),
        ("valueEncodingOffset", "memberEncodingOffset"),
        ("valueLength", "memberLength"),
        # JSON keys in skip/toString
        ('"key"', '"source"'),
        ('"field"', '"destination"'),
        ('"value"', '"member"'),
    ]
    for old, new in replacements:
        text = text.replace(old, new)
    write_header(GEN / "SMove.h", text)
    print("  HSetNx.h -> SMove.h (id=110) [hand-renamed]")

    # keys group only
    for name, tid in (("SInter", 111), ("SUnion", 112), ("SDiff", 113)):
        clone("MGet", name, tid)

    # keys group + destination (like HDel: group then trailing key)
    for name, tid in (
        ("SInterStore", 114),
        ("SUnionStore", 115),
        ("SDiffStore", 116),
    ):
        clone(
            "HDel",
            name,
            tid,
            renames=[
                ("Fields", "Keys"),
                ("fields", "keys"),
                ("Field", "Key"),
                ("field", "key"),
                ("FIELDS", "KEYS"),
                ("FIELD", "KEY"),
            ],
        )
        # HDel's group field and trailing key are both named "key" after the
        # group rename. Only rename the trailing varData block; the group must
        # retain its getKey/putKey API.
        path = GEN / f"{name}.h"
        text = path.read_text()
        blocks = list(re.finditer(r"static const char \*keyMetaAttribute", text))
        if len(blocks) != 2:
            raise RuntimeError(f"unexpected {name} key block layout")
        split = blocks[1].start()
        head, tail = text[:split], text[split:]
        for old, new in (
            ("putKey", "putDestination"),
            ("getKey", "getDestination"),
            ("keyMetaAttribute", "destinationMetaAttribute"),
            ("keyCharacterEncoding", "destinationCharacterEncoding"),
            ("keyId", "destinationId"),
            ("keySinceVersion", "destinationSinceVersion"),
            ("keyInActingVersion", "destinationInActingVersion"),
            ("keyEncodingOffset", "destinationEncodingOffset"),
            ("keyHeaderLength", "destinationHeaderLength"),
            ("keyLength", "destinationLength"),
            ("skipKey", "skipDestination"),
            ('"key"', '"destination"'),
        ):
            tail = tail.replace(old, new)
        write_header(path, head + tail)

    # limit + keys group: start from LPop (count field) but need keys group.
    # Build SInterCard from Eval-like isn't available. Use a hybrid:
    # take HDel (group + trailing varData) and add a limit field in the block.
    # Simpler: clone LPop, rename count->limit, then we only have one key —
    # WRONG for multi-key.
    #
    # Best available template with fixed field + group: look at... none simple.
    # Use MGet (keys group only) and encode limit as a synthetic first approach:
    # Actually use Eval structure is complex. Custom: clone HIncrBy? delta+key+field.
    #
    # Practical: SInterCard uses keys group only + limit as int64 in block before
    # group. Clone from a hand-built variant of MGet with blockLength 8.
    # Start from LPush which has uint8 fields + group + key - messy.
    #
    # Clone HDel and inject limit field by changing sbeBlockLength 0 -> 8 and
    # adding count()-like accessors from LPop renamed to limit. That's fragile.
    #
    # Alternative API: put limit in the wire as optional trailing varData "limit"
    # string - ugly.
    #
    # Go with: SInterCard = keys group only, limit always 0 on wire for v1... no.
    #
    # Use PubSub-like: field operation + group. Or GoblinZWindow.
    # GoblinZWindow has multiple doubles + key + member.
    #
    # Simplest correct approach: write SInterCard by copying LPop (blockLength 8)
    # then replacing the key varData section with a keys group copied from MGet.
    # Too fragile for sed.
    #
    # Use two messages? No.
    #
    # Final approach for SInterCard: reuse SInter wire (keys only) and put limit
    # as a separate optional later. For now include limit as first "key" is bad.
    #
    # I'll clone LPop -> SInterCard, rename count->limit, and add keys by
    # keeping single key for 1-set case only? Insufficient.
    #
    # Read Eval.h structure for field + groups...

    # SInterCard: clone from custom assembly in gen_sintercard()
    gen_sintercard()
    gen_sscan()
    print("done.")


def gen_sintercard() -> None:
    """limit (int64) + keys group. Assemble from LPop block + MGet group."""
    lpop = (GEN / "LPop.h").read_text()
    mget = (GEN / "MGet.h").read_text()
    # Use MGet as base (keys group) and inject limit field like LPop's count.
    text = mget
    text = text.replace("_MGET_CXX_H_", "_SINTERCARD_CXX_H_")
    text = re.sub(r"\bMGet\b", "SInterCard", text)
    text = re.sub(
        r"(sbeTemplateId\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()(\d+)(\);)",
        r"\g<1>117\3",
        text,
        count=1,
    )
    # Change block length 0 -> 8
    text = re.sub(
        r"(sbeBlockLength\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()0(\);)",
        r"\g<1>8\2",
        text,
        count=1,
    )
    # Insert limit field accessors before keys() - copy count from LPop renamed.
    count_block = re.search(
        r"(SBE_NODISCARD static const char \*countMetaAttribute.*?LPop &count\(const std::int64_t value\) SBE_NOEXCEPT\s*\{.*?return \*this;\s*\})",
        lpop,
        re.S,
    )
    if not count_block:
        # Fallback: simpler injection
        limit_accessors = """
    SBE_NODISCARD std::int64_t limit() const SBE_NOEXCEPT
    {
        std::int64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::int64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    SInterCard &limit(const std::int64_t value) SBE_NOEXCEPT
    {
        std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::int64_t));
        return *this;
    }
"""
    else:
        limit_accessors = (
            count_block.group(1)
            .replace("LPop", "SInterCard")
            .replace("count", "limit")
            .replace("Count", "Limit")
            .replace("COUNT", "LIMIT")
        )
    # Insert before "Keys m_keys" or keys() method
    if "Keys m_keys" in text:
        text = text.replace(
            "Keys m_keys",
            "public:\n" + limit_accessors + "\nprivate:\n    Keys m_keys",
        )
    elif "class Keys" in text:
        text = text.replace("class Keys", limit_accessors + "\n    class Keys")
    write_header(GEN / "SInterCard.h", text)
    print("  MGet+LPop -> SInterCard.h (id=117)")


def gen_sscan() -> None:
    """cursor uint64 + count int64 + key + match. Clone LRange (2x int64) + extra varData."""
    lrange = (GEN / "LRange.h").read_text()
    text = lrange
    text = text.replace("_LRANGE_CXX_H_", "_SSCAN_CXX_H_")
    text = re.sub(r"\bLRange\b", "SScan", text)
    text = re.sub(
        r"(sbeTemplateId\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()(\d+)(\);)",
        r"\g<1>118\3",
        text,
        count=1,
    )
    # start/stop are int64 at 0 and 8. Map start->cursor but cursor is uint64 -
    # wire layout is still 8+8 bytes; treat as uint64/int64 via rename and cast APIs.
    text = text.replace("start", "cursor")
    text = text.replace("Start", "Cursor")
    text = text.replace("START", "CURSOR")
    text = text.replace("stop", "count")
    text = text.replace("Stop", "Count")
    text = text.replace("STOP", "COUNT")
    # Fix over-replacement of "stop" inside other words if any — none expected.
    # Change cursor type from int64 to uint64 in accessors for correctness.
    text = text.replace("std::int64_t cursor", "std::uint64_t cursor")
    text = text.replace("const std::int64_t value) SBE_NOEXCEPT\n    {\n        std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);\n        std::memcpy(m_buffer + m_offset + 0",
                        "const std::uint64_t value) SBE_NOEXCEPT\n    {\n        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);\n        std::memcpy(m_buffer + m_offset + 0")
    # Add match varData after key by cloning the complete key accessor block.
    key_start = text.index("    SBE_NODISCARD static const char *keyMetaAttribute")
    key_end = text.index("\ntemplate<typename CharT", key_start)
    key_block = text[key_start:key_end]
    match_block = (
        key_block.replace("key", "match")
        .replace("Key", "Match")
        .replace("KEY", "MATCH")
    )
    match_block = re.sub(
        r"(matchId\(\) SBE_NOEXCEPT\s*\{\s*return static_cast<std::uint16_t>\()3(\);)",
        r"\g<1>4\2",
        match_block,
        count=1,
    )
    text = text[:key_end] + "\n" + match_block + text[key_end:]
    text = text.replace("    skipKey();\n", "    skipKey();\n    skipMatch();\n", 1)
    text = text.replace(
        "computeLength(std::size_t keyLength = 0)",
        "computeLength(std::size_t keyLength = 0, std::size_t matchLength = 0)",
        1,
    )
    length_tail = """
    length += matchHeaderLength();
    if (matchLength > 1073741824LL)
    {
        throw std::runtime_error("matchLength too long for length type [E109]");
    }
    length += matchLength;
"""
    text = text.replace("\n    return length;", length_tail + "\n    return length;", 1)
    text = text.replace("std::int64_t val;\n        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::int64_t));",
                        "std::uint64_t val;\n        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));",
                        1)
    write_header(GEN / "SScan.h", text)
    print("  LRange.h -> SScan.h (id=118)")


if __name__ == "__main__":
    main()
