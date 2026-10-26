#!/usr/bin/env python3
"""Builds the Amiga system drive FS-UAE boots to run the Amiga port.

The drive is the user's Workbench 3.1 disk ("Workbench", disk 2) unpacked
into AMIGA_SYSTEM, with a startup-sequence that runs WORK:Run-Test when the
second drive carries one (amiga_run.py writes it) and otherwise ends in a
shell. Nothing of it goes into the repository.

usage: amiga_system.py [--force]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import recfg

STARTUP_SEQUENCE = """\
; Written by tools/re/amiga_system.py: the smallest boot that runs a test.
C:SetPatch QUIET
C:Assign >NIL: ENV: RAM:
C:Assign >NIL: T: RAM:
FailAt 21
If EXISTS WORK:Run-Test
  Execute WORK:Run-Test
EndIf
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--force", action="store_true", help="rebuild an existing drive")
    args = parser.parse_args()

    cfg = recfg.load()
    adf, system, xdftool = recfg.need(cfg, "WORKBENCH_ADF", "AMIGA_SYSTEM", "XDFTOOL")

    if os.path.exists(os.path.join(system, "S", "Startup-Sequence")) and not args.force:
        print(f"{system}: already built (--force to rebuild)")
        return

    with tempfile.TemporaryDirectory() as temp:
        subprocess.run([xdftool, adf, "unpack", temp], check=True, stdout=subprocess.DEVNULL)
        volumes = [d for d in os.listdir(temp) if os.path.isdir(os.path.join(temp, d))]

        if len(volumes) != 1:
            sys.exit(f"{adf}: expected one volume, found {volumes}")

        if os.path.exists(system):
            shutil.rmtree(system)

        shutil.copytree(os.path.join(temp, volumes[0]), system)

    # xdftool writes the Amiga file attributes beside the files; FS-UAE keeps
    # its own, so they are not needed.
    for root, _, files in os.walk(system):
        for name in files:
            if name.endswith(".uaem") or name == ".xdfmeta":
                os.remove(os.path.join(root, name))

    startup = os.path.join(system, "S", "Startup-Sequence")
    os.replace(startup, startup + ".workbench") if os.path.exists(startup) else None
    with open(startup, "w", newline="\n") as out:
        out.write(STARTUP_SEQUENCE)

    for needed in ("C/SetPatch", "C/Assign", "Libs/lowlevel.library"):
        if not os.path.exists(os.path.join(system, needed)):
            print(f"warning: {needed} not on the disk", file=sys.stderr)

    print(f"{system}: built from {os.path.basename(adf)}")


if __name__ == "__main__":
    main()
