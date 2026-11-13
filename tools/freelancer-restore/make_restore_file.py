"""Lists the Freelancer campaign snapshots in a NoMoreAltF4 session log and writes one of them
as freelancer-restore.json, the input of the restore tool (see README.md).

The server sends the whole campaign (ContractProgressionData) in every Freelancer
GetForPlay2 response, so each response is a snapshot of the campaign at that moment.

usage:
    python make_restore_file.py <session log>                  list the snapshots
    python make_restore_file.py <session log> <line> <out>     write the snapshot from that log line

Session logs have lines over 100 KB; this script reads them line by line and prints only a summary.
"""
import json
import os
import sys


def snapshots(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for no, line in enumerate(f, 1):
            if "[NoMoreAltF4] HTTP Response" not in line:
                continue
            text = line.split("): ", 1)[-1].strip()
            if not text.startswith('{"Contract":'):
                continue
            try:
                obj = json.loads(text)
            except ValueError:
                continue
            cpd = obj.get("ContractProgressionData")
            if not isinstance(cpd, dict):
                continue
            meta = (obj.get("Contract") or {}).get("Metadata") or {}
            yield no, meta, obj.get("ContractSessionId", ""), cpd


def summary(cpd):
    inv = cpd.get("InventoryItems")
    return (f"{len(inv) if isinstance(inv, list) else '?'} carried items, {int(cpd.get('MyMoney') or 0)} merces, "
            f"campaign step {int(cpd.get('CPD_CampaignStep') or 0)}, "
            f"Hot_MissionFailed {str(cpd.get('Hot_MissionFailed')).lower()}")


def main():
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    log = sys.argv[1]

    if len(sys.argv) == 2:
        found = False
        for no, meta, csid, cpd in snapshots(log):
            found = True
            print(f"line {no}: contract {meta.get('Id', '?')[:8]} ({meta.get('Type')}), session ..{csid[-6:]}, "
                  f"CpdId {meta.get('CpdId')}, {len(cpd)} keys: {summary(cpd)}")
        if not found:
            print("no Freelancer campaign snapshot in this log (was 'Log HTTP requests' on?)")
        return

    want, out = int(sys.argv[2]), sys.argv[3]
    for no, meta, csid, cpd in snapshots(log):
        if no != want:
            continue
        cpd_id = meta.get("CpdId") or ""
        if len(cpd_id) != 36:
            sys.exit(f"line {no} has no CpdId")
        text = (f"{summary(cpd)} (snapshot from {os.path.basename(log)} line {no})")
        values = json.dumps(cpd, ensure_ascii=True, separators=(",", ":"))
        with open(out, "w", encoding="ascii", newline="\n") as f:
            f.write(cpd_id + "\n" + text + "\n" + values + "\n")
        print(f"wrote {out}: {text}, {len(values)} bytes")
        return
    sys.exit(f"no campaign snapshot at line {want}")


if __name__ == "__main__":
    main()
