# Kubernetes Assets

This directory is the narrow Kubernetes handoff for the current release-control-plane sample.

Shipped assets:

- `namespace.yaml`
- `configmap.yaml`
- `secret.example.yaml`
- `persistentvolumeclaim.yaml`
- `statefulset.yaml`
- `service.yaml`
- `ingress-nginx.yaml`

These files intentionally stop at:

- a single-replica `StatefulSet`
- a ClusterIP Service
- a PVC-backed SQLite state directory
- a mounted Secret volume for `APP_*_TOKEN_FILE`, including an optional worker token
- an ingress-nginx example that terminates TLS before traffic reaches the Nebula Pod
- an optional Postgres preview data-plane mode through `APP_DATA_BACKEND=postgres`, with the
  database itself managed outside these manifests

They do not claim:

- a built-in ingress/controller choice
- a Nebula-specific rollout framework
- hot secret reload
- operator-managed backups or migrations beyond `deploy/operations/README.md`

The mounted-secret contract stays startup-only:

- rotate the Secret data
- roll the `StatefulSet`
- let the new Pod pick up the changed token files at startup

## Apply Order

1. Build the release binary:

```bash
nebula build apps/service --mode release --out-dir .service-out
```

2. Build and publish the runtime image:

```bash
docker build -f deploy/container/Dockerfile.runtime -t release-control-plane:local .
```

3. Create the namespace first:

```bash
kubectl apply -f deploy/k8s/namespace.yaml
```

4. Apply non-secret config, Secret, and storage before the Pod comes up:

```bash
kubectl apply -f deploy/k8s/configmap.yaml
kubectl apply -f deploy/k8s/secret.example.yaml
kubectl apply -f deploy/k8s/persistentvolumeclaim.yaml
```

5. Apply the Service before the `StatefulSet`:

```bash
kubectl apply -f deploy/k8s/service.yaml
kubectl apply -f deploy/k8s/statefulset.yaml
```

6. If you use ingress-nginx, apply the ingress after the Service exists:

```bash
kubectl apply -f deploy/k8s/ingress-nginx.yaml
```

7. Wait for the single replica to become ready:

```bash
kubectl -n release-control-plane rollout status statefulset/release-control-plane
```

8. Verify the narrow handoff shape:

```bash
kubectl -n release-control-plane get pods,svc,pvc,ingress
```

Before applying production-like migrations, read:

```text
deploy/operations/README.md
```

That runbook defines the required backup-before-upgrade flow for the default SQLite PVC and the
optional Postgres preview data plane.

## Rollout And Secret Rotation

Current rollout semantics stay intentionally narrow:

- this sample is single-replica because it persists SQLite on a single PVC
- do not scale it out and pretend the SQLite/PVC story is now multi-writer safe
- expect restart-based secret rotation, not hot reload
- if `APP_DATA_BACKEND=postgres`, keep the Postgres service/backups outside this manifest bundle and
  roll the StatefulSet after changing the `postgres.dsn` Secret value consumed through
  `APP_POSTGRES_DSN_FILE`

Secret rotation runbook:

1. Update the Secret manifest or recreate the Secret data.
2. Apply the updated Secret:

```bash
kubectl apply -f deploy/k8s/secret.example.yaml
```

3. Restart the `StatefulSet` so the new Pod takes a fresh startup snapshot:

```bash
kubectl -n release-control-plane rollout restart statefulset/release-control-plane
kubectl -n release-control-plane rollout status statefulset/release-control-plane
```

4. Re-run a narrow auth check against the restarted Pod/service path.

If you use a dedicated worker client against `/v1/workers/*`, also re-check the worker lane after
rotation:

1. submit a narrow internal event or `release apply`
2. claim a lease with the mounted worker token
3. verify heartbeat/complete still succeed after the restarted Pod comes up

This is intentionally restart-based and may cause brief unavailability because the sample stays at
one replica.
