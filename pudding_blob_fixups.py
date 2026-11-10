#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Paranoid Android
# SPDX-License-Identifier: Apache-2.0
"""Versioned pudding blob transforms; unsupported inputs are explicit errors."""
import hashlib
import json
import struct
from pathlib import Path
from xml.dom import minidom


def storage_capability_xml(data):
    document = minidom.parseString(data)
    root = document.documentElement
    if root.tagName != "PerfResources":
        raise ValueError("Expected PerfResources root")
    matches = []
    for item in root.getElementsByTagName("Config"):
        if not item.hasAttribute("MajorValue") or not item.hasAttribute("MinorValue"):
            continue
        if int(item.getAttribute("MajorValue"), 0) != 0xB or int(item.getAttribute("MinorValue"), 0) != 0x4:
            continue
        targets = [target.strip() for target in item.getAttribute("Target").split(",")]
        if "canoe" in targets:
            matches.append((item, targets))
    if len(matches) > 1:
        raise ValueError("Duplicate canoe storage capability entries")
    if matches:
        item, targets = matches[0]
        if targets == ["canoe"] and item.getAttribute("Supported") == "no":
            return data
        if len(targets) > 1:
            canoe = item.cloneNode(True)
            item.setAttribute("Target", ",".join(target for target in targets if target != "canoe"))
            canoe.setAttribute("Target", "canoe")
            item.parentNode.insertBefore(canoe, item)
            item = canoe
    else:
        item = document.createElement("Config")
        for name, value in [("MajorValue", "0xB"), ("MinorValue", "0x4"), ("Target", "canoe")]:
            item.setAttribute(name, value)
        root.appendChild(item)
    item.setAttribute("Supported", "no")
    output = document.toxml(encoding="utf-8")
    minidom.parseString(output)
    return output


def fixup_storage(ctx, file, file_path):
    path = Path(file_path)
    path.write_bytes(storage_capability_xml(path.read_bytes()))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def elf_sections(data):
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    if header[0][:6] != b"\x7fELF\x02\x01" or header[2] != 183:
        raise ValueError("Expected little-endian AArch64 ELF64")
    rows = [struct.unpack_from("<IIQQQQIIQQ", data, header[6] + i * header[11])
            for i in range(header[12])]
    names = rows[header[13]]
    strings = data[names[4]:names[4] + names[5]]
    return {strings[row[0]:strings.index(0, row[0])].decode(): (row[3], row[4], row[5])
            for row in rows}


def transform_elf(data, rule):
    sections = elf_sections(data)
    hashes = {}
    for name in (".text", ".rodata"):
        address, offset, size = sections[name]
        hashes[name] = digest(data[offset:offset + size])
    expected = rule["section_sha256"]
    if all(hashes[name] == value["after"] for name, value in expected.items()):
        return data
    if any(hashes[name] != value["before"] for name, value in expected.items()):
        raise ValueError("Unsupported ELF code/constants for " + rule["path"])
    output = bytearray(data)
    address, offset, size = sections[".text"]
    for patch in rule["patches"]:
        old, new = bytes.fromhex(patch["old"]), bytes.fromhex(patch["new"])
        relative = int(patch["va"], 0) - address
        if relative < 0 or relative + len(old) > size or len(old) != len(new):
            raise ValueError("Patch is outside the pinned text section")
        at = offset + relative
        if output[at:at + len(old)] != old:
            raise ValueError("Instruction preimage mismatch")
        output[at:at + len(old)] = new
    for name, value in expected.items():
        _, start, length = sections[name]
        if digest(output[start:start + length]) != value["after"]:
            raise ValueError("ELF output section mismatch")
    return bytes(output)


def transform_cpuset(data):
    output = data.decode("utf-8")
    for group in ("background", "system-background"):
        old = "echo 0-3 > /dev/cpuset/" + group + "/cpus"
        new = "echo 0-5 > /dev/cpuset/" + group + "/cpus"
        if output.count(old) != 1:
            raise ValueError("Unexpected canoe cpuset input: " + group)
        output = output.replace(old, new, 1)
    return output.encode("utf-8")


def transform_boosts(data):
    document = minidom.parseString(data)
    unsupported = {0x42C10000, 0x43488000}
    for item in list(document.getElementsByTagName("Config")):
        targets = [target.strip() for target in item.getAttribute("Target").split(",")]
        if "canoe" not in targets:
            continue
        work = item
        if len(targets) > 1:
            work = item.cloneNode(True)
            work.setAttribute("Target", "canoe")
            item.setAttribute("Target", ",".join(target for target in targets if target != "canoe"))
            item.parentNode.insertBefore(work, item)
        for node in [work] + list(work.getElementsByTagName("Selection")):
            for name in ("Resources", "LaunchResources"):
                if not node.hasAttribute(name):
                    continue
                tokens = [token.strip() for token in node.getAttribute(name).split(",") if token.strip()]
                if len(tokens) % 2:
                    raise ValueError("Unpaired perf resource/value")
                pairs = list(zip(tokens[::2], tokens[1::2]))
                kept = [(resource, value) for resource, value in pairs if int(resource, 0) not in unsupported]
                if kept != pairs:
                    if not kept:
                        raise ValueError("Removing the entire boost requires a new reviewed recipe")
                    node.setAttribute(name, ", ".join(value for pair in kept for value in pair))
    return document.toxml().encode("utf-8")


def transform(resource, data):
    rules = json.loads(Path(__file__).with_name("pudding-blob-rules.json").read_text())
    for rule in rules["elf"]:
        if rule["path"] == resource:
            return transform_elf(data, rule)
    rule = rules["files"].get(resource)
    if rule is None:
        raise ValueError("No pudding transform declared for " + resource)
    current = digest(data)
    if current == rule["after"]:
        return data
    if current != rule["before"]:
        raise ValueError("Unsupported input version for " + resource)
    output = transform_cpuset(data) if resource.endswith(".sh") else transform_boosts(data)
    if digest(output) != rule["after"]:
        raise ValueError("Pudding transform output mismatch: " + resource)
    return output


def fixup(ctx, file, file_path, resource):
    path = Path(file_path)
    path.write_bytes(transform(resource, path.read_bytes()))
