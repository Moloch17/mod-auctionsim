#!/usr/bin/env python3
"""Stage 0: AzerothCore's item_template -> out/item_template.parquet.

Reads data/sql/base/db_world/item_template.sql under AC_ROOT (default: the
AzerothCore tree this module sits in, i.e. ../..; set AC_ROOT when ml/ runs
from a checkout outside modules/). Keeps what the simulator needs: vendor
sell price (deposits and the price floor), stack size and item class.
"""

import re
import sys

import polars as pl

from common import ITEM_TEMPLATE_SQL, OUT

KEEP = {"entry": "item", "class": "class", "subclass": "subclass", "name": "name", "Quality": "quality",
        "BuyPrice": "buy_price", "SellPrice": "sell_price", "stackable": "stackable", "ItemLevel": "item_level",
        "RequiredLevel": "required_level"}
VALUE = re.compile(r"'((?:[^'\\]|\\.)*)'|(-?[\d.]+|NULL)")


def main():
    if not ITEM_TEMPLATE_SQL.exists():
        sys.exit(f"{ITEM_TEMPLATE_SQL} not found; set AC_ROOT to the AzerothCore source tree")
    sql = ITEM_TEMPLATE_SQL.read_text(encoding="utf-8")
    create = re.search(r"CREATE TABLE `item_template` \((.*?)\n\)", sql, re.S).group(1)
    cols = re.findall(r"^\s*`(\w+)`", create, re.M)
    idx = {c: cols.index(c) for c in KEEP}
    rows = []
    for insert in re.finditer(r"INSERT INTO `item_template` VALUES\s*(.*?);\n", sql, re.S):
        body = insert.group(1)
        i = 0
        while (start := body.find("(", i)) != -1:
            vals, j = [], start + 1
            while True:
                m = VALUE.match(body, j)
                vals.append(m.group(1) if m.group(1) is not None else m.group(2))
                j = m.end()
                if body[j] == ")":
                    break
                j += 1  # comma
            rows.append([vals[idx[c]] for c in KEEP])
            i = j + 1
    df = pl.DataFrame(rows, schema=list(KEEP.values()), orient="row").with_columns(
        [pl.col(c).cast(pl.Int64) for c in KEEP.values() if c != "name"])
    df.write_parquet(OUT / "item_template.parquet")
    print(f"{df.height} items, {df.filter(pl.col('sell_price') > 0).height} with a vendor price")


if __name__ == "__main__":
    main()
