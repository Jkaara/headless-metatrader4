// Compiles a real TrendLeverage EA against minimal stubs of the terminal's API and feeds it D1 bars:
// MQL4/Experts/TrendLeverage.mq4 by default, MQL5/Experts/TrendLeverage.mq5 when built with -DMT5.
// MetaEditor does not run on Linux, so this is the only automated check of the EAs' logic. Run it with ./run.sh
//   ./ea_test [netting]                      self-test on a synthetic price path
//   ./ea_test [netting] replay.csv EXPECTED  replays "close,financing" rows and compares the final equity multiple
// "netting" simulates an MT5 account that keeps one position per symbol; the default keeps positions separate.
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

// --- simulated broker: one symbol, 1 lot = 1 index unit, fills at the quoted price ---
struct Order { int ticket; double lots; int magic; };
static std::vector<double> closes;   // closes[d] = close of D1 bar d
static std::vector<Order> orders;    // open buy positions
static int day, selected, tickets, failNext, dropNext, sends;   // failNext: opens to refuse, dropNext: closes to lose
static bool netting;
static datetime now;
static double equity, maxLot, Ask, Bid, leverage;   // leverage: right after the EA acted on the bar's open

static bool openBuy(double lots, int magic) {   // false: the server refused the request
  sends++;
  if (failNext > 0) { failNext--; return false; }
  assert(lots >= 0.01 && lots <= maxLot);
  if (netting && !orders.empty()) orders[0].lots += lots;
  else orders.push_back({++tickets, lots, magic});
  return true;
}
static bool closeBuy(int ticket, double lots) {
  for (size_t i = 0; i < orders.size(); i++)
    if (orders[i].ticket == ticket) {
      assert(lots > 0 && lots <= orders[i].lots + 1e-9);
      orders[i].lots -= lots;
      if (orders[i].lots < 1e-9) orders.erase(orders.begin() + i);
      return true;
    }
  return false;
}
static double mean(int period, int shift) {     // SMA of `period` closes ending `shift` bars back
  double sum = 0;
  for (int i = 0; i < period; i++) sum += closes[day - shift - i];
  return sum / period;
}

// --- API both languages share ---
datetime TimeCurrent() { return now; }
datetime iTime(string, int, int shift) { return (day - shift) * 86400LL; }
double iClose(string, int, int shift) { return shift > day ? 0 : closes[day - shift]; }
double NormalizeDouble(double v, int digits) { double p = std::pow(10, digits); return std::round(v * p) / p; }
double MathFloor(double v) { return std::floor(v); }
double MathMin(double a, double b) { return a < b ? a : b; }
double MathAbs(double v) { return std::fabs(v); }
double MathMax(double a, double b) { return a > b ? a : b; }
double MathSqrt(double v) { return std::sqrt(v); }
template <class... A> void Print(A...) {}
template <class... A> void Comment(A...) {}

#ifndef MT5
enum { PERIOD_D1, MODE_SMA, PRICE_CLOSE, SELECT_BY_POS, MODE_TRADES, OP_BUY, clrGreen, clrRed,
       MODE_LOTSTEP, MODE_MINLOT, MODE_MAXLOT, MODE_TICKVALUE, MODE_TICKSIZE,
       INIT_SUCCEEDED = 0, INIT_PARAMETERS_INCORRECT = 32767 };
