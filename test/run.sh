#!/bin/sh
# Checks the EAs without MetaTrader: builds each real source against API stubs (see ea_test.cpp) and runs its self-test.
# With the backtest data present it also replays the Nasdaq-100 history and requires each EA to match backtest.py.
set -e
cd "$(dirname "$0")"
[ -d ../backtest/data ] && expected=$(python3 ../backtest/backtest.py --dump replay.csv)

check() {   # $1 = EA source, $2 = compiler flag selecting its API stubs, rest = account modes to run
    src=$1 flag=$2
    shift 2
    grep -v '^#property\|^#include' "$src" > ea.inc
    g++ -std=c++17 -Wall -Wextra $flag -o ea_test ea_test.cpp
    for mode in "$@"; do
        ./ea_test $mode
        [ -z "$expected" ] || ./ea_test $mode replay.csv "$expected"
    done
}
check ../MQL4/Experts/TrendLeverage.mq4 "" ""
check ../MQL5/Experts/TrendLeverage.mq5 -DMT5 "" netting
