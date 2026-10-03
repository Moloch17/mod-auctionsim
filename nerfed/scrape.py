#!/usr/bin/env python3
"""Mirror ah.nerfed.net (Web Auctioneer) pages for Warmane - Lordaeron.

Raw collection only. Nothing here is compiled into auctionsim.dat, and nothing
outside this directory is written; ../data/auctionsim.dat is only read for
extra item ids.

Every page is saved as it came, gzipped, so all the site shows is kept:

  raw/<faction>/list/<page>.html.gz   realm listing pages: per-item current
                                      stats, the Valuable and Craft Profit tabs
  raw/<faction>/items/<id>.html.gz    item pages: the price/volume history
                                      (all_data: quantity, bid mean/median,
                                      buyout median/min, cost price per
                                      snapshot since 2023), current and
                                      lifetime stats, cost-price breakdown

Item ids are the listing's plus every id in auctionsim.dat; ids the site
doesn't know come back as "Item not found" and are saved as such. The run is
resumable: pages already on disk are skipped (--refresh fetches them again).

  ./scrape.py                 # both factions
  ./scrape.py --faction horde

Load: --workers connections (default 2) with no added delay, i.e. about as
fast as the site answers two people browsing. Any 429/5xx pauses every
worker, with backoff.
"""

import argparse
import gzip
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import requests

SITE = "https://ah.nerfed.net"
REALM = 14  # Warmane - Lordaeron
FACTIONS = {"horde": 1, "alliance": 2}
HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
DAT = HERE.parent / "data" / "auctionsim.dat"

ITEM_LINK_RE = re.compile(r"/item/index\?id=(\d+)")
TOTAL_RE = re.compile(r"Showing <b>[^<]*</b> of <b>([\d,]+)</b>")
PER_PAGE = 50  # the site ignores larger per-page values
RETRY_STATUS = {429, 500, 502, 503, 504, 520, 521, 522, 524}


class Client:
    def __init__(self):
        self.local = threading.local()
        self.resume_at = 0.0  # shared pause after a 429/5xx
        self.lock = threading.Lock()

    def session(self):
        if not hasattr(self.local, "s"):
            self.local.s = requests.Session()
            self.local.s.headers["User-Agent"] = "mod-auctionsim data collection (github.com/Moloch17/mod-auctionsim)"
        return self.local.s

    def get(self, path, params):
        for attempt in range(8):
            wait = self.resume_at - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                r = self.session().get(SITE + path, params=params, timeout=60)
                if r.status_code == 200:
                    return r.content
                if r.status_code not in RETRY_STATUS:
                    raise RuntimeError(f"{path} {params}: HTTP {r.status_code}")
                reason = f"HTTP {r.status_code}"
            except requests.RequestException as e:
                reason = str(e)
            backoff = min(900, 30 * 2 ** attempt)
            with self.lock:
                self.resume_at = max(self.resume_at, time.time() + backoff)
            print(f"\n{path} {params}: {reason}, pausing {backoff}s", file=sys.stderr)
        raise RuntimeError(f"giving up on {path} {params}")


def save(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(gzip.compress(content))
    tmp.replace(path)


def dat_item_ids(name):
    """auctionsim.dat's item ids for one faction (AuctionHouseId: 2 = Alliance, 6 = Horde)."""
    if not DAT.exists():
        return set()
    code = "6" if name == "horde" else "2"
    lines = DAT.read_text().splitlines()
    items, categories = map(int, lines[1].split())
    return {int(line.split(":", 2)[1]) for line in lines[2 + categories:2 + categories + items]
            if line.startswith(code + ":")}


def scrape(client, pool, name, refresh):
    faction = FACTIONS[name]
    out = RAW / name

    # Listing pages; page 1 says how many there are.
    def list_page(page):
        path = out / "list" / f"{page}.html.gz"
        if path.exists() and not refresh:
            return gzip.decompress(path.read_bytes())
        html = client.get("/realm/base", {"id": REALM, "faction": faction, "sort": "itemid",
                                          "page": page, "per-page": PER_PAGE})
        save(path, html)
        return html

    first = list_page(1)
    pages = -(-int(TOTAL_RE.search(first.decode()).group(1).replace(",", "")) // PER_PAGE)
    ids = set(map(int, ITEM_LINK_RE.findall(first.decode())))
    for html in pool.map(list_page, range(2, pages + 1)):
        ids.update(map(int, ITEM_LINK_RE.findall(html.decode())))
    listed = len(ids)
    ids |= dat_item_ids(name)
    print(f"{name}: {pages} listing pages, {listed} items listed, {len(ids)} ids with auctionsim.dat")

    todo = [i for i in sorted(ids) if refresh or not (out / "items" / f"{i}.html.gz").exists()]
    counts = {"done": 0, "found": 0}
    start = time.time()

    def item_page(item):
        html = client.get("/item/index", {"id": item, "faction": faction, "realm": REALM})
        save(out / "items" / f"{item}.html.gz", html)
        counts["done"] += 1
        counts["found"] += b"var all_data" in html
        rate = counts["done"] / (time.time() - start)
        print(f"{name}: {counts['done']}/{len(todo)} item pages, {counts['found']} with history,"
              f" {rate:.1f}/s, ~{(len(todo) - counts['done']) / rate / 60:.0f} min left",
              end="\r", flush=True)

    for _ in pool.map(item_page, todo):
        pass
    print(f"\n{name}: {len(ids) - len(todo)} already on disk, fetched {counts['done']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--faction", choices=list(FACTIONS), help="one faction only (default both)")
    ap.add_argument("--workers", type=int, default=2, help="concurrent connections (default 2)")
    ap.add_argument("--refresh", action="store_true", help="fetch pages already on disk again")
    args = ap.parse_args()
    client = Client()
    with ThreadPoolExecutor(args.workers) as pool:
        for name in FACTIONS:
            if not args.faction or name == args.faction:
                scrape(client, pool, name, args.refresh)


if __name__ == "__main__":
    main()