string Symbol() { return "SIM"; }
int iBars(string, int) { return day + 1; }
double iMA(string, int, int period, int, int, int, int shift) { return shift + period - 1 > day ? 0 : mean(period, shift); }
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
int OrderSend(string, int, double lots, double, int, double, double, string, int magic, datetime, int) { return openBuy(lots, magic) ? tickets : -1; }
bool OrderClose(int ticket, double lots, double, int, int) {
  if (dropNext > 0) { dropNext--; return false; }
  return closeBuy(ticket, lots);
}
#else
#define ulong unsigned long long   // macros, because glibc already has typedefs with these names
#define uint unsigned
enum { PERIOD_D1, POSITION_SYMBOL, POSITION_MAGIC, POSITION_TYPE, POSITION_VOLUME, POSITION_TYPE_BUY,
       SYMBOL_ASK, SYMBOL_VOLUME_STEP, SYMBOL_VOLUME_MIN, SYMBOL_VOLUME_MAX, SYMBOL_TRADE_TICK_VALUE, SYMBOL_TRADE_TICK_SIZE,
       ACCOUNT_EQUITY, ACCOUNT_MARGIN_MODE, ACCOUNT_MARGIN_MODE_RETAIL_NETTING, ACCOUNT_MARGIN_MODE_RETAIL_HEDGING,
       TRADE_RETCODE_REJECT = 10006, TRADE_RETCODE_PLACED = 10008, TRADE_RETCODE_DONE = 10009, TRADE_RETCODE_DONE_PARTIAL = 10010,
       INIT_SUCCEEDED = 0, INIT_PARAMETERS_INCORRECT = 32767 };
static const string _Symbol = "SIM";
double SymbolInfoDouble(string, int what) { return what == SYMBOL_ASK ? Ask : what == SYMBOL_VOLUME_MAX ? maxLot : 0.01; }
double AccountInfoDouble(int) { return equity; }
long long AccountInfoInteger(int) { return netting ? ACCOUNT_MARGIN_MODE_RETAIL_NETTING : ACCOUNT_MARGIN_MODE_RETAIL_HEDGING; }
int PositionsTotal() { return orders.size(); }
ulong PositionGetTicket(int i) { selected = i; return i >= 0 && i < (int)orders.size() ? orders[i].ticket : 0; }
string PositionGetString(int) { return "SIM"; }
long long PositionGetInteger(int what) { return what == POSITION_MAGIC ? orders[selected].magic : POSITION_TYPE_BUY; }
double PositionGetDouble(int) { return orders[selected].lots; }
// Like the real class: a method returns true once its request was sent, the server's verdict is in ResultRetcode(),
// and a method that gives up before sending leaves the previous return code in place.
struct CTrade {
  uint code = 0;
  int magic = 0;
  bool sent(bool accepted) { code = accepted ? TRADE_RETCODE_DONE : TRADE_RETCODE_REJECT; return true; }
  double held(ulong ticket) { for (auto &o : orders) if ((ulong)o.ticket == ticket) return o.lots; return 0; }
  void SetExpertMagicNumber(ulong m) { magic = m; }
  void SetDeviationInPoints(ulong) {}
  bool SetTypeFillingBySymbol(string) { return true; }
  uint ResultRetcode() { return code; }
  string ResultRetcodeDescription() { return ""; }
  bool Buy(double lots, string, double, double, double, string) { return sent(openBuy(lots, magic)); }
  bool lost() { return dropNext > 0 && dropNext--; }   // gave up before sending: the old return code still says done
  bool Sell(double lots, string, double, double, double, string) {
    assert(netting && orders.size() == 1);   // on a hedging account this would open a short instead of reducing the long
    assert(lots <= maxLot);                  // one order cannot exceed the maximum lot, not even to close
    return lost() ? false : sent(closeBuy(orders[0].ticket, lots));
  }
  bool PositionClose(ulong ticket) {
    assert(!netting && held(ticket) <= maxLot);
    return lost() ? false : sent(closeBuy(ticket, held(ticket)));
  }
  bool PositionClosePartial(ulong ticket, double lots) { return netting ? false : sent(closeBuy(ticket, lots)); }
};
#endif

#include "ea.inc"   // the EA source without its #property and #include lines, written by run.sh

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

