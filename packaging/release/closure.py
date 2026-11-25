#!/usr/bin/env python3
"""Audit ELF dependencies against the bundle and the pinned Platform, not the SDK."""
import argparse
import json
from pathlib import Path
import re
import subprocess

MACHINES = {"aarch64": "AArch64", "x86_64": "Advanced Micro Devices X86-64"}


def rooted(root, path):
    """Resolve absolute runtime symlinks inside their sysroot, never on the host."""
    parts = list(Path(path).parts)
    resolved = root
    hops = 0
    while parts:
        part = parts.pop(0)
        if part == "/":
            resolved = root
        elif part == "..":
            resolved = resolved.parent if resolved != root else root
        elif part != ".":
            resolved = resolved / part
            if resolved.is_symlink():
                hops += 1
                if hops > 40:
                    raise RuntimeError("Symlink loop: " + str(path))
                target = resolved.readlink()
                resolved = resolved.parent
                parts = list(target.parts) + parts
    return resolved


def elf(path):
    try:
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                return None
    except (FileNotFoundError, IsADirectoryError):
        return None
    header = subprocess.check_output(["readelf", "-h", str(path)], text=True)
    dynamic = subprocess.check_output(["readelf", "-dW", str(path)], text=True)
    program = subprocess.check_output(["readelf", "-lW", str(path)], text=True)
    versions = subprocess.check_output(["readelf", "-VW", str(path)], text=True)
    machine = re.search(r"Machine:\s*(.*)", header).group(1).strip()
    needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
    soname = re.findall(r"\(SONAME\).*\[(.*?)\]", dynamic)
    paths = re.findall(r"\((?:RUNPATH|RPATH)\).*\[(.*?)\]", dynamic)
    interpreter = re.findall(r"Requesting program interpreter: (.*?)\]", program)
    definitions = set()
    requirements = {}
    state = None
    current = None
    for line in versions.splitlines():
        if line.startswith("Version definition section"):
            state = "definitions"
        elif line.startswith("Version needs section"):
            state = "requirements"
        elif line.startswith("Version symbols section"):
            state = None
        if state == "definitions":
            definitions.update(re.findall(r"Name: (\S+)", line))
        elif state == "requirements":
            match = re.search(r"File: (\S+)", line)
            if match:
                current = match.group(1)
            match = re.search(r"Name: (\S+)", line)
            if match and current:
                requirements.setdefault(current, set()).add(match.group(1))
    return {"machine": machine, "needed": needed, "soname": soname[0] if soname else path.name,
            "paths": paths, "definitions": definitions, "requirements": requirements,
            "interpreter": interpreter[0] if interpreter else None}


def symbols(path):
    """The names an object must be given and the names it provides. A versioned
    import is name@VERSION and needs that version; a plain one takes any."""
    wanted, provided = set(), set()
    table = subprocess.check_output(["readelf", "--dyn-syms", "-W", str(path)], text=True)
    for line in table.splitlines():
        # Num: Value Size Type Bind Vis [other] Ndx Name
        row = re.match(r"\s*\d+:\s+\S+\s+\S+\s+\S+\s+(\S+)\s+\S+(?:\s+\[[^\]]*\])?\s+(\S+)\s+(\S+)", line)
        if not row or row[1] == "LOCAL":
            continue
        name = row[3].replace("@@", "@")
        if row[2] != "UND":
            provided.update({name, name.split("@")[0]})
        elif row[1] == "GLOBAL":
            wanted.add(name)
    return wanted, provided


def audit(bundle, platform, architecture):
    bundle, platform = Path(bundle).resolve(), Path(platform).resolve()
    expected = MACHINES[architecture]
    providers = {}
    objects = {}
    platform_names = set()
    errors = []
    # Prefer the actual bundle, exactly as its launcher's library path does.
    for root, files in [(platform, platform.rglob("*.so*")), (bundle, bundle.rglob("*"))]:
        for path in files:
            real = rooted(root, path.relative_to(root))
            if real not in objects:
                objects[real] = elf(real)
            info = objects[real]
            if info and info["machine"] == expected:
                if root == platform:
                    platform_names.add(info["soname"])
                elif info["soname"] in platform_names:
                    errors.append(str(path) + ": shadows a Platform library " + info["soname"])
                # The loader opens the requested filename; a SONAME alone does
                # not make libfoo.so.1.2 reachable as libfoo.so.1.
                if root == platform or path.parent == bundle / "lib":
                    providers[path.name] = (real, info)
    roots = [p for p, info in objects.items() if info and p.is_relative_to(bundle)]
    main = rooted(bundle, "bin/videoframevr")
    if not main.is_file():
        errors.append("Missing bin/videoframevr")
    elif not objects.get(main):
        errors.append("bin/videoframevr is not an ELF executable")
    for path in bundle.rglob("*"):
        if path.is_symlink() and not path.resolve().is_relative_to(bundle):
            errors.append(str(path) + ": symlink escapes the bundle")
    seen = set()
    report = []
    queue = list(roots)
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        info = objects[path]
        if info["machine"] != expected:
            errors.append(str(path) + ": wrong architecture")
        bundled = path.is_relative_to(bundle)
        if bundled:
            if info["interpreter"]:
                loader = rooted(platform, info["interpreter"])
                if not loader.is_file():
                    errors.append(str(path) + ": Platform lacks interpreter " + info["interpreter"])
            for search in info["paths"]:
                for part in search.split(":"):
                    relative = None
                    for origin in ["$ORIGIN", "${ORIGIN}"]:
                        if part == origin or part.startswith(origin + "/"):
                            relative = path.parent / part[len(origin):].lstrip("/")
                    if relative is None or not relative.resolve().is_relative_to(bundle):
                        errors.append(str(path) + ": non-relocatable RPATH " + search)
        edges = {}
        for name in info["needed"]:
            if name not in providers:
                errors.append(str(path) + ": missing " + name)
                continue
            provider, dependency = providers[name]
            missing = info["requirements"].get(name, set()) - dependency["definitions"]
            if missing:
                errors.append(str(path) + ": " + name + " lacks " + ", ".join(sorted(missing)))
            origin = "bundle" if provider.is_relative_to(bundle) else "platform"
            edges[name] = origin + "/" + str(provider.relative_to(bundle if origin == "bundle" else platform))
            queue.append(provider)
        if bundled:
            report.append({"file": str(path.relative_to(bundle)), "needed": edges})
    # Most Platform libraries have no symbol versions, so a function a newer SDK
    # added is only caught by name.
    tables = {path: symbols(path) for path in seen}
    provided = set().union(*(names for _, names in tables.values()))
    for path, (wanted, _) in tables.items():
        if path.is_relative_to(bundle) and wanted - provided:
            errors.append(str(path) + ": nothing provides " + ", ".join(sorted(wanted - provided)))
    result = {"schema": 1, "architecture": architecture, "objects": sorted(report, key=lambda x: x["file"]),
              "errors": sorted(set(errors)),
              "scope": "DT_NEEDED, architecture, GNU symbol versions and symbol names; "
                       "not a GPU or dlopen execution check"}
    if not roots:
        result["errors"].append("Bundle contains no ELF objects")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("platform", type=Path)
    parser.add_argument("--arch", choices=MACHINES, required=True)
    args = parser.parse_args()
    result = audit(args.bundle, args.platform, args.arch)
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result["errors"]))
