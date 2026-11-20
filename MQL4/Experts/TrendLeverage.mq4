//+------------------------------------------------------------------+
//|                                                TrendLeverage.mq4 |
//| Leveraged trend follower for index CFDs (US500, NAS100, ...).    |
//| Long at a target leverage while the last D1 close is above its   |
//| SMA, flat otherwise. Never adds to a losing position.            |
//+------------------------------------------------------------------+
#property strict
#property version   "1.00"
#property description "Long at a target leverage while the D1 close is above its SMA, flat otherwise."

// shortcut: no server-side stop loss, the position stays open while the terminal is offline; add one before running this anywhere that is not always on.

input int    SmaPeriod = 200;      // Trend filter: SMA length in D1 bars
input double Leverage  = 2.0;      // Target leverage of this chart (position value / account equity)
input double Band      = 0.10;     // Rebalance only when leverage is off target by more than this fraction
input int    Magic     = 20261120; // Identifies the orders of this EA
input int    Slippage  = 50;       // Maximum price deviation in points

int OnInit()
  {
   if(SmaPeriod < 2 || Leverage <= 0 || Band < 0)
      return(INIT_PARAMETERS_INCORRECT);
   return(INIT_SUCCEEDED);
  }

// Total lots of this EA's open buy orders on the chart symbol.
double OpenLots()
  {
   double lots = 0;
   for(int i = OrdersTotal() - 1; i >= 0; i--)
      if(OrderSelect(i, SELECT_BY_POS, MODE_TRADES) && OrderSymbol() == Symbol()
         && OrderMagicNumber() == Magic && OrderType() == OP_BUY)
         lots += OrderLots();
   return(lots);
  }

// Rounds down to the broker's lot step.
double RoundLots(double lots)
  {
   double step = MarketInfo(Symbol(), MODE_LOTSTEP);
   return(NormalizeDouble(MathFloor(lots / step + 1e-9) * step, 2));
  }

// Buys `lots`, split into several orders when the broker's maximum lot is smaller.
bool Buy(double lots)
  {
   double minLot = MarketInfo(Symbol(), MODE_MINLOT);
   double maxLot = MarketInfo(Symbol(), MODE_MAXLOT);
   while(lots >= minLot)
     {
      double part = MathMin(lots, maxLot);
      RefreshRates();
      if(OrderSend(Symbol(), OP_BUY, part, Ask, Slippage, 0, 0, "TrendLeverage", Magic, 0, clrGreen) < 0)
        {
         Print("OrderSend of ", part, " lots failed, error ", GetLastError());
         return(false);
        }
      lots = RoundLots(lots - part);
     }
   return(true);
  }

// Closes `lots` of this EA's buy orders, whole orders or parts of them.
bool Reduce(double lots)
  {
   double minLot = MarketInfo(Symbol(), MODE_MINLOT);
   for(int i = OrdersTotal() - 1; i >= 0 && lots > 1e-9; i--)
     {
      if(!OrderSelect(i, SELECT_BY_POS, MODE_TRADES) || OrderSymbol() != Symbol()
         || OrderMagicNumber() != Magic || OrderType() != OP_BUY)
         continue;
      double part = NormalizeDouble(MathMin(lots, OrderLots()), 2);
      if(OrderLots() - part < minLot - 1e-9)   // the broker rejects a partial close that leaves less than the minimum lot
         part = OrderLots();
      RefreshRates();
      if(!OrderClose(OrderTicket(), part, Bid, Slippage, clrRed))
        {
         Print("OrderClose of #", OrderTicket(), " failed, error ", GetLastError());
         return(false);
        }
      lots -= part;
     }
   return(true);
  }

void OnTick()
  {
   static datetime doneBar = 0;   // D1 bar that has already been handled
   static datetime retryAt = 0;   // after a failed order, wait instead of hammering the trade server
   datetime bar = iTime(Symbol(), PERIOD_D1, 0);
   if(bar == doneBar || TimeCurrent() < retryAt)
      return;

   double sma       = iMA(Symbol(), PERIOD_D1, SmaPeriod, 0, MODE_SMA, PRICE_CLOSE, 1);
   double last      = iClose(Symbol(), PERIOD_D1, 1);
   double tickValue = MarketInfo(Symbol(), MODE_TICKVALUE);
   double tickSize  = MarketInfo(Symbol(), MODE_TICKSIZE);
   double minLot    = MarketInfo(Symbol(), MODE_MINLOT);
   // History or market data not loaded yet: wait, a zero here would otherwise read as a signal.
   if(iBars(Symbol(), PERIOD_D1) <= SmaPeriod || sma <= 0 || last <= 0 || Ask <= 0
      || tickValue <= 0 || tickSize <= 0 || minLot <= 0
      || MarketInfo(Symbol(), MODE_LOTSTEP) <= 0 || MarketInfo(Symbol(), MODE_MAXLOT) <= 0)
      return;

   double have = OpenLots();
   double want = 0;               // lots that give the target leverage, 0 while the trend is down
   if(last > sma)
      want = Leverage * AccountEquity() / (Ask / tickSize * tickValue);

   bool ok = true;
   if(want <= 0)
      ok = Reduce(have);
   else if(have <= 0 || MathAbs(have - want) > Band * want)
     {
      double diff = RoundLots(MathAbs(want - have));
      if(diff >= minLot)
        {
         if(want > have)
            ok = Buy(diff);
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
