# Simulator

Supports both Node.js and Bun.

## Running

```bash
npm install        # or: bun install

npm start          # Node.js (tsx)
npm run start:bun  # Bun

npm run start:once          # Node.js, single report
npm run start:once:bun      # Bun, single report
```

Pass `--interval <seconds>` to override the 30s default:
```bash
node --import tsx/esm simulate.ts --interval 10
bun simulate.ts --interval 10
```

## Environment

| Variable | Default |
|---|---|
| `MQTT_BROKER_URL` | `mqtt://109.123.238.215:1883` |
