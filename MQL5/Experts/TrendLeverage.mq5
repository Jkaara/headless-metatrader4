//+------------------------------------------------------------------+
//|                                                TrendLeverage.mq5 |
//| MetaTrader 5 version of MQL4/Experts/TrendLeverage.mq4.          |
//| Long at a target leverage while the last D1 close is above its   |
//| SMA, flat otherwise. Never adds to a losing position.            |
//+------------------------------------------------------------------+
#property version   "1.00"
#property description "Long at a target leverage while the D1 close is above its SMA, flat otherwise."

#include <Trade\Trade.mqh>

// shortcut: no server-side stop loss, the position stays open while the terminal is offline; add one before running this anywhere that is not always on.

input int    SmaPeriod = 200;      // Trend filter: SMA length in D1 bars
input double Leverage  = 2.0;      // Target leverage of this chart (position value / account equity)
input double Band      = 0.10;     // Rebalance only when leverage is off target by more than this fraction
input int    Magic     = 20261120; // Identifies the positions of this EA (hedging accounts only)
input int    Slippage  = 50;       // Maximum price deviation in points

CTrade trade;
bool   hedging;   // false: netting account, the symbol has a single position and the EA manages all of it

int OnInit()
  {
   if(SmaPeriod < 2 || Leverage <= 0 || Band < 0)
      return(INIT_PARAMETERS_INCORRECT);
   hedging = AccountInfoInteger(ACCOUNT_MARGIN_MODE) == ACCOUNT_MARGIN_MODE_RETAIL_HEDGING;
   trade.SetExpertMagicNumber((ulong)Magic);
   trade.SetDeviationInPoints((ulong)Slippage);
   trade.SetTypeFillingBySymbol(_Symbol);
   return(INIT_SUCCEEDED);
  }

// True if the position just selected by PositionGetTicket() is one this EA manages.
// A netting account merges every deal on the symbol into one position, so the magic number cannot tell them apart there.
bool Ours()
  {
   return(PositionGetString(POSITION_SYMBOL) == _Symbol && PositionGetInteger(POSITION_TYPE) == POSITION_TYPE_BUY
          && (!hedging || PositionGetInteger(POSITION_MAGIC) == Magic));
  }

// Total lots of the managed buy positions on the chart symbol.
double OpenLots()
  {
   double lots = 0;
   for(int i = PositionsTotal() - 1; i >= 0; i--)
      if(PositionGetTicket(i) != 0 && Ours())
         lots += PositionGetDouble(POSITION_VOLUME);
   return(lots);
  }

// SMA of the last SmaPeriod completed D1 closes, 0 while the history is not loaded.
double Sma()
  {
   double sum = 0;
   for(int i = 1; i <= SmaPeriod; i++)
     {
      double close = iClose(_Symbol, PERIOD_D1, i);
      if(close <= 0)
         return(0);
      sum += close;
     }
   return(sum / SmaPeriod);
  }

// Rounds down to the broker's lot step.
double RoundLots(double lots)
  {
   double step = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
   return(NormalizeDouble(MathFloor(lots / step + 1e-9) * step, 8));
  }

// CTrade reports true once a request was sent; whether the server executed it is in the return code.
bool Filled(bool sent)
  {
   uint code = trade.ResultRetcode();
   if(sent && (code == TRADE_RETCODE_DONE || code == TRADE_RETCODE_DONE_PARTIAL || code == TRADE_RETCODE_PLACED))
      return(true);
   Print("Trade request failed, return code ", code, " ", trade.ResultRetcodeDescription());
   return(false);
  }

// Sends market orders for `lots`, split into several when the broker's maximum lot is smaller.
bool MarketOrders(bool buy, double lots)
  {
   double minLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   double maxLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
   while(lots >= minLot)
     {
      double part = MathMin(lots, maxLot);
      bool   sent = false;
      if(buy)
         sent = trade.Buy(part, _Symbol, 0, 0, 0, "TrendLeverage");
      else
         sent = trade.Sell(part, _Symbol, 0, 0, 0, "TrendLeverage");
      if(!Filled(sent))
         return(false);
      lots = RoundLots(lots - part);
     }
   return(true);
  }

// Closes `lots` of the managed buy positions, whole positions or parts of them.
bool Reduce(double lots)
  {
   double minLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   if(!hedging)
     {
      // Netting: a sell order shrinks the symbol's single position.
      double position = OpenLots();
      if(position - lots < minLot - 1e-9)   // never sell more than is held, that opens a short, nor leave less than the minimum lot
         lots = position;
      return(MarketOrders(false, lots));
     }
   for(int i = PositionsTotal() - 1; i >= 0 && lots > 1e-9; i--)
     {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0 || !Ours())
         continue;
      double held = PositionGetDouble(POSITION_VOLUME);
      double part = NormalizeDouble(MathMin(lots, held), 8);
      bool   sent = false;
      if(held - part < minLot - 1e-9)   // the broker rejects a partial close that leaves less than the minimum lot
        {
         part = held;
         sent = trade.PositionClose(ticket);
        }
      else
         sent = trade.PositionClosePartial(ticket, part);
      if(!Filled(sent))
         return(false);
      lots -= part;
     }
   return(true);
  }

void OnTick()
  {
   static datetime doneBar = 0;   // D1 bar that has already been handled
   static datetime retryAt = 0;   // after a failed order, wait instead of hammering the trade server
   datetime bar = iTime(_Symbol, PERIOD_D1, 0);
   if(bar == doneBar || TimeCurrent() < retryAt)
      return;

   double sma       = Sma();
   double last      = iClose(_Symbol, PERIOD_D1, 1);
   double ask       = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   double tickValue = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_VALUE);
   double tickSize  = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_TICK_SIZE);
   double minLot    = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   // History or market data not loaded yet: wait, a zero here would otherwise read as a signal.
   if(sma <= 0 || last <= 0 || ask <= 0 || tickValue <= 0 || tickSize <= 0 || minLot <= 0
      || SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP) <= 0 || SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX) <= 0)
      return;

   double have = OpenLots();
   double want = 0;               // lots that give the target leverage, 0 while the trend is down
   if(last > sma)
      want = Leverage * AccountInfoDouble(ACCOUNT_EQUITY) / (ask / tickSize * tickValue);

   bool ok = true;
   if(want <= 0)
      ok = Reduce(have);
   else if(have <= 0 || MathAbs(have - want) > Band * want)
     {
      double diff = RoundLots(MathAbs(want - have));
      if(diff >= minLot)
        {
         if(want > have)
            ok = MarketOrders(true, diff);
         else
            ok = Reduce(diff);
        }
     }
   if(ok)
      doneBar = bar;
   else
      retryAt = TimeCurrent() + 60;
   Comment("TrendLeverage: D1 close ", last, (last > sma ? " > " : " <= "), "SMA", SmaPeriod, " ", sma, ", lots ", OpenLots());
  }
