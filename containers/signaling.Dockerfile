FROM node:24.16.0-bookworm-slim@sha256:ca520832af80fa37a57c14077ed0fcdd83b5aefccc356059fdc3a9a05b78ae1f

ENV NODE_ENV=production
WORKDIR /opt/glyphrelay

COPY package.json pnpm-lock.yaml ./
RUN corepack enable \
    && corepack prepare pnpm@11.21.0 --activate \
    && pnpm install --prod --frozen-lockfile --ignore-scripts \
    && pnpm store prune

COPY receiver ./receiver
COPY signaling ./signaling

USER node
EXPOSE 8443
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD ["node", "signaling/healthcheck.ts"]

CMD ["node", "signaling/main.ts"]
