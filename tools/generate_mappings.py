#!/usr/bin/env python3
"""Generate woke.wtf's ``mappings.json`` from official Fabric Yarn mappings.

Every JNI handle the client resolves (``class_XXX``, ``method_XXXX``, ``field_XXXX``)
comes from this file. It is therefore generated from the authoritative upstream artefact
instead of being hand-written: a game update is repaired by regenerating this asset, and
no obfuscated identifier is ever guessed.

Source of truth:
    https://maven.fabricmc.net/net/fabricmc/yarn/<version>/yarn-<version>-v2.jar
    (contains ``mappings/mappings.tiny``, Tiny v2, namespaces: intermediary, named)

Usage:
    python3 tools/generate_mappings.py \\
        --jar yarn-1.21.11+build.6-v2.jar \\
        --version 1.21.11 \\
        --source net.fabricmc:yarn:1.21.11+build.6 \\
        --out mappings.json

Only classes listed in CURATED_CLASSES (plus everything reachable from them at the class
level) are emitted, which keeps the asset small enough to ship and review while covering
every system the client currently touches. Add a Yarn named path to that list when a
module needs a new class, then regenerate.
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import zipfile

# Yarn named paths for every class the client's core systems and modules touch.
# Missing entries are reported as warnings rather than failing the build, so a mapping
# refresh never blocks on one renamed class.
CURATED_CLASSES = [
    # client core
    "net/minecraft/client/MinecraftClient",
    "net/minecraft/client/Mouse",
    "net/minecraft/client/Keyboard",
    "net/minecraft/client/util/Window",
    "net/minecraft/client/option/GameOptions",
    "net/minecraft/client/option/SimpleOption",
    "net/minecraft/client/option/KeyBinding",
    "net/minecraft/client/option/Perspective",
    "net/minecraft/client/render/GameRenderer",
    "net/minecraft/client/render/WorldRenderer",
    "net/minecraft/client/render/Camera",
    "net/minecraft/client/gui/hud/InGameHud",
    "net/minecraft/client/gui/screen/Screen",
    "net/minecraft/client/network/ClientPlayerEntity",
    "net/minecraft/client/network/ClientPlayerInteractionManager",
    "net/minecraft/client/network/ClientPlayNetworkHandler",
    "net/minecraft/client/world/ClientWorld",
    # world / entities
    "net/minecraft/entity/Entity",
    "net/minecraft/entity/LivingEntity",
    "net/minecraft/entity/player/PlayerEntity",
    "net/minecraft/entity/player/PlayerInventory",
    "net/minecraft/entity/player/PlayerAbilities",
    "net/minecraft/entity/effect/StatusEffectInstance",
    "net/minecraft/world/World",
    "net/minecraft/block/BlockState",
    # math / hit results / items
    "net/minecraft/util/math/Vec3d",
    "net/minecraft/util/math/Vec3i",
    "net/minecraft/util/math/BlockPos",
    "net/minecraft/util/math/Box",
    "net/minecraft/util/math/MathHelper",
    "net/minecraft/util/hit/HitResult",
    "net/minecraft/util/hit/EntityHitResult",
    "net/minecraft/util/hit/BlockHitResult",
    "net/minecraft/util/Hand",
    "net/minecraft/item/ItemStack",
    "net/minecraft/item/Item",
    # cooldowns (Mace Wind Charge CD, roadmap step 8d) - the thrown-item timers live on the
    # player's ItemCooldownManager
    "net/minecraft/entity/player/ItemCooldownManager",
    # text / formatting
    "net/minecraft/text/Text",
    "net/minecraft/util/Formatting",
    "net/minecraft/screen/ScreenHandler",
]


def unescape(value: str) -> str:
    """Tiny v2 escapes tabs, newlines and backslashes inside names."""
    if "\\" not in value:
        return value
    out = []
    index = 0
    while index < len(value):
        char = value[index]
        if char == "\\" and index + 1 < len(value):
            nxt = value[index + 1]
            out.append({"n": "\n", "t": "\t", "\\": "\\"}.get(nxt, nxt))
            index += 2
        else:
            out.append(char)
            index += 1
    return "".join(out)


def parse_tiny(text: str):
    """Parse Tiny v2 into {named_class: {intermediary, methods, fields}}.

    Layout of the file (verified against yarn-1.21.11+build.6-v2.jar):

        tiny	2	0	intermediary	named      <- one column per namespace
        c	<name ns1>	<name ns2>              <- classes carry names only
        \tf\t<descriptor ns1>\t<name ns1>\t<name ns2>   <- 1 descriptor + 1 name per ns
        \tm\t<descriptor ns1>\t<name ns1>\t<name ns2>
        \t\tp\t<index>\t\t<name>                  <- parameters, ignored

    The descriptor column is expressed with intermediary class names, which is exactly
    what GetMethodID/GetFieldID expect at runtime. Note that intermediary member names
    are not always method_XXXX/field_XXXX: newer Yarn builds also emit comp_XXXX for
    record components, so the loader must never assume a name prefix.
    """
    classes: dict[str, dict] = {}
    current: dict | None = None
    namespace_count = 2  # corrected from the file header before any member is parsed

    for raw_line in text.split("\n"):
        if not raw_line:
            continue

        if raw_line.startswith("tiny\t"):
            # tiny, major, minor, then one column per namespace.
            namespace_count = max(1, len(raw_line.split("\t")) - 3)
            continue

        # Two tabs of indentation means a parameter/local, not a member of the class.
        if raw_line.startswith("\t\t"):
            continue

        if raw_line.startswith("\t"):
            if current is None:
                continue
            columns = raw_line[1:].split("\t")
            if len(columns) < 2 + namespace_count:
                continue
            marker = columns[0]
            if marker != "m" and marker != "f":
                continue  # 'c' entries at this depth are javadoc comments
            names = [unescape(column) for column in columns[2 : 2 + namespace_count]]
            entry = {
                "named": names[1] if namespace_count > 1 else names[0],
                "intermediary": names[0],
                "descriptor": unescape(columns[1]),
            }
            if marker == "m":
                current["methods"].append(entry)
            else:
                current["fields"].append(entry)
            continue

        columns = raw_line.split("\t")
        if columns[0] != "c" or len(columns) < 1 + namespace_count:
            current = None
            continue

        names = [unescape(column) for column in columns[1 : 1 + namespace_count]]
        named_class = names[1] if namespace_count > 1 else names[0]
        current = classes.setdefault(
            named_class,
            {"intermediary": names[0], "methods": [], "fields": []},
        )

    return classes


def squeeze(entries: list[dict]) -> dict:
    """Collapse a member list into {yarn_name: {intermediary, descriptor}}."""
    result: dict[str, dict] = {}
    for entry in entries:
        # Overloaded members share a Yarn name; the first occurrence wins, which matches
        # how the reflection cache looks a handle up by name.
        result.setdefault(
            entry["named"],
            {"intermediary": entry["intermediary"], "descriptor": entry["descriptor"]},
        )
    return result


def build_document(classes: dict, wanted: list[str], version: str, source: str) -> tuple[dict, list[str]]:
    document: dict = {
        "version": version,
        "namespace": "intermediary",
        "source": source,
        # timezone.utc keeps this runnable on Python 3.7+ (datetime.UTC needs 3.11+).
        "generated_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "note": (
            "Generated by tools/generate_mappings.py from official Fabric Yarn mappings. "
            "Keys are Yarn names; values are intermediary identifiers used for JNI lookups. "
            "Regenerate instead of editing by hand."
        ),
        "classes": {},
    }

    missing: list[str] = []
    for named_path in wanted:
        entry = classes.get(named_path)
        if entry is None:
            missing.append(named_path)
            continue

        intermediary_class = entry["intermediary"]
        simple_name = named_path.rsplit("/", 1)[-1]
        short_intermediary = intermediary_class.rsplit("/", 1)[-1]

        document["classes"][simple_name] = {
            "yarn": named_path,
            "intermediary": intermediary_class,
            "aliases": [simple_name, short_intermediary, intermediary_class],
            "methods": squeeze(entry["methods"]),
            "fields": squeeze(entry["fields"]),
        }

    document["stats"] = {
        "classes": len(document["classes"]),
        "methods": sum(len(c["methods"]) for c in document["classes"].values()),
        "fields": sum(len(c["fields"]) for c in document["classes"].values()),
        "missing": missing,
    }
    return document, missing


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate mappings.json from Fabric Yarn mappings")
    parser.add_argument("--jar", required=True, help="path to yarn-<version>-v2.jar")
    parser.add_argument("--version", required=True, help="Minecraft version, e.g. 1.21.11")
    parser.add_argument("--source", required=True, help="maven coordinate of the mapping build")
    parser.add_argument("--out", required=True, help="output path for mappings.json")
    args = parser.parse_args()

    with zipfile.ZipFile(args.jar) as archive:
        names = [n for n in archive.namelist() if n.endswith("mappings.tiny")]
        if not names:
            print(f"error: {args.jar} contains no mappings.tiny", file=sys.stderr)
            return 1
        raw = archive.read(names[0]).decode("utf-8")

    classes = parse_tiny(raw)
    document, missing = build_document(classes, CURATED_CLASSES, args.version, args.source)

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=1, sort_keys=True)
        handle.write("\n")

    stats = document["stats"]
    print(
        f"wrote {args.out}: {stats['classes']} classes, "
        f"{stats['methods']} methods, {stats['fields']} fields"
    )
    if missing:
        print(f"warning: {len(missing)} curated class(es) not found in this build:", file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
