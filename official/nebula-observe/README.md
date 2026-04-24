# nebula-observe

Official Nebula backend SDK package for service observability helpers.

Recommended installed backend SDK dependency shape:

```toml
[dependencies]
observe = { installed = "nebula-observe" }
```

Repo-checkout dependency shape:

```toml
[dependencies]
observe = { path = "/path/to/nebula/official/nebula-observe" }
```

Release posture:

- Linux backend GA surface
- distributed in the optional Linux backend SDK asset
- not installed by default core CLI/tooling archives

Current surface in this repo wave:

- `observe::log`
  - `classification(reason, detail) -> Json`
  - `info_event(service, event, message)`
  - `error_event(service, event, message)`
  - `error_event_request(service, event, message, request_id)`
  - `error_event_request_classified(service, event, message, request_id, classification)`
  - `error_event_request_with_text(service, event, message, request_id, request_id_text)`
  - `error_event_request_with_text_classified(service, event, message, request_id, request_id_text, classification)`
  - `error_event_request_with_details(service, event, message, request_id, request_id_text, details)`
  - `error_event_request_with_details_classified(service, event, message, request_id, request_id_text, classification, details)`
  - `request_started_classified(service, request_id, path, classification)`
  - `request_started(service, request_id, path)`
  - `request_started_with_text_classified(service, request_id, request_id_text, path, classification)`
  - `request_started_with_text(service, request_id, request_id_text, path)`
  - `request_finished_classified(service, request_id, status, duration_ms, classification)`
  - `request_finished(service, request_id, status, duration_ms)`
  - `request_finished_with_text_classified(service, request_id, request_id_text, status, duration_ms, classification)`
  - `request_finished_with_text(service, request_id, request_id_text, status, duration_ms)`
  - `lifecycle_listener_bound(service, bind_host, port, message)`
  - `lifecycle_drain_requested(service, path)`
  - `lifecycle_shutdown_requested(service, path)`
  - `lifecycle_listener_stopped(service, reason, served_requests)`
- `observe::metrics`
  - `delta_counter(service, metric, unit, value)`
  - `count(service, metric, value)`
- collector-side bridge helper
  - `prometheus_bridge.py render --input <observe.ndjson>`
  - `prometheus_bridge.py serve --input <observe.ndjson> --port 9464`

Design notes for this wave:

- The package emits newline-delimited JSON through `std::log`.
- It is intentionally log-first and stateless.
- Request and lifecycle events now carry a stable `classification = { reason, detail }` object so
  operator logs can share the same machine-readable incident vocabulary as transport debug/explain
  surfaces.
- Metrics are event-shaped counters, not a resident metrics registry.
- `delta_counter(...)` is the stable bridge contract for collector-side translation.
  It emits `schema = "nebula.observe.metric.v1"` and `signal = "metric"` together with
  `service`, `metric`, `unit`, `value`, and `unix_ms`.
- In this revision, every `delta_counter(...)` event is a delta counter sample.
  Downstream collectors may translate those events into Prometheus counter increments or
  OpenTelemetry counter datapoints, but that translation lives outside this package.
- `prometheus_bridge.py` is the narrow sample bridge for this repo wave.
  It rereads a redirected observe log file and renders:
  - `unit = "1"` delta-counter events as Prometheus counters
  - derived `nebula_service_requests_total{service,status}` samples from `request_finished` events
  - derived `nebula_service_request_duration_ms_sum` /
    `nebula_service_request_duration_ms_count` counters from `request_finished` events
  - derived `nebula_service_events_by_classification_total{service,event,reason,detail}` samples
    from classified observe events
  - derived `nebula_service_requests_by_classification_total{service,status,reason,detail}`
    samples from classified `request_finished` events
  - derived `nebula_service_request_duration_ms_sum_by_classification` /
    `nebula_service_request_duration_ms_count_by_classification` counters from classified
    `request_finished` events
  It is intentionally a sidecar-style helper rather than a built-in exporter.
  - it is not a stateful metrics daemon: reusing a long-lived append-only log file will preserve
    previously emitted counter history in subsequent renders/scrapes
- `count(...)` remains as the legacy minimal counter event for callers that only need the
  original `{service, metric, value, unix_ms}` shape.

Example bridge flow:

```bash
nebula run . --run-gate none 2> service.observe.ndjson
python3 official/nebula-observe/prometheus_bridge.py serve --input service.observe.ndjson --port 9464
curl http://127.0.0.1:9464/metrics
```

When the Linux backend SDK is installed, the same helper ships beside the installed package under
`share/nebula/sdk/backend/nebula-observe/prometheus_bridge.py`.

Current non-goals for this package revision:

- Prometheus/OpenTelemetry exporters
- a resident `/metrics` registry or scrape endpoint
- tailing, log rotation coordination, or multi-process aggregation inside the helper itself
- histograms, summaries, or label sets
- sampling or log shipping integrations
