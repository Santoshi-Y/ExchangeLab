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
  sequence: number;
  orderId: number;
  timestamp: number;
  price: number;
  quantity: number;
  side: Side;
};

type ExecuteMessage = {
  type: "execute";
  sequence: number;
  buyOrderId: number;
  sellOrderId: number;
  price: number;
  quantity: number;
};

type DeleteMessage = {
  type: "delete";
  sequence: number;
  orderId: number;
};

type MarketMessage =
  | BookMessage
  | AddMessage
  | ExecuteMessage
  | DeleteMessage;

type TapeItem = {
  sequence: number;
  text: string;
};

function App() {
  const [connected, setConnected] = useState(false);
  const [bestBid, setBestBid] = useState<{ price: number; quantity: number } | null>(null);
  const [bestAsk, setBestAsk] = useState<{ price: number; quantity: number } | null>(null);
  const [orders, setOrders] = useState<Map<number, Order>>(new Map());
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

        if (message.type === "book") {
          setBestBid(
            message.hasBid
              ? { price: message.bestBid, quantity: message.bidQuantity }
              : null
          );
          setBestAsk(
            message.hasAsk
              ? { price: message.bestAsk, quantity: message.askQuantity }
              : null
          );
          return;
        }

        if (message.type === "add") {
          setOrders((previous) => {
            const next = new Map(previous);
            next.set(message.orderId, {
              orderId: message.orderId,
              price: message.price,
              quantity: message.quantity,
              side: message.side,
            });
            return next;
          });

          setTape((previous) =>
            [
              {
                sequence: message.sequence,
                text: `ADD ${message.side} #${message.orderId} ${message.quantity} @ ${message.price}`,
              },
              ...previous,
            ].slice(0, 40)
          );
          return;
        }

        if (message.type === "execute") {
          setOrders((previous) => {
            const next = new Map(previous);

            for (const id of [message.buyOrderId, message.sellOrderId]) {
              const order = next.get(id);
              if (!order) continue;

              const remaining = order.quantity - message.quantity;
              if (remaining > 0) {
                next.set(id, { ...order, quantity: remaining });
              }
            }

            return next;
          });

          setTape((previous) =>
            [
              {
                sequence: message.sequence,
                text: `TRADE ${message.quantity} @ ${message.price}  buy #${message.buyOrderId} / sell #${message.sellOrderId}`,
              },
              ...previous,
            ].slice(0, 40)
          );
          return;
        }

        if (message.type === "delete") {
          setOrders((previous) => {
            const next = new Map(previous);
            next.delete(message.orderId);
            return next;
          });

          setTape((previous) =>
            [
              {
                sequence: message.sequence,
                text: `DELETE #${message.orderId}`,
              },
              ...previous,
            ].slice(0, 40)
          );
        }
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

  const bids = useMemo(
    () =>
      [...orders.values()]
        .filter((order) => order.side === "BUY")
        .sort((a, b) => b.price - a.price || a.orderId - b.orderId)
        .slice(0, 10),
    [orders]
  );

  const asks = useMemo(
    () =>
      [...orders.values()]
        .filter((order) => order.side === "SELL")
        .sort((a, b) => a.price - b.price || a.orderId - b.orderId)
        .slice(0, 10),
    [orders]
  );

  const midpoint =
    bestBid && bestAsk
      ? ((bestBid.price + bestAsk.price) / 2).toFixed(2)
      : "—";

  return (
    <main className="shell">
      <header>
        <div>
          <p className="eyebrow">LOW-LATENCY EXCHANGE SIMULATOR</p>
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

      <section className="metrics">
        <Metric
          label="Best Bid"
          value={bestBid ? `${bestBid.price}` : "—"}
          sub={bestBid ? `${bestBid.quantity} units` : "No bid"}
        />
        <Metric
          label="Midpoint"
          value={midpoint}
          sub="Top of book"
        />
        <Metric
          label="Best Ask"
          value={bestAsk ? `${bestAsk.price}` : "—"}
          sub={bestAsk ? `${bestAsk.quantity} units` : "No ask"}
        />
        <Metric
          label="Visible L3 Orders"
          value={`${orders.size}`}
          sub="Reconstructed from feed"
        />
      </section>

      <section className="grid">
        <div className="panel order-book">
          <div className="panel-title">
            <h2>Level 3 Order Book</h2>
            <span>price-time feed</span>
          </div>

          <div className="book-columns">
            <BookSide title="Bids" orders={bids} />
            <BookSide title="Asks" orders={asks} />
          </div>
        </div>

        <div className="panel tape">
          <div className="panel-title">
            <h2>Event Tape</h2>
            <span>latest 40 events</span>
          </div>

          <div className="tape-list">
            {tape.length === 0 ? (
              <div className="empty">Waiting for market data…</div>
            ) : (
              tape.map((item) => (
                <div className="tape-row" key={`${item.sequence}-${item.text}`}>
                  <span>{item.sequence}</span>
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