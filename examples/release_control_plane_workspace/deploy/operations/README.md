# Operations Runbook

This runbook is the narrow backup/restore/upgrade handoff for the current release-control-plane
sample. It covers both supported preview data planes:

- default SQLite state under `APP_STATE_DIR`
- opt-in Postgres state with `APP_DATA_BACKEND=postgres`

It is intentionally not a built-in backup service, migration framework, database operator, or
cross-database replication story.

## Data Plane Selection

SQLite remains the default:

```bash
APP_DATA_BACKEND=sqlite
APP_STATE_DIR=/var/lib/release-control-plane
```

The active SQLite database file is:

```text
$APP_STATE_DIR/release-control-plane.db
```

Postgres is preview and must be opted in explicitly:

```bash
APP_DATA_BACKEND=postgres
APP_POSTGRES_PREVIEW=1
APP_POSTGRES_DSN='postgresql://user:pass@host:5432/release_control_plane'
# Or prefer APP_POSTGRES_DSN_FILE=/run/release-control-plane-secrets/postgres.dsn
APP_AUTH_REQUIRED=1
```

Do not enable both data planes, do not dual-write, and do not expect automatic SQLite-to-Postgres
migration. Pick one data plane per service deployment.

## SQLite Backup

Use a stopped-service backup for this sample. The app can run with WAL enabled, so copying only the
main `.db` file while the process is running is not a safe handoff.

Host `systemd` flow:

```bash
sudo touch /run/release-control-plane/drain
sudo systemctl stop release-control-plane
sudo install -d -m 0700 /var/backups/release-control-plane
sudo cp -a /var/lib/release-control-plane/release-control-plane.db* \
  /var/backups/release-control-plane/
sudo systemctl start release-control-plane
```

Kubernetes flow:

```bash
kubectl -n release-control-plane scale statefulset/release-control-plane --replicas=0
kubectl -n release-control-plane wait --for=delete pod/release-control-plane-0 --timeout=120s
# Take a storage-provider VolumeSnapshot of the release-control-plane-state PVC here.
kubectl -n release-control-plane scale statefulset/release-control-plane --replicas=1
kubectl -n release-control-plane rollout status statefulset/release-control-plane
```

If your storage platform does not provide PVC snapshots, run an operator-owned copy from a temporary
maintenance Pod while the StatefulSet is scaled down.

## SQLite Restore

Restore with the service stopped, then restart and verify `/healthz`, `/readyz`, and a CLI read path.

Host `systemd` flow:

```bash
sudo systemctl stop release-control-plane
sudo install -d -m 0750 -o nebula -g nebula /var/lib/release-control-plane
sudo cp -a /var/backups/release-control-plane/release-control-plane.db* \
  /var/lib/release-control-plane/
sudo chown nebula:nebula /var/lib/release-control-plane/release-control-plane.db*
sudo systemctl start release-control-plane
nebula run apps/ctl --run-gate none -- status --url http://127.0.0.1:40480 --token "$APP_READ_TOKEN"
```

Kubernetes restore uses your storage provider's restore-from-snapshot mechanism, then:

```bash
kubectl -n release-control-plane rollout restart statefulset/release-control-plane
kubectl -n release-control-plane rollout status statefulset/release-control-plane
```

## Postgres Backup

Postgres backup is operator-owned. The Nebula service does not run `pg_dump` for you.

Recommended logical backup:

```bash
pg_dump --format=custom --no-owner --no-privileges \
  --file release-control-plane.pgcustom \
  "$APP_POSTGRES_DSN"
```

Keep the DSN out of shell history where possible. In production, prefer `APP_POSTGRES_DSN_FILE`,
your platform's secret manager, or a protected environment file readable only by the operator
account.

## Postgres Restore

Restore into an empty database or a maintenance window. Stop the Nebula service first so it does not
write while the restore is running.

```bash
sudo systemctl stop release-control-plane
pg_restore --clean --if-exists --no-owner --no-privileges \
  --dbname "$APP_POSTGRES_DSN" \
  release-control-plane.pgcustom
sudo systemctl start release-control-plane
nebula run apps/ctl --run-gate none -- status --url http://127.0.0.1:40480 --token "$APP_READ_TOKEN"
```

For Kubernetes, rotate/update the Secret containing the DSN if the target database changes, then
restart the StatefulSet so the process takes a fresh startup snapshot.

## Upgrade And Migration

The service runs official package migrations during startup. Treat migration as part of rollout:

1. Back up the active data plane first.
2. Check the migration status in staging or rollout automation before applying a new binary.
3. Start only one service instance against the data plane.
4. Let startup migrations run.
5. Restart once against the same data plane to verify split migration slices are idempotent.
6. Verify `/healthz`, `/readyz`, `ctl status`, and one read path such as `ctl release list`.

The official Postgres preview package exposes a read-only `Connection_migration_status(...)` helper
for rollout tooling. It returns `current_version`, `target_version`, `pending_count`,
`history_issue_count`, `safe_to_apply`, `status`, `pending`, and `history_issues`. Treat
`status=history_mismatch` or `safe_to_apply=false` as a hard stop and restore from a known-good
backup or investigate the schema history before starting the service.

If startup fails during migration, keep the failed logs, do not keep retrying blindly, and restore
from the pre-upgrade backup before rolling back the binary.

Schema downgrade is not supported by either data package. Rollback means restoring a backup taken
before the migration and then starting the older binary.

The current migration runner supports split migration slices: restarting after the database has
advanced through later store modules must not make earlier migration slices look like a downgrade.
It still rejects missing or renamed migration history for any version supplied by the running binary.

## Non-Claims

- No automatic SQLite-to-Postgres migration.
- No online backup API.
- No connection pool.
- No ORM/query DSL.
- No multi-writer SQLite story.
- No built-in Postgres operator.
