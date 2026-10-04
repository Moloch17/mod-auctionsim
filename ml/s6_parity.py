#!/usr/bin/env python3
"""Stage 6e: does the module's C++ Market engine behave like s6_market_sim?

Both run the shipped auctionsim_market.dat from an empty house at the same
Market.Scale and start time, with different random streams, and write the
house once a simulated day (the C++ through its test harness, as
out/s6/parity_cpp_<faction id>.csv). After the house has filled (day >= FROM_DAY)
they are compared: totals per day, and per item the median price and units --
median log ratio C++/Python and rank correlation across items.

  ./s6_parity.py
"""

import numpy as np
import polars as pl

from common import OUT

S6 = OUT / "s6"
FROM_DAY = 7


def summary(df):
    return df.group_by("item").agg(units=pl.col("units").median(), price=pl.col("med_unit").median(),
                                   spread=(pl.col("min_unit") / pl.col("med_unit")).median(), days=pl.len())


def main():
    lines = []
    for faction, fid in (("horde", 6), ("alliance", 2)):
        py = (pl.read_parquet(S6 / f"sim_parity_py_{faction}_daily.parquet")
              .with_columns(pl.col("item").cast(pl.Int64), pl.col("day").cast(pl.Int64)))
        cpp = (pl.read_csv(S6 / f"parity_cpp_{fid}.csv")
               .with_columns(pl.col("item").cast(pl.Int64), pl.col("day").cast(pl.Int64)))
        py, cpp = py.filter(pl.col("day") >= FROM_DAY), cpp.filter(pl.col("day") >= FROM_DAY)
        totals = {name: {"listings_per_day": float(d.group_by("day").agg(pl.col("listings").first())["listings"].median())
                         if "listings" in d.columns else
                         float(d.group_by("day").agg(pl.col("listings_total").first())["listings_total"].median()),
                         "items_per_day": float(d.group_by("day").len()["len"].median()),
                         "units_per_day": float(d.group_by("day").agg(pl.col("units").sum())["units"].median())}
                  for name, d in (("python", py), ("cpp", cpp))}
        j = summary(py).join(summary(cpp), on="item", suffix="_cpp")
        out = {}
        for m in ("units", "price", "spread", "days"):
            a, b = j[f"{m}_cpp"].cast(pl.Float64).to_numpy(), j[m].cast(pl.Float64).to_numpy()
            ok = (a > 0) & (b > 0)
            out[m] = {"median_log_cpp_over_py": float(np.median(np.log(a[ok] / b[ok]))),
                      "rank_corr": float(np.corrcoef(pl.Series(a[ok]).rank().to_numpy(),
                                                     pl.Series(b[ok]).rank().to_numpy())[0, 1])}
        lines.append(f"{faction}: items in both {j.height}")
        lines.append(f"  totals {totals}")
        for m, v in out.items():
            lines.append(f"  {m:7s} median log(C++/Python) {v['median_log_cpp_over_py']:+.3f}, "
                         f"rank corr {v['rank_corr']:.3f}")
    report = "\n".join(lines)
    (S6 / "parity_report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