// What the EA's target leverage should be on the current bar, worked out separately from the EA's own formula.
static double targetLeverage() {
  double changes[VOL_DAYS], average = 0, squares = 0;
  for (int i = 0; i < VOL_DAYS; i++) average += (changes[i] = closes[day - 1 - i] / closes[day - 2 - i] - 1) / VOL_DAYS;
  for (double c : changes) squares += (c - average) * (c - average);
  double volatility = std::sqrt(squares / (VOL_DAYS - 1) * 252);
  return volatility > VolLimit ? Leverage * VolLimit / volatility : Leverage;
}

static void selftest() {
  closes.assign(200, 100.0);                                  // flat: no trend
  for (int i = 1; i <= 60; i++) closes.push_back(100 + i);   // calm rally above the SMA
  for (int i = 0; i < 30; i++) closes.push_back(closes.back() * (i % 2 ? 0.97 : 1.05));   // still rising, in wild swings
  for (int i = 1; i <= 120; i++) closes.push_back(closes.back() - 1);                      // slide back below the SMA
  equity = 100000; maxLot = 1000;
  orders = {{++tickets, 5.0, 1}};                             // somebody else's position on the symbol
  assert(OnInit() == INIT_SUCCEEDED);

  int rebalancedUp = 0, rebalancedDown = 0, scaledDown = 0;
  for (day = 1; day < (int)closes.size();) {
    bool entry = day == 201;                                  // first close above the SMA
    if (entry) failNext = 1;                                  // the first order fails, the EA has to retry
    double before = eaLots();
    bool trendUp = day >= SmaPeriod && closes[day - 1] > mean(SmaPeriod, 1);
    double target = trendUp ? targetLeverage() : 0;
    bool exit = before > 0 && !trendUp;                       // first close below the SMA
    if (exit) dropNext = 1;                                   // the first close request is lost, the EA has to retry
    bar(0, entry || exit ? 2 : 1);
    double lots = eaLots();
    assert((lots > 0) == trendUp);                            // position follows the signal on every bar
    if (trendUp) assert(leverage > target * (1 - Band) - 0.01 && leverage < target * (1 + Band) + 0.01);
    // 1000 + 980.19 lots after one refused request; a netting account holds them as one position
    if (entry) assert(std::fabs(lots - 1980.19) < 1e-6 && sends == 3 && orders.size() == (netting ? 1u : 3u));
    if (before > 0 && lots > before) rebalancedUp++;
    if (lots > 0 && lots < before) rebalancedDown++;
    if (trendUp && target < Leverage / 2) scaledDown++;
  }
  assert(rebalancedUp > 0 && rebalancedDown > 0);             // both directions of the band were exercised
  assert(scaledDown > 0);                                     // and so was the volatility limit
  // Separate positions: the foreign one is untouched. Netting: it was the symbol's position, so the EA closed it when flat.
  if (netting) assert(orders.empty());
  else assert(orders.size() == 1 && orders[0].magic == 1 && orders[0].lots == 5.0);
  printf("self-test passed%s (%d order requests, final equity %.0f)\n", netting ? ", netting" : "", sends, equity);
}

int main(int argc, char **argv) {
  if (argc > 1 && string(argv[1]) == "netting") { netting = true; argv++; argc--; }
  if (argc < 3) { selftest(); return 0; }
  FILE *f = fopen(argv[1], "r");
  assert(f);
  std::vector<double> financing;
  for (double c, fin; fscanf(f, "%lf,%lf", &c, &fin) == 2;) { closes.push_back(c); financing.push_back(fin); }
  fclose(f);
  equity = 1e9; maxLot = 1e12;                                // large account: lot rounding does not matter
  assert(OnInit() == INIT_SUCCEEDED);
  for (day = 1; day < (int)closes.size();) bar(financing[day]);
  double got = equity / 1e9, expected = atof(argv[2]);
  printf("replay of %zu bars%s: EA %.4f, backtest %.4f\n", closes.size(), netting ? ", netting" : "", got, expected);
  assert(std::fabs(got / expected - 1) < 0.005);
  return 0;
}
