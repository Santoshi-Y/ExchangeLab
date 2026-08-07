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

type PerformanceMessage = {
  type: "performance";
  timestampMs: number;
  uptimeMs: number;
  ordersTotal: number;
  acceptedTotal: number;
  rejectedTotal: number;
  riskRejectedTotal: number;
  cancelRequestsTotal: number;
  successfulCancelsTotal: number;
  replaceRequestsTotal: number;
  successfulReplacesTotal: number;
  tradesTotal: number;
  tradedQuantity: number;
  ordersPerSecond: number;
  executionsPerSecond: number;
  activeOrders: number;
  instruments: number;
  connectedClients: number;
  matchingLatencyNs: {
    samples: number;
    mean: number;
    p50: number;
    p95: number;
    p99: number;
    max: number;
  };
  marketData: {
    enqueued: number;
    sent: number;
    dropped: number;
    sendErrors: number;
    queueDepth: number;
    maxQueueDepth: number;
    queueCapacity: number;
    producerShardsUsed: number;
    producerRegistrationFailures: number;
  };
};

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

type HistoryPoint = {
  ordersPerSecond: number;
  executionsPerSecond: number;
  p99LatencyNs: number;
  queueDepth: number;
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
  const [performance, setPerformance] = useState<PerformanceMessage | null>(null);
  const [history, setHistory] = useState<HistoryPoint[]>([]);
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
        const message = JSON.parse(event.data) as MarketMessage | PerformanceMessage;

        if (message.type === "performance") {
          setPerformance(message);
          setHistory((previous) =>
            [
              ...previous,
              {
                ordersPerSecond: message.ordersPerSecond,
                executionsPerSecond: message.executionsPerSecond,
                p99LatencyNs: message.matchingLatencyNs.p99,
                queueDepth: message.marketData.queueDepth,
              },
            ].slice(-60)
          );
          return;
        }

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
    if (symbols.length > 0 && !instruments.has(selectedSymbol)) {
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

  const orderRateHistory = history.map((point) => point.ordersPerSecond);
  const executionRateHistory = history.map((point) => point.executionsPerSecond);
  const latencyHistory = history.map((point) => point.p99LatencyNs);
  const queueHistory = history.map((point) => point.queueDepth);

  return (
    <main className="shell">
      <header>
        <div>
          <p className="eyebrow">LOW-LATENCY MULTI-SYMBOL EXCHANGE SIMULATOR</p>
          <h1>ExchangeLab</h1>
          <p className="subtitle">Live market data + exchange performance telemetry</p>
        </div>

        <div className="status-group">
          <span className={`status ${connected ? "online" : "offline"}`}>
            <span className="dot" />
            {connected ? "WebSocket connected" : "Disconnected"}
          </span>
          <span className="sequence">SEQ {lastSequence || "—"}</span>
          <span className="sequence">
            UPTIME {performance ? formatUptime(performance.uptimeMs) : "—"}
          </span>
        </div>
      </header>

      <section className="section-heading">
        <div>
          <span className="section-kicker">ENGINE</span>
          <h2>Performance</h2>
        </div>
        <span className="live-note">1-second telemetry snapshots</span>
      </section>

      <section className="performance-metrics">
        <Metric
          label="Order Rate"
          value={performance ? formatRate(performance.ordersPerSecond) : "—"}
          sub={`${performance?.ordersTotal ?? 0} total new orders`}
        />
        <Metric
          label="Execution Rate"
          value={performance ? formatRate(performance.executionsPerSecond) : "—"}
          sub={`${performance?.tradesTotal ?? 0} total trades`}
        />
        <Metric
          label="Matching p50"
          value={performance ? formatLatency(performance.matchingLatencyNs.p50) : "—"}
          sub={performance ? `mean ${formatLatency(performance.matchingLatencyNs.mean)}` : "No samples"}
        />
        <Metric
          label="Matching p95"
          value={performance ? formatLatency(performance.matchingLatencyNs.p95) : "—"}
          sub={`${performance?.matchingLatencyNs.samples ?? 0} samples`}
        />
        <Metric
          label="Matching p99"
          value={performance ? formatLatency(performance.matchingLatencyNs.p99) : "—"}
          sub={performance ? `max ${formatLatency(performance.matchingLatencyNs.max)}` : "No samples"}
        />
        <Metric
          label="Active Orders"
          value={`${performance?.activeOrders ?? 0}`}
          sub={`${performance?.instruments ?? symbols.length} instruments`}
        />
      </section>

      <section className="charts-grid">
        <ChartCard
          title="Order throughput"
          value={performance ? formatRate(performance.ordersPerSecond) : "—"}
          data={orderRateHistory}
        />
        <ChartCard
          title="Executions"
          value={performance ? formatRate(performance.executionsPerSecond) : "—"}
          data={executionRateHistory}
        />
        <ChartCard
          title="p99 matching latency"
          value={performance ? formatLatency(performance.matchingLatencyNs.p99) : "—"}
          data={latencyHistory}
        />
        <ChartCard
          title="Market-data queue"
          value={performance ? `${performance.marketData.queueDepth}` : "—"}
          data={queueHistory}
          detail={performance ? `/ ${performance.marketData.queueCapacity} slots` : "queue depth"}
        />
      </section>

      <section className="health-grid">
        <HealthItem label="Connected clients" value={`${performance?.connectedClients ?? 0}`} />
        <HealthItem label="Accepted orders" value={`${performance?.acceptedTotal ?? 0}`} />
        <HealthItem label="Rejected orders" value={`${performance?.rejectedTotal ?? 0}`} />
        <HealthItem label="Risk rejects" value={`${performance?.riskRejectedTotal ?? 0}`} />
        <HealthItem label="Traded quantity" value={formatInteger(performance?.tradedQuantity ?? 0)} />
        <HealthItem label="MD sent" value={formatInteger(performance?.marketData.sent ?? 0)} />
        <HealthItem label="MD dropped" value={formatInteger(performance?.marketData.dropped ?? 0)} warn={(performance?.marketData.dropped ?? 0) > 0} />
        <HealthItem label="Send errors" value={formatInteger(performance?.marketData.sendErrors ?? 0)} warn={(performance?.marketData.sendErrors ?? 0) > 0} />
        <HealthItem label="Queue high-water" value={`${performance?.marketData.maxQueueDepth ?? 0}`} />
        <HealthItem label="Producer shards" value={`${performance?.marketData.producerShardsUsed ?? 0}`} />
      </section>

      <section className="section-heading market-heading">
        <div>
          <span className="section-kicker">MARKET</span>
          <h2>Live Order Book</h2>
        </div>
      </section>

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

      <section className="market-metrics">
        <Metric
          label={`${selectedSymbol} Best Bid`}
          value={selected.bestBid ? `${selected.bestBid.price}` : "—"}
          sub={selected.bestBid ? `${selected.bestBid.quantity} units` : "No bid"}
        />
        <Metric label="Midpoint" value={midpoint} sub={selectedSymbol} />
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

function HealthItem({
  label,
  value,
  warn = false,
}: {
  label: string;
  value: string;
  warn?: boolean;
}) {
  return (
    <div className={`health-item ${warn ? "warning" : ""}`}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function ChartCard({
  title,
  value,
  data,
  detail = "last 60 seconds",
}: {
  title: string;
  value: string;
  data: number[];
  detail?: string;
}) {
  return (
    <div className="chart-card">
      <div className="chart-header">
        <div>
          <span>{title}</span>
          <strong>{value}</strong>
        </div>
        <small>{detail}</small>
      </div>
      <Sparkline data={data} />
    </div>
  );
}

function Sparkline({ data }: { data: number[] }) {
  const points = useMemo(() => {
    if (data.length === 0) return "";

    const minimum = Math.min(...data);
    const maximum = Math.max(...data);
    const range = Math.max(maximum - minimum, 1);
    const divisor = Math.max(data.length - 1, 1);

    return data
      .map((value, index) => {
        const x = (index / divisor) * 100;
        const y = 30 - ((value - minimum) / range) * 26;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      })
      .join(" ");
  }, [data]);

  return (
    <div className="sparkline-wrap">
      {points ? (
        <svg className="sparkline" viewBox="0 0 100 32" preserveAspectRatio="none" aria-hidden="true">
          <polyline points={points} vectorEffect="non-scaling-stroke" />
        </svg>
      ) : (
        <div className="sparkline-empty">Waiting for telemetry…</div>
      )}
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

function formatLatency(nanoseconds: number): string {
  if (!Number.isFinite(nanoseconds) || nanoseconds <= 0) return "—";
  if (nanoseconds < 1_000) return `${Math.round(nanoseconds)} ns`;
  if (nanoseconds < 1_000_000) return `${(nanoseconds / 1_000).toFixed(2)} µs`;
  return `${(nanoseconds / 1_000_000).toFixed(2)} ms`;
}

function formatRate(value: number): string {
  if (!Number.isFinite(value)) return "—";
  if (value >= 1_000_000) return `${(value / 1_000_000).toFixed(2)}M/s`;
  if (value >= 1_000) return `${(value / 1_000).toFixed(2)}K/s`;
  return `${value.toFixed(1)}/s`;
}

function formatInteger(value: number): string {
  return new Intl.NumberFormat("en-US").format(value);
}

function formatUptime(milliseconds: number): string {
  const totalSeconds = Math.floor(milliseconds / 1000);
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;

  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${seconds}s`;
  return `${seconds}s`;
}

export default App;