# nebula-jobs

Preview background jobs and workflow kernel for Nebula backend apps.

Current surface:

- DAG stage contracts with explicit `depends_on`
- SQLite-first job definition/run store
- pull-based worker lease claim/heartbeat/complete
- max-attempt retry and dead-letter semantics
- idempotent event receipts
- durable outbox claim/complete

Embedding note:

- release-control-plane currently consumes the jobs contracts, DAG validation, stage-state rules,
  retry semantics, and outbox state helpers through explicit adapters
- direct replacement of a product workflow store with `jobs::sqlite` must preserve the product's
  transaction boundary, data-backend boundary, and app/channel scoped claim semantics; do not add a
  second write-through run store as a hidden fallback

Non-claims:

- no Postgres jobs store
- no native Kafka/NATS/RabbitMQ client
- no cron daemon
- no workflow UI
- no distributed remote deploy agent
- no in-process shell command execution
