# nebula-jobs

Preview background jobs and workflow kernel for Nebula backend apps.

Current surface:

- DAG stage contracts with explicit `depends_on`
- SQLite-first job definition/run store
- pull-based worker lease claim/heartbeat/complete
- max-attempt retry and dead-letter semantics
- idempotent event receipts
- durable outbox claim/complete

Non-claims:

- no Postgres jobs store
- no native Kafka/NATS/RabbitMQ client
- no cron daemon
- no workflow UI
- no distributed remote deploy agent
- no in-process shell command execution
