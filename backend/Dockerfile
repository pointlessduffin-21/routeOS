FROM oven/bun:1.2-alpine

WORKDIR /app

# Copy package files first for layer caching
COPY backend/package.json backend/bun.lockb* ./
RUN bun install --frozen-lockfile 2>/dev/null || bun install

# Copy source and marketplace catalog
COPY backend/ .
COPY marketplace/sensors /app/marketplace/sensors

EXPOSE 4003

ENV PORT=4003
ENV MQTT_URL=mqtt://mosquitto:1883
ENV DB_PATH=/data/routeos.db
ENV CATALOG_PATH=/app/marketplace/sensors

CMD ["bun", "src/index.ts"]
