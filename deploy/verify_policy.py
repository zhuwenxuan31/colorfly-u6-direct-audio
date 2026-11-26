"""Check that every original policy rule remains, and only declared U6 rules were added."""
from pathlib import Path
import sys


def normalized(line):
    words = line.replace("{", "").replace("}", "").split()
    if words[0] == "allow":
        return {" ".join(words[:4] + [permission]) for permission in words[4:]}
    if words[0] == "type":
        return {" ".join(words[:2] + sorted(words[2:]))}
    return {" ".join(words).replace('"', "")}


def dump(path):
    return {
        entry for line in path.read_text().splitlines()
        if line.strip() and line != "u6_status=0" for entry in normalized(line)
    }


def expected_patch(path):
    lines = path.read_text().splitlines()
    types = {}
    extra = set()
    for line in lines:
        words = line.split()
        if not words:
            continue
        if words[0] in ("type", "typeattribute"):
            types.setdefault(words[1], set()).add(words[2])
        elif words[0] in ("allow", "type_transition"):
            extra.update(normalized(line))
        else:
            raise ValueError(f"unexpected patch statement: {line}")
    extra.update(" ".join(["type", name, *sorted(attributes)])
                 for name, attributes in types.items())
    return extra


def main():
    patch, original, candidate = map(Path, sys.argv[1:])
    before, after = dump(original), dump(candidate)
    deleted, added = before - after, after - before
    expected = expected_patch(patch)
    if deleted or added != expected:
        raise ValueError(f"removed={sorted(deleted)} missing={sorted(expected - added)} "
                         f"extra={sorted(added - expected)}")
    for entry in sorted(added):
        print(entry)


if __name__ == "__main__":
    main()
