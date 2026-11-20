// Compiles the real MQL4/Experts/TrendLeverage.mq4 against minimal stubs of the MT4 API and feeds it D1 bars.
// MetaEditor does not run on Linux, so this is the only automated check of the EA's logic. Run it with ./run.sh
//   ./ea_test                      self-test on a synthetic price path
//   ./ea_test replay.csv EXPECTED  replays "close,financing" rows and compares the final equity multiple
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

typedef std::string string;
typedef long long datetime;
#define input
enum { PERIOD_D1, MODE_SMA, PRICE_CLOSE, SELECT_BY_POS, MODE_TRADES, OP_BUY, clrGreen, clrRed,
       MODE_LOTSTEP, MODE_MINLOT, MODE_MAXLOT, MODE_TICKVALUE, MODE_TICKSIZE,
       INIT_SUCCEEDED = 0, INIT_PARAMETERS_INCORRECT = 32767 };

// --- simulated broker: one symbol, 1 lot = 1 index unit, fills at the quoted price ---
struct Order { int ticket; double lots; int magic; };
static std::vector<double> closes;   // closes[d] = close of D1 bar d
static std::vector<Order> orders;
static int day, selected, tickets, failNext, sends;
static datetime now;
static double equity, maxLot, Ask, Bid, leverage;   // leverage: right after the EA acted on the bar's open

string Symbol() { return "SIM"; }
datetime TimeCurrent() { return now; }
datetime iTime(string, int, int shift) { return (day - shift) * 86400LL; }
int iBars(string, int) { return day + 1; }
double iClose(string, int, int shift) { return closes[day - shift]; }
double iMA(string, int, int period, int, int, int, int shift) {
  if (day - shift - period + 1 < 0) return 0;
  double sum = 0;
  for (int i = 0; i < period; i++) sum += closes[day - shift - i];
  return sum / period;
}
double MarketInfo(string, int what) {
  if (what == MODE_LOTSTEP || what == MODE_MINLOT || what == MODE_TICKSIZE || what == MODE_TICKVALUE) return 0.01;
  return what == MODE_MAXLOT ? maxLot : 0;
}
double AccountEquity() { return equity; }
int OrdersTotal() { return orders.size(); }
bool OrderSelect(int i, int, int) { selected = i; return i >= 0 && i < (int)orders.size(); }
string OrderSymbol() { return "SIM"; }
int OrderMagicNumber() { return orders[selected].magic; }
int OrderType() { return OP_BUY; }
double OrderLots() { return orders[selected].lots; }
int OrderTicket() { return orders[selected].ticket; }
bool RefreshRates() { return true; }
int GetLastError() { return 0; }
int OrderSend(string, int, double lots, double, int, double, double, string, int magic, datetime, int) {
  sends++;
  if (failNext > 0) { failNext--; return -1; }
  assert(lots >= 0.01 && lots <= maxLot);
  orders.push_back({++tickets, lots, magic});
  return tickets;
}
bool OrderClose(int ticket, double lots, double, int, int) {
  for (size_t i = 0; i < orders.size(); i++)
    if (orders[i].ticket == ticket) {
      assert(lots > 0 && lots <= orders[i].lots + 1e-9);
      orders[i].lots -= lots;
      if (orders[i].lots < 1e-9) orders.erase(orders.begin() + i);
      return true;
    }
  return false;
}
double NormalizeDouble(double v, int digits) { double p = std::pow(10, digits); return std::round(v * p) / p; }
double MathFloor(double v) { return std::floor(v); }
double MathMin(double a, double b) { return a < b ? a : b; }
double MathAbs(double v) { return std::fabs(v); }
template <class... A> void Print(A...) {}
template <class... A> void Comment(A...) {}

#include "ea.inc"   // the EA source without its #property lines, written by run.sh

static double eaLots() {
  double lots = 0;
  for (auto &o : orders) if (o.magic == Magic) lots += o.lots;
  return lots;
}

// One D1 bar: it opens at the previous close, the EA gets ticks 61 s apart, then the bar's move and financing are booked.
static void bar(double financing = 0, int ticks = 1) {
  Ask = Bid = closes[day - 1];
  for (int i = 0; i < ticks; i++) { now = day * 86400LL + i * 61; OnTick(); }
  leverage = eaLots() * Ask / equity;
  equity += eaLots() * (closes[day] - closes[day - 1] - closes[day - 1] * financing);
  day++;
}

static void selftest() {
  closes.assign(200, 100.0);                                  // flat: no trend
  for (int i = 1; i <= 60; i++) closes.push_back(100 + i);   // rally above the SMA
  for (int i = 1; i <= 80; i++) closes.push_back(160 - i);   // slide back below it
  equity = 100000; maxLot = 1000;
  orders = {{++tickets, 5.0, 1}};                             // somebody else's order, must stay untouched
  assert(OnInit() == INIT_SUCCEEDED);

  int rebalancedUp = 0, rebalancedDown = 0;
  for (day = 1; day < (int)closes.size();) {
    bool entry = day == 201;                                  // first close above the SMA
    if (entry) failNext = 1;                                  // the first order fails, the EA has to retry
    double before = eaLots(), sma = iMA("", 0, SmaPeriod, 0, 0, 0, 1);
    bool trendUp = day >= SmaPeriod && closes[day - 1] > sma;
    bar(0, entry ? 2 : 1);
    double lots = eaLots();
    assert((lots > 0) == trendUp);                            // position follows the signal on every bar
    if (trendUp) assert(leverage > Leverage * (1 - Band) - 0.01 && leverage < Leverage * (1 + Band) + 0.01);
    if (entry) assert(std::fabs(lots - 1980.19) < 1e-6 && orders.size() == 3 && sends == 3);   // 1000 + 980.19 after one failure
    if (before > 0 && lots > before) rebalancedUp++;
    if (lots > 0 && lots < before) rebalancedDown++;
  }
  assert(rebalancedUp > 0 && rebalancedDown > 0);             // both directions of the band were exercised
  assert(orders.size() == 1 && orders[0].magic == 1 && orders[0].lots == 5.0);
  printf("self-test passed (%d order requests, final equity %.0f)\n", sends, equity);
}

int main(int argc, char **argv) {
  if (argc < 3) { selftest(); return 0; }
  FILE *f = fopen(argv[1], "r");
  assert(f);
  std::vector<double> financing;
  for (double c, fin; fscanf(f, "%lf,%lf", &c, &fin) == 2;) { closes.push_back(c); financing.push_back(fin); }
  fclose(f);
  equity = 1e9; maxLot = 1e12;                                // large account: lot rounding does not matter
  for (day = 1; day < (int)closes.size();) bar(financing[day]);
  double got = equity / 1e9, expected = atof(argv[2]);
  printf("replay of %zu bars: EA %.4f, backtest %.4f\n", closes.size(), got, expected);
  assert(std::fabs(got / expected - 1) < 0.005);
  return 0;
}
