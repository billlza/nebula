#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

METRIC_SCHEMA = "nebula.observe.metric.v1"
METRICS_CONTENT_TYPE = "text/plain; version=0.0.4; charset=utf-8"
DELTA_COUNTER_HELP_TEXT = "Nebula service delta counter aggregated from nebula.observe.metric.v1 events."
REQUESTS_HELP_TEXT = "Nebula request_finished events aggregated by HTTP status."
REQUEST_DURATION_SUM_HELP_TEXT = "Sum of Nebula request_finished durations in milliseconds."
REQUEST_DURATION_COUNT_HELP_TEXT = "Count of Nebula request_finished duration samples."
EVENTS_BY_CLASSIFICATION_HELP_TEXT = (
    "Nebula observe events aggregated by event plus classification.reason/detail."
)
REQUESTS_BY_CLASSIFICATION_HELP_TEXT = (
    "Nebula request_finished events aggregated by HTTP status plus classification.reason/detail."
)
REQUEST_DURATION_SUM_BY_CLASSIFICATION_HELP_TEXT = (
    "Sum of Nebula request_finished durations in milliseconds grouped by classification.reason/detail."
)
REQUEST_DURATION_COUNT_BY_CLASSIFICATION_HELP_TEXT = (
    "Count of Nebula request_finished duration samples grouped by classification.reason/detail."
)


def sanitize_metric_name(metric: str) -> str:
    sanitized = re.sub(r"[^a-zA-Z0-9_:]", "_", metric)
    sanitized = re.sub(r"_+", "_", sanitized).strip("_")
    if not sanitized:
        sanitized = "unknown"
    return f"nebula_service_{sanitized}_total"


def escape_label_value(value: str) -> str:
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace('"', '\\"')


def classification_fields(payload: dict[object, object]) -> tuple[str, str] | None:
    classification = payload.get("classification")
    if not isinstance(classification, dict):
        return None
    reason = classification.get("reason")
    detail = classification.get("detail")
    if not isinstance(reason, str) or not reason:
        return None
    if not isinstance(detail, str) or not detail:
        return None
    return reason, detail


def load_samples(input_path: Path) -> tuple[dict[str, dict[str, int]],
                                           dict[str, dict[str, int]],
                                           dict[str, int],
                                           dict[str, int],
                                           dict[tuple[str, str, str, str], int],
                                           dict[tuple[str, str, str, str], int],
                                           dict[tuple[str, str, str], int],
                                           dict[tuple[str, str, str], int]]:
    families: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    requests_total: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    request_duration_sum: dict[str, int] = defaultdict(int)
    request_duration_count: dict[str, int] = defaultdict(int)
    events_by_classification: dict[tuple[str, str, str, str], int] = defaultdict(int)
    requests_by_classification: dict[tuple[str, str, str, str], int] = defaultdict(int)
    request_duration_sum_by_classification: dict[tuple[str, str, str], int] = defaultdict(int)
    request_duration_count_by_classification: dict[tuple[str, str, str], int] = defaultdict(int)
    if not input_path.exists():
        return {}, {}, {}, {}, {}, {}, {}, {}

    with input_path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if not line.startswith("{"):
                json_start = line.find("{")
                if json_start == -1:
                    continue
                line = line[json_start:]
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(payload, dict):
                continue
            event = payload.get("event")
            service = payload.get("service")
            grouped_classification = classification_fields(payload)
            if isinstance(event, str) and event and isinstance(service, str) and service and grouped_classification is not None:
                reason, detail = grouped_classification
                events_by_classification[(service, event, reason, detail)] += 1
            if event == "request_finished":
                status = payload.get("status")
                duration_ms = payload.get("duration_ms")
                if isinstance(service, str) and service and isinstance(status, int) and isinstance(duration_ms, int):
                    requests_total[service][str(status)] += 1
                    request_duration_sum[service] += duration_ms
                    request_duration_count[service] += 1
                    if grouped_classification is not None:
                        reason, detail = grouped_classification
                        requests_by_classification[(service, str(status), reason, detail)] += 1
                        request_duration_sum_by_classification[(service, reason, detail)] += duration_ms
                        request_duration_count_by_classification[(service, reason, detail)] += 1
            if payload.get("schema") != METRIC_SCHEMA or payload.get("signal") != "metric":
                continue
            if payload.get("unit") != "1":
                continue
            metric = payload.get("metric")
            value = payload.get("value")
            if not isinstance(service, str) or not service:
                continue
            if not isinstance(metric, str) or not metric:
                continue
            if not isinstance(value, int):
                continue
            family = sanitize_metric_name(metric)
            families[family][service] += value
    return (
        {family: dict(services) for family, services in families.items()},
        {service: dict(statuses) for service, statuses in requests_total.items()},
        dict(request_duration_sum),
        dict(request_duration_count),
        dict(events_by_classification),
        dict(requests_by_classification),
        dict(request_duration_sum_by_classification),
        dict(request_duration_count_by_classification),
    )


