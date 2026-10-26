"""Reads tools/re/re.cfg (KEY=VALUE, '#' comments) for the other tools."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def load(path=None):
    path = path or os.path.join(HERE, "re.cfg")

    if not os.path.exists(path):
        sys.exit(f"{path} not found: copy re.cfg.example to re.cfg and fill in the paths")

    cfg = {}

    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()

        if "=" in line:
            key, value = line.split("=", 1)
            value = value.strip()
            # Relative paths are relative to the repository.
            if value and not os.path.isabs(value) and "/" in value:
                value = os.path.join(REPO, value)
            cfg[key.strip()] = value

    return cfg


def need(cfg, *keys):
    missing = [key for key in keys if not cfg.get(key)]

    if missing:
        sys.exit("re.cfg: missing " + ", ".join(missing))

    return [cfg[key] for key in keys]
