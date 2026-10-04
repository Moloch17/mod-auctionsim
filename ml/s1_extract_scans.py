#!/usr/bin/env python3
"""Stage 1a: every Auctioneer scan in data/scans -> one row per auction.

Output (ml/out/):
  auctions/<faction>/<scan_time>.parquet   one file per AH snapshot
  snapshots.parquet                        one row per snapshot: faction,
                                           scan_time (LastFullScan), start/end,
                                           scan_count, records, source file

Parsing follows data/compile-data.cpp: the ["ropes"] strings are Lua source
escaped as Lua string literals (layer 1); each rope is `return {{...},...}`
whose records are 27 positional fields per Auctioneer's ScanPosLabels, with
string fields escaped again (layer 2). Faction is detected per ropes block
from the nearest ["Horde"]/["Alliance"] above it, not from the filename.

Oracle: each faction section's scanstats[0].scanCount says how many records
the scan holds; a snapshot whose parsed record count differs is reported and
written with ok=false in snapshots.parquet. Scans Auctioneer marked
wasIncomplete (no LastFullScan; a few dozen daily files) are also ok=false:
a partial snapshot would make every unscanned auction look gone.

A snapshot is identified by (faction, scan_time), scan_time being the scan's
endTime (equal to LastFullScan on a complete scan); a second file holding the
same scan is skipped. Files already extracted are skipped on a rerun.
"""

import re
import sys
import polars as pl

from common import ALLIANCE, FACTION_NAMES, HORDE, OUT, SCANS, pool

# Layer 1: a Lua double-quoted literal; layer 2 tokens inside a rope.
LUA_STRING = re.compile(rb'"((?:[^"\\]|\\.)*)"', re.S)
ESCAPE = re.compile(rb"\\(.)", re.S)
ESCAPES = {b"n": b"\n", b"t": b"\t", b"r": b"\r"}
TOKEN = re.compile(rb'"((?:[^"\\]|\\.)*)"|([{}])|([^,{}\s"]+)', re.S)
INT = re.compile(rb"-?\d+")


def unescape(raw):
    return ESCAPE.sub(lambda m: ESCAPES.get(m.group(1), m.group(1)), raw)


# Auctioneer ScanPosLabels, 0-based, and the columns kept.
FIELDS = {"ilevel": 1, "itype": 2, "isub": 3, "tleft": 6, "name": 8, "count": 10, "quality": 11,
          "ulevel": 13, "minbid": 14, "mininc": 15, "buyout": 16, "curbid": 17, "amhigh": 18,
          "seller": 19, "flag": 20, "item": 22, "suffix": 23, "factor": 24, "enchant": 25, "seed": 26}
INT_FIELDS = ["ilevel", "tleft", "count", "quality", "ulevel", "minbid", "mininc", "buyout", "curbid",
              "flag", "item", "suffix", "factor", "enchant", "seed"]
STR_FIELDS = ["itype", "isub", "name", "seller"]


def parse_records(rope):
    """[(fields...)] for one unescaped rope `return {{...},{...},}`."""
    records, fields, depth = [], None, 0
    for m in TOKEN.finditer(rope):
        s, brace, bare = m.groups()
        if brace == b"{":
            depth += 1
            if depth == 2:
                fields = []
        elif brace == b"}":
            if depth == 2:
                records.append(fields)
            depth -= 1
        elif depth == 2:
            fields.append(unescape(s) if s is not None else bare)
    return records


