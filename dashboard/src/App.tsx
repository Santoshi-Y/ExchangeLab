import { useEffect, useMemo, useRef, useState } from "react";

type Side = "BUY" | "SELL";

type Order = {
  orderId: number;
  price: number;
  quantity: number;
  side: Side;
};

type BookMessage = {
  type: "book";
  symbol: string;
  sequence: number;
  hasBid: boolean;
  bestBid: number;
  bidQuantity: number;
  hasAsk: boolean;
  bestAsk: number;
  askQuantity: number;
};

type AddMessage = {
  type: "add";
  symbol: string;
  sequence: number;
  orderId: number;
  timestamp: number;
  price: number;
  quantity: number;
  side: Side;
};

type ExecuteMessage = {
  type: "execute";
  symbol: string;
  sequence: number;
  buyOrderId: number;
  sellOrderId: number;
  price: number;
  quantity: number;
};

type DeleteMessage = {
  type: "delete";
  symbol: string;
  sequence: number;
  orderId: number;
};

type MarketMessage =
  | BookMessage
  | AddMessage
  | ExecuteMessage
  | DeleteMessage;

type TopOfBook = {
  price: number;
  quantity: number;
};

type InstrumentView = {
  bestBid: TopOfBook | null;
  bestAsk: TopOfBook | null;
  orders: Map<number, Order>;
};

type TapeItem = {
  sequence: number;
  symbol: string;
  text: string;
};

function emptyInstrument(): InstrumentView {
  return {
    bestBid: null,
    bestAsk: null,
    orders: new Map(),
  };
}