def render_metrics_text(input_path: Path) -> str:
    (
        families,
        requests_total,
        request_duration_sum,
        request_duration_count,
        events_by_classification,
        requests_by_classification,
        request_duration_sum_by_classification,
        request_duration_count_by_classification,
    ) = load_samples(input_path)
    if (
        not families
        and not requests_total
        and not request_duration_sum
        and not request_duration_count
        and not events_by_classification
        and not requests_by_classification
        and not request_duration_sum_by_classification
        and not request_duration_count_by_classification
    ):
        return "# no metrics observed\n"

    lines: list[str] = []
    for family in sorted(families.keys()):
        lines.append(f"# HELP {family} {DELTA_COUNTER_HELP_TEXT}")
        lines.append(f"# TYPE {family} counter")
        for service in sorted(families[family].keys()):
            lines.append(
                f'{family}{{service="{escape_label_value(service)}"}} {families[family][service]}'
            )

    if requests_total:
        lines.append("# HELP nebula_service_requests_total " + REQUESTS_HELP_TEXT)
        lines.append("# TYPE nebula_service_requests_total counter")
        for service in sorted(requests_total.keys()):
            for status in sorted(requests_total[service].keys(), key=lambda value: int(value)):
                lines.append(
                    'nebula_service_requests_total{service="%s",status="%s"} %s'
                    % (
                        escape_label_value(service),
                        escape_label_value(status),
                        requests_total[service][status],
                    )
                )

    if events_by_classification:
        lines.append("# HELP nebula_service_events_by_classification_total " + EVENTS_BY_CLASSIFICATION_HELP_TEXT)
        lines.append("# TYPE nebula_service_events_by_classification_total counter")
        for service, event, reason, detail in sorted(events_by_classification.keys()):
            lines.append(
                'nebula_service_events_by_classification_total{service="%s",event="%s",reason="%s",detail="%s"} %s'
                % (
                    escape_label_value(service),
                    escape_label_value(event),
                    escape_label_value(reason),
                    escape_label_value(detail),
                    events_by_classification[(service, event, reason, detail)],
                )
            )

    if requests_by_classification:
        lines.append("# HELP nebula_service_requests_by_classification_total " + REQUESTS_BY_CLASSIFICATION_HELP_TEXT)
        lines.append("# TYPE nebula_service_requests_by_classification_total counter")
        for service, status, reason, detail in sorted(
            requests_by_classification.keys(),
            key=lambda value: (value[0], int(value[1]), value[2], value[3]),
        ):
            lines.append(
                'nebula_service_requests_by_classification_total{service="%s",status="%s",reason="%s",detail="%s"} %s'
                % (
                    escape_label_value(service),
                    escape_label_value(status),
                    escape_label_value(reason),
                    escape_label_value(detail),
                    requests_by_classification[(service, status, reason, detail)],
                )
            )

    if request_duration_sum:
        lines.append("# HELP nebula_service_request_duration_ms_sum " + REQUEST_DURATION_SUM_HELP_TEXT)
        lines.append("# TYPE nebula_service_request_duration_ms_sum counter")
        for service in sorted(request_duration_sum.keys()):
            lines.append(
                'nebula_service_request_duration_ms_sum{service="%s"} %s'
                % (escape_label_value(service), request_duration_sum[service])
            )

    if request_duration_count:
        lines.append("# HELP nebula_service_request_duration_ms_count " + REQUEST_DURATION_COUNT_HELP_TEXT)
        lines.append("# TYPE nebula_service_request_duration_ms_count counter")
        for service in sorted(request_duration_count.keys()):
            lines.append(
                'nebula_service_request_duration_ms_count{service="%s"} %s'
                % (escape_label_value(service), request_duration_count[service])
            )

    if request_duration_sum_by_classification:
        lines.append(
            "# HELP nebula_service_request_duration_ms_sum_by_classification "
            + REQUEST_DURATION_SUM_BY_CLASSIFICATION_HELP_TEXT
        )
        lines.append("# TYPE nebula_service_request_duration_ms_sum_by_classification counter")
        for service, reason, detail in sorted(request_duration_sum_by_classification.keys()):
            lines.append(
                'nebula_service_request_duration_ms_sum_by_classification{service="%s",reason="%s",detail="%s"} %s'
                % (
                    escape_label_value(service),
                    escape_label_value(reason),
                    escape_label_value(detail),
                    request_duration_sum_by_classification[(service, reason, detail)],
                )
            )

    if request_duration_count_by_classification:
        lines.append(
            "# HELP nebula_service_request_duration_ms_count_by_classification "
            + REQUEST_DURATION_COUNT_BY_CLASSIFICATION_HELP_TEXT
        )
        lines.append("# TYPE nebula_service_request_duration_ms_count_by_classification counter")
        for service, reason, detail in sorted(request_duration_count_by_classification.keys()):
            lines.append(
                'nebula_service_request_duration_ms_count_by_classification{service="%s",reason="%s",detail="%s"} %s'
                % (
                    escape_label_value(service),
                    escape_label_value(reason),
                    escape_label_value(detail),
                    request_duration_count_by_classification[(service, reason, detail)],
                )
            )

    return "\n".join(lines) + "\n"


class MetricsHandler(BaseHTTPRequestHandler):
    server: "MetricsServer"

    def do_GET(self) -> None:
        if self.path == "/healthz":
            body = b'{"status":"ok"}'
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path != "/metrics":
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return

        body = render_metrics_text(self.server.input_path).encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", METRICS_CONTENT_TYPE)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format: str, *args: object) -> None:
        return


class MetricsServer(ThreadingHTTPServer):
    def __init__(self, server_address: tuple[str, int], input_path: Path):
        super().__init__(server_address, MetricsHandler)
        self.input_path = input_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render or serve Prometheus text from nebula.observe.metric.v1 log events"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    render_parser = subparsers.add_parser("render", help="Render a Prometheus text snapshot to stdout")
    render_parser.add_argument("--input", required=True, help="Path to newline-delimited observe log output")

    serve_parser = subparsers.add_parser("serve", help="Serve /metrics from a newline-delimited observe log file")
    serve_parser.add_argument("--input", required=True, help="Path to newline-delimited observe log output")
    serve_parser.add_argument("--host", default="127.0.0.1")
    serve_parser.add_argument("--port", type=int, default=9464)

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input).resolve()
    if args.command == "render":
        print(render_metrics_text(input_path), end="")
        return 0

    server = MetricsServer((args.host, args.port), input_path)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