def sections(text):
    """(faction, stats, [rope sources]) per ["ropes"] block."""
    out = []
    pos = 0
    while (r := text.find(b'["ropes"]', pos)) != -1:
        horde, ally = text.rfind(b'["Horde"]', 0, r), text.rfind(b'["Alliance"]', 0, r)
        faction = HORDE if horde > ally else ALLIANCE if ally > horde else 0
        stats_at = text.rfind(b'["scanstats"]', 0, r)
        head = text[stats_at:r] if stats_at != -1 else b""
        stats = {}
        for key in (b"LastFullScan", b"startTime", b"endTime", b"scanCount"):
            m = re.search(rb'\["' + key + rb'"\] = (\d+)', head)
            stats[key.decode()] = int(m.group(1)) if m else None
        stats["incomplete"] = b'["wasIncomplete"] = true' in head
        # The ropes table: string literals until its closing brace.
        i = text.index(b"{", r) + 1
        ropes = []
        while True:
            m = re.compile(rb'\s*(?:--[^\n]*\n\s*|,\s*)*').match(text, i)
            i = m.end()
            if i >= len(text) or text[i:i + 1] != b'"':
                break
            lit = LUA_STRING.match(text, i)
            ropes.append(unescape(lit.group(1)))
            i = lit.end()
        out.append((faction, stats, ropes))
        pos = i
    return out


def to_int(b):
    return int(b) if b and INT.fullmatch(b) else None


def extract(path):
    text = path.read_bytes()
    results = []
    for faction, stats, ropes in sections(text):
        rows = {k: [] for k in FIELDS}
        malformed = 0
        for rope in ropes:
            for f in parse_records(rope):
                if len(f) < 27:
                    malformed += 1
                    continue
                for k, i in FIELDS.items():
                    rows[k].append(f[i])
        df = pl.DataFrame({
            **{k: [to_int(v) for v in rows[k]] for k in INT_FIELDS},
            **{k: [v.decode("utf-8", "replace") for v in rows[k]] for k in STR_FIELDS},
            "amhigh": [v == b"true" for v in rows["amhigh"]],
        }, schema_overrides={k: pl.Int64 for k in INT_FIELDS})
        results.append((faction, stats, malformed, df))
    return path.name, results


def main():
    (OUT / "auctions").mkdir(parents=True, exist_ok=True)
    snap_path = OUT / "snapshots.parquet"
    snaps = pl.read_parquet(snap_path) if snap_path.exists() else None
    done = set(snaps["file"]) if snaps is not None else set()
    seen = set(zip(snaps["faction"], snaps["scan_time"])) if snaps is not None else set()
    files = sorted(p for p in SCANS.iterdir() if p.is_file() and p.name not in done)
    print(f"{len(files)} scan files to extract ({len(done)} done before)")
    new = []
    with pool() as workers:
        for n, (name, results) in enumerate(workers.imap_unordered(extract, files), 1):
            for faction, stats, malformed, df in results:
                scan_time = stats["endTime"] or stats["LastFullScan"]
                key = (faction, scan_time)
                row = {"file": name, "faction": faction, "scan_time": scan_time,
                       "start_time": stats["startTime"], "end_time": stats["endTime"],
                       "scan_count": stats["scanCount"], "records": df.height, "malformed": malformed,
                       "incomplete": stats["incomplete"],
                       "ok": df.height == stats["scanCount"] and faction != 0 and not stats["incomplete"],
                       "duplicate": key in seen}
                if df.height != stats["scanCount"] or not faction:
                    print(f"\n{name}: faction {faction}, {df.height} records vs scanCount "
                          f"{stats['scanCount']}, {malformed} malformed", file=sys.stderr)
                if not row["duplicate"] and faction and scan_time:
                    seen.add(key)
                    d = OUT / "auctions" / FACTION_NAMES[faction]
                    d.mkdir(exist_ok=True)
                    df.write_parquet(d / f"{scan_time}.parquet")
                new.append(row)
            print(f"{n}/{len(files)} files", end="\r", flush=True)
    if new:
        add = pl.DataFrame(new)
        snaps = add if snaps is None else pl.concat([snaps, add], how="vertical_relaxed")
        snaps.sort("faction", "scan_time").write_parquet(snap_path)
    print()
    if snaps is not None:
        print(snaps.group_by("faction", "ok", "incomplete", "duplicate").agg(
            pl.len().alias("snapshots"), pl.col("records").sum(), (pl.col("records") == pl.col("scan_count")).sum()
            .alias("count_matches")).sort("faction", "ok", "incomplete", "duplicate"))


if __name__ == "__main__":
    main()
