# ExchangeLab Docker deployment

This deployment packages the current ExchangeLab platform into two containers:

- `exchangelab-backend`: exchange core + WebSocket gateway + FIX 4.4 gateway
- `exchangelab-dashboard`: production Vite build served by Nginx

The three C++ backend processes intentionally share one container/network namespace. This preserves ExchangeLab's existing local UDP design without changing the hot path:

- public market data: `239.255.0.1:9100` multicast, internal to the backend container
- performance telemetry: `127.0.0.1:9200`, internal to the backend container

Externally published ports:

- `9000/tcp`: ExchangeLab binary order-entry protocol v3
- `9878/tcp`: FIX 4.4 order-entry gateway
- `8080/tcp`: browser WebSocket market/performance feed
- `3000/tcp`: React performance dashboard

The exchange journal is stored in the named Docker volume `exchange-data`, so restarting or recreating the backend container preserves order recovery state.

## Start everything

From the repository root:

```bash
docker compose up --build
```

Then open:

```text
http://localhost:3000
```

The dashboard connects to the WebSocket gateway using the browser's current hostname on port 8080, so it works from `localhost` and from another machine reaching the Docker host by name/IP.

## Run in the background

```bash
docker compose up --build -d
```

Inspect status:

```bash
docker compose ps
```

Follow backend logs:

```bash
docker compose logs -f backend
```

Follow dashboard logs:

```bash
docker compose logs -f dashboard
```

## Generate sample exchange activity

Because `exchange_client` is a demo tool rather than a deployed service, run it from your normal local Release build while Docker owns port 9000:

```bash
./build-release/exchange_client
```

That reaches the container through the published `localhost:9000` port, and the browser should update through `localhost:8080`.

The existing local FIX demo client can likewise reach the containerized FIX gateway on `localhost:9878`:

```bash
./build-release/exchange_fix_client
```

## Stop

```bash
docker compose down
```

This stops/removes containers but keeps the `exchange-data` journal volume.

## Full clean reset

To remove the persistent journal volume too:

```bash
docker compose down -v
```

The next `docker compose up` will start with an empty journal.

## Rebuild after source changes

```bash
docker compose up --build
```

## Architecture

```text
Browser :3000
    |
    | WebSocket :8080
    v
+-----------------------------------------+
| exchangelab-backend                     |
|                                         |
|  FIX :9878 ---> FIX Gateway --------+   |
|                                     |   |
|  TCP :9000 -----------------------> |   |
|                                     v   |
|                              Exchange   |
|                              + Risk     |
|                              + Books    |
|                                 |       |
|                 multicast :9100 |       |
|                telemetry :9200  |       |
|                                 v       |
|                         WebSocket GW    |
+-----------------------------------------+
                  |
                  | persistent /data/exchange.log
                  v
          Docker volume: exchange-data
```

## Why the backend processes share one container

The current exchange uses UDP multicast for public market data and loopback UDP for performance telemetry. Keeping the core, WebSocket gateway, and FIX gateway in one backend network namespace preserves those semantics exactly and avoids weakening the project just to accommodate Docker bridge networking. The React/Nginx frontend remains independently deployable.