function App() {
  const [connected, setConnected] = useState(false);
  const [instruments, setInstruments] = useState<Map<string, InstrumentView>>(
    new Map()
  );
  const [selectedSymbol, setSelectedSymbol] = useState("AAPL");
  const [tape, setTape] = useState<TapeItem[]>([]);
  const [lastSequence, setLastSequence] = useState(0);
  const reconnectTimer = useRef<number | null>(null);

  useEffect(() => {
    let socket: WebSocket | null = null;
    let cancelled = false;

    const connect = () => {
      if (cancelled) return;

      socket = new WebSocket("ws://127.0.0.1:8080");

      socket.onopen = () => setConnected(true);

      socket.onclose = () => {
        setConnected(false);

        if (!cancelled) {
          reconnectTimer.current = window.setTimeout(connect, 1000);
        }
      };

      socket.onerror = () => {
        socket?.close();
      };

      socket.onmessage = (event) => {
        const message = JSON.parse(event.data) as MarketMessage;
        setLastSequence(message.sequence);

        setInstruments((previous) => {
          const next = new Map(previous);
          const existing = next.get(message.symbol) ?? emptyInstrument();
          const instrument: InstrumentView = {
            bestBid: existing.bestBid,
            bestAsk: existing.bestAsk,
            orders: new Map(existing.orders),
          };

          if (message.type === "book") {
            instrument.bestBid = message.hasBid
              ? {
                  price: message.bestBid,
                  quantity: message.bidQuantity,
                }
              : null;

            instrument.bestAsk = message.hasAsk
              ? {
                  price: message.bestAsk,
                  quantity: message.askQuantity,
                }
              : null;
          } else if (message.type === "add") {
            instrument.orders.set(message.orderId, {
              orderId: message.orderId,
              price: message.price,
              quantity: message.quantity,
              side: message.side,
            });
          } else if (message.type === "execute") {
            for (const id of [message.buyOrderId, message.sellOrderId]) {
              const order = instrument.orders.get(id);

              if (!order) continue;

              const remaining = order.quantity - message.quantity;

              if (remaining > 0) {
                instrument.orders.set(id, {
                  ...order,
                  quantity: remaining,
                });
              }
            }
          } else {
            instrument.orders.delete(message.orderId);
          }

          next.set(message.symbol, instrument);
          return next;
        });

        if (message.type === "book") return;

        let text = "";

        if (message.type === "add") {
          text = `ADD ${message.side} #${message.orderId} ${message.quantity} @ ${message.price}`;
        } else if (message.type === "execute") {
          text = `TRADE ${message.quantity} @ ${message.price}  buy #${message.buyOrderId} / sell #${message.sellOrderId}`;
        } else {
          text = `DELETE #${message.orderId}`;
        }

        setTape((previous) =>
          [
            {
              sequence: message.sequence,
              symbol: message.symbol,
              text,
            },
            ...previous,
          ].slice(0, 60)
        );
      };
    };

    connect();

    return () => {
      cancelled = true;

      if (reconnectTimer.current !== null) {
        window.clearTimeout(reconnectTimer.current);
      }

      socket?.close();
    };
  }, []);

  const symbols = useMemo(
    () => [...instruments.keys()].sort(),
    [instruments]
  );

  useEffect(() => {
    if (
      symbols.length > 0 &&
      !instruments.has(selectedSymbol)
    ) {
      setSelectedSymbol(symbols[0]);
    }
  }, [instruments, selectedSymbol, symbols]);

  const selected = instruments.get(selectedSymbol) ?? emptyInstrument();

  const bids = useMemo(
    () =>
      [...selected.orders.values()]
        .filter((order) => order.side === "BUY")
        .sort((a, b) => b.price - a.price || a.orderId - b.orderId)
        .slice(0, 10),
    [selected]
  );

  const asks = useMemo(
    () =>
      [...selected.orders.values()]
        .filter((order) => order.side === "SELL")
        .sort((a, b) => a.price - b.price || a.orderId - b.orderId)
        .slice(0, 10),
    [selected]
  );

  const midpoint =
    selected.bestBid && selected.bestAsk
      ? ((selected.bestBid.price + selected.bestAsk.price) / 2).toFixed(2)
      : "—";

  return (
    <main className="shell">
      <header>
        <div>
          <p className="eyebrow">LOW-LATENCY MULTI-SYMBOL EXCHANGE SIMULATOR</p>
          <h1>ExchangeLab</h1>
        </div>

        <div className="status-group">
          <span className={`status ${connected ? "online" : "offline"}`}>
            <span className="dot" />
            {connected ? "WebSocket connected" : "Disconnected"}
          </span>
          <span className="sequence">SEQ {lastSequence || "—"}</span>
        </div>
      </header>

      <section className="symbol-bar">
        <span>Instrument</span>
        <div className="symbol-tabs">
          {symbols.length === 0 ? (
            <button className="symbol-tab active" type="button">
              Waiting for feed…
            </button>
          ) : (
            symbols.map((symbol) => (
              <button
                className={`symbol-tab ${symbol === selectedSymbol ? "active" : ""}`}
                key={symbol}
                onClick={() => setSelectedSymbol(symbol)}
                type="button"
              >
                {symbol}
              </button>
            ))
          )}
        </div>
      </section>

      <section className="metrics">
        <Metric
          label={`${selectedSymbol} Best Bid`}
          value={selected.bestBid ? `${selected.bestBid.price}` : "—"}
          sub={selected.bestBid ? `${selected.bestBid.quantity} units` : "No bid"}
        />
        <Metric
          label="Midpoint"
          value={midpoint}
          sub={selectedSymbol}
        />
        <Metric
          label={`${selectedSymbol} Best Ask`}
          value={selected.bestAsk ? `${selected.bestAsk.price}` : "—"}
          sub={selected.bestAsk ? `${selected.bestAsk.quantity} units` : "No ask"}
        />
        <Metric
          label="Visible L3 Orders"
          value={`${selected.orders.size}`}
          sub={`${symbols.length} instrument${symbols.length === 1 ? "" : "s"} observed`}
        />
      </section>

      <section className="grid">
        <div className="panel order-book">
          <div className="panel-title">
            <h2>{selectedSymbol} Level 3 Order Book</h2>
            <span>price-time feed</span>
          </div>

          <div className="book-columns">
            <BookSide title="Bids" orders={bids} />
            <BookSide title="Asks" orders={asks} />
          </div>
        </div>

        <div className="panel tape">
          <div className="panel-title">
            <h2>Cross-Symbol Event Tape</h2>
            <span>latest 60 events</span>
          </div>

          <div className="tape-list">
            {tape.length === 0 ? (
              <div className="empty">Waiting for market data…</div>
            ) : (
              tape.map((item) => (
                <div className="tape-row" key={`${item.sequence}-${item.symbol}-${item.text}`}>
                  <span>{item.sequence}</span>
                  <strong>{item.symbol}</strong>
                  <code>{item.text}</code>
                </div>
              ))
            )}
          </div>
        </div>
      </section>
    </main>
  );
}

function Metric({
  label,
  value,
  sub,
}: {
  label: string;
  value: string;
  sub: string;
}) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>{value}</strong>
      <small>{sub}</small>
    </div>
  );
}

function BookSide({
  title,
  orders,
}: {
  title: string;
  orders: Order[];
}) {
  return (
    <div>
      <div className="book-header">
        <span>{title}</span>
        <span>Qty</span>
        <span>Price</span>
      </div>

      {orders.length === 0 ? (
        <div className="empty">No orders</div>
      ) : (
        orders.map((order) => (
          <div className="book-row" key={order.orderId}>
            <span>#{order.orderId}</span>
            <span>{order.quantity}</span>
            <strong>{order.price}</strong>
          </div>
        ))
      )}
    </div>
  );
}

export default App;