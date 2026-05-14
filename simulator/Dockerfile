FROM oven/bun:1-slim
WORKDIR /app

COPY package.json bun.lock* ./
RUN bun install --no-frozen-lockfile

COPY . .

# Default: multi-bin simulator (override with `command:` in docker-compose)
CMD ["bun", "simulate.ts"]
