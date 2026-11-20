#!/usr/bin/env python3
"""Backtest of the TrendLeverage EA (MQL4/Experts, MQL5/Experts) on daily index closes, compared with buying and holding the index.

Needs pandas and pyarrow, and three public data repositories cloned into ./data (commands in the README):
  us-market-data     SteelCerberus/us-market-data             S&P 500 since 1928 with dividends, T-bill rate
  stock              ChengYan7/stock                          Nasdaq-100 price index since 1985
  us-market-data-2y  sungholee0221-ops/us-market-data (data)  last two years, extends the S&P 500 to the present
"""
import argparse
import pathlib

import numpy as np
import pandas as pd

# Financing: (markup over the T-bill rate paid on the position value, share of the T-bill rate earned on the account).
SCENARIOS = {
    "EA, typical cash CFD (T-bill + 2.5 %, no interest on cash)": (0.025, 0.0),
    "EA, cheap financing (T-bill + 0.5 %, cash earns T-bill)": (0.005, 1.0),
}
COST = 0.0002  # spread and slippage per traded position value


def load(data):
    """Per index: px = price index (what the EA sees), ret = daily return of a long position, rf = T-bill rate."""
    sc = pd.read_csv(data / "us-market-data/data/us_market_data.csv", index_col=0, parse_dates=True).loc["1928":]
    ndx = pd.read_csv(data / "stock/data/processed/ndx_daily.csv", index_col=0, parse_dates=True)["close"]
    rec = pd.read_parquet(data / "us-market-data-2y/prices.parquet")
    rec = rec.assign(date=pd.to_datetime(rec["date"])).pivot(index="date", columns="ticker", values="close")
    assert rec.index[0] <= sc.index[-1], "gap between the S&P 500 history and its two-year extension"
    new = rec.pct_change().loc[lambda r: (r.index > sc.index[-1]) & (r.index <= ndx.index[-1])]
    price = pd.concat([sc["Close"].pct_change(), new["^GSPC"]]).fillna(0)
    total = pd.concat([sc["Adjusted Close"].pct_change(), new["SPY"]]).fillna(0)
    rf = sc["Risk Free Rate"] / 100  # carried forward where an index runs past the end of this source
    out = {
        "S&P 500": pd.DataFrame({"px": (1 + price).cumprod(), "ret": total, "rf": rf.reindex(price.index).ffill()}),
        # No dividend data for the Nasdaq-100: they are left out of the EA and of buy and hold alike.
        "Nasdaq-100": pd.DataFrame({"px": ndx, "ret": ndx.pct_change().fillna(0), "rf": rf.reindex(ndx.index).ffill()}),
    }
    for d in out.values():
        assert d.notna().all().all() and (d.px > 0).all() and d.index.is_monotonic_increasing and d.index.is_unique
    return out


def run(d, sma, lev, band, markup, cash, cost=COST):
    """Equity curve of the EA's rules: target leverage `lev` while the previous close is above its SMA, else flat."""
    long = (d.px > d.px.rolling(sma).mean()).shift(1, fill_value=False).to_numpy()
    days = d.index.to_series().diff().dt.days.fillna(1).to_numpy()
    eq, e, n = np.zeros(len(d)), 1.0, 0.0  # e = equity, n = position value
    for t, (r, f) in enumerate(zip(d.ret.to_numpy(), d.rf.to_numpy())):
        target = lev * e if long[t] else 0.0
        if n != target and (target == 0 or n == 0 or abs(n / e - lev) > band * lev):  # same rule as the EA
            e -= cost * abs(target - n)
            n = target
        e = e * (1 + cash * f * days[t] / 360) + n * (r - (f + markup) * days[t] / 360)
        if e <= 0:  # wiped out, the curve stays at 0
            break
        n *= 1 + r
        eq[t] = e
    return pd.Series(eq, d.index)


def stats(eq):
    years = (eq.index[-1] - eq.index[0]).days / 365.25
    return f"{((eq.iloc[-1] / eq.iloc[0]) ** (1 / years) - 1) * 100:5.1f} % / {(eq / eq.cummax() - 1).min() * 100:4.0f} %"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--data", type=pathlib.Path, default=pathlib.Path(__file__).parent / "data")
    p.add_argument("--sma", type=int, default=200, help="EA input SmaPeriod")
    p.add_argument("--leverage", type=float, default=2.0, help="EA input Leverage")
    p.add_argument("--band", type=float, default=0.10, help="EA input Band")
    p.add_argument("--dump", help="for test/run.sh: write a replay file for the real EA and print the result it must reach")
    args = p.parse_args()
    data = load(args.data)

    if args.dump:
        d, (markup, cash) = data["Nasdaq-100"], next(iter(SCENARIOS.values()))
        days = d.index.to_series().diff().dt.days.fillna(1)
        pd.DataFrame({"close": d.px, "fin": (d.rf + markup) * days / 360}).to_csv(args.dump, index=False, header=False)
        print(run(d, args.sma, args.leverage, args.band, markup, cash, cost=0).iloc[-1])
        return

    for name, d in data.items():
        curves = {"Buy and hold": (1 + d.ret).cumprod()}
        for label, (markup, cash) in SCENARIOS.items():
            curves[label] = run(d, args.sma, args.leverage, args.band, markup, cash)
        end = d.index[-1]
        periods = {"all": d.index[args.sma], "since 2000": "2000", "since 2010": "2010",
                   "last 10 years": end - pd.DateOffset(years=10), "last 5 years": end - pd.DateOffset(years=5)}
        table = pd.DataFrame({k: {label: stats(eq.loc[start:]) for label, eq in curves.items()} for k, start in periods.items()})
        print(f"\n{name}, {d.index[args.sma].date()} to {end.date()}: return per year / deepest drawdown")
        print(table.to_string())


if __name__ == "__main__":
    main()
