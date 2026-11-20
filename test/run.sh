#!/bin/sh
# Checks the EA without MetaTrader: builds the real .mq4 against API stubs (see ea_test.cpp) and runs its self-test.
# With the backtest data present it also replays the Nasdaq-100 history and requires the EA to match backtest.py.
set -e
cd "$(dirname "$0")"
grep -v '^#property' ../MQL4/Experts/TrendLeverage.mq4 > ea.inc
g++ -std=c++17 -Wall -Wextra -o ea_test ea_test.cpp
./ea_test
if [ -d ../backtest/data ]; then
    ./ea_test replay.csv "$(python3 ../backtest/backtest.py --dump replay.csv)"
fi
