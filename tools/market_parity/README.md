# Market mode parity harness

Runs the module's C++ `MarketEngine` on a market file with no server: empty house, no players, `Market.Scale` 0.1,
100 bots, 21 simulated days from 2026-07-06 00:00 UTC. It writes `parity_cpp_<faction id>.csv` (one row per item per
simulated day at 20:00 UTC) for `ml/s6_parity.py`, which compares them with `ml/s6_market_sim.py --empty-start` runs.
`stubs/` stands in for the few core headers the engine includes.

```
g++ -O2 -std=c++20 -I stubs -I ../../src parity.cpp ../../src/MarketData.cpp ../../src/MarketEngine.cpp -o parity
./parity ../../data/auctionsim_market.dat <output dir>
```

On the 2026-10-03 file, two Python runs with different seeds agree with each other as well as C++ agrees with Python
(per-item rank correlation: units 0.80, price 0.84-0.89), and posts, buyers and sales per faction match within 2%.

`fill_check.cpp` runs the same 21 days and then MARKET_FORMAT.md's Fill at the end, once on an empty house and once on
the 21-day house, and prints the sizes, items up and the fill's cost:

```
g++ -O2 -std=c++20 -I stubs -I ../../src fill_check.cpp ../../src/MarketData.cpp ../../src/MarketEngine.cpp -o fill_check
./fill_check ../../data/auctionsim_market.dat [scale, default 0.1]
```
