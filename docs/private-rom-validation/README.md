# Private ROM validation release gate

The public `release.yml` and `private-rom-tests.yml` use the same fail-closed
client, `scripts/private_rom_validation.py`. Releases require success for the
exact tag commit. Desktop, Android, Web and Google Play publication jobs are
unchanged by this correction. Nothing here dispatches a release during setup.

## Required private repository installation

These are **complete replacement files**, deliberately outside the public
`.github/workflows` directory. The private repository was not accessed or changed
for this correction. Install both files in `DanielSeim/gbb-private-tests` on `main`:

| Supplied file | Private repository destination |
| --- | --- |
| `gbb-validation.yml` | `.github/workflows/gbb-validation.yml` |
| `tools/serialized_validation.py` | `tools/serialized_validation.py` |

Keep the private repository's `tools/compare_regression.py` and
`baselines/frame-sha256.json`; no ROMs or private baseline values belong in the
public repository. The baseline and report contracts are preserved: 100 unique
cases, all passing, matching model, frame count and hashes at 300/600/900.

Install the private changes **before enabling the updated public gate**. Its new
run title includes the correlation and exact commit; its sole job must be named
`Private ROM deployment and regression`. An old private workflow fails the gate.
Do not mix the old title/protocol with the new public client.

## Queue, runner and Synology requirements

1. Keep exactly one dedicated Linux x64 runner with label `gbb-rom-tests`, Python
   3, CMake, Ninja, Git and the build dependencies. The runner must support the
   Node 24 runtime used by checkout v6. Mount the same **persistent local Synology
   filesystem** at `/staging` across runner/container restarts. Do not put the lock
   on an unreliable network filesystem, ephemeral container storage or per-job
   mounts. The runner's service account needs write permission; preserve the
   existing deployment service's access to staged binaries.
2. The complete deployment and test are one job, on that runner, in the private
   repository's fixed `gbb-private-rom-execution` group. Set `queue: max` and
   `cancel-in-progress: false` for **every** private workflow that can deploy or
   run regressions. Prefer disabling old standalone deployment/test workflows
   and routing every request through this supplied workflow. Workflows in other
   repositories do not share GitHub concurrency groups.
3. All remaining NAS writers (manual tools, scheduled jobs, other runners or
   repositories) must be disabled or use the same transaction helper, shared
   `transaction.lock` and `active.json` guard. Merely using the same runner label
   or a separate `flock` is insufficient. Restrict deployment/test service
   credentials and network access to cooperating callers. No YAML can serialize
   an external client which bypasses the lock.
4. Preserve the private `private-rom-tests` environment and its `GBB_DEPLOY_TOKEN`
   and `GBB_TEST_TOKEN`. The supplied helper uses the existing endpoints
   `http://deploy-service:8788/deploy` (`{"commit": "..."}`) and
   `http://rom-service:8787/test` (`{}`). Successful deployment must mean the
   requested staged binary has been installed and is the one the ROM service
   executes. Returning a regression report must mean the worker has finished.
   Verify these service contracts on the NAS; the public checkout and dispatch
   correlation alone cannot attest which executable a remote service ran.
5. The public `private-rom-trigger` environment retains
   `GBB_PRIVATE_ACTIONS_TOKEN`, with private Actions read/write access. The public
   `github.token` only reads public source/tag/run metadata. Private responses,
   ROM results and artifacts remain private; public logs contain only validation
   phase, run ID, source commit and success/failure.

GitHub's documented [`queue: max`](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
is a server-maintained queue for **up to 100 pending runs**, surviving runner
restarts. Ordinary pushes cannot replace an already waiting release request.
Overflow cancels the additional request, which the gate treats as failure.
FIFO is defined by admission to the concurrency group, **not dispatch time**;
strict ordering by push/dispatch timestamp is not promised. The two public
workflows do not share a single-pending group: push requests independently reach
the private queue. The public release group also uses `queue: max` so another
release attempt cannot replace its pending publisher.

The runner serializes the whole job; the NAS file lock covers staging,
deployment, regression and baseline checking. The persistent ledger under
`/staging/.gbb-validation` binds each correlation to its source SHA, executable
hash and baseline hash. A successful duplicate request reuses its report without
redeploying. A duplicate with changed identity or a previous failure fails closed.
Ledger files are internal and must not be copied into public artifacts.

## Deadlines and API failures

| Phase | Maximum |
| --- | --- |
| Private workflow registration | 10 minutes |
| Private queue, from private run creation to actual job start | 120 minutes |
| Private job execution, from job `started_at` | 75 minutes |
| Public gate job | 240 minutes |

The client polls both run and job metadata: a workflow can be `in_progress` while
its self-hosted job still waits for a runner. It switches to the execution budget
only once that job actually starts. `timeout-minutes: 75` also limits the private
job itself. The parent allows all phase budgets plus checkout and separate public
metadata/tag lookups. A public timeout never publishes and never force-cancels a
potentially active NAS service. The queued private request may still run after
the public gate has failed; revalidate explicitly before publishing.

Read requests retry HTTP 429, 500, 502, 503, 504 and transport errors at most six
times with exponential backoff. Rate-limited 403 responses are distinguished
from permission errors. `Retry-After` seconds/HTTP dates and primary rate-limit
reset times are respected within the current phase deadline. HTTP 401,
non-rate-limited 403 and inaccessible 404 are explicit failures.

Dispatch is intentionally different: only an explicitly rejected rate-limit
response is retried. A timeout, malformed reply or HTTP 5xx can mean the POST was
accepted. The client reconciles the run by correlation **without resending**;
no matching run within the registration deadline means failure, never success.
Lookup is paginated and bounded; exceeding 1,000 matching-window runs fails
closed rather than guessing. The search window starts at the originating public
workflow's creation time and therefore survives a hosted-runner replacement.
An exclusive local marker is written and synced before dispatch. Correlation is
`public_repository_id-public_run_id` and remains stable across public reruns.
Public reruns may reuse the existing private run but never issue another dispatch
if none is visible. Start a new public workflow run to request a new validation.
Rerunning the private run itself is rejected; use a new request instead.

## Interruption recovery

Before the first mutating NAS request, the helper synchronizes `active.json` to
disk. If deployment/test times out, the runner disappears, or the job is killed,
that marker **continues to block every subsequent cooperating deployment** even
after the OS releases the file lock. An unanswered NAS POST is never retried.
This matters because the ROM service may continue executing after its HTTP client
has gone away. A fully returned report with a baseline mismatch records failure
and releases the guard, since the worker has finished.

For a quarantined transaction, an operator must first stop/confirm idle both NAS
services, inspect the recorded request, and verify the installed executable.
While holding the same `/staging/.gbb-validation/transaction.lock`, archive the
marker for investigation and remove `active.json`. Request a **new correlation**;
do not clear a marker based only on elapsed time or a canceled GitHub run.
Keep ledger records and reports at least as long as workflow reruns are allowed;
any cleanup must hold the same lock and must never remove an active transaction.

## Validation evidence and private acceptance

Run the offline tests with:

```sh
python3 tests/private_rom_validation_tests.py
```

They exercise 20 minutes queued plus 60 minutes executing using a virtual clock,
phase deadlines, transient HTTP 503 and rate limits, ambiguous dispatch results,
public reruns, wrong source/workflow identity, duplicate matching runs, NAS
idempotence, interruption quarantine and four push requests arriving while a
release transaction owns the real file lock. Workflow contracts verify both
server-side durable queues and the dedicated runner configuration. These are
local contract tests, **not a claim that the private installation was exercised**.

After private installation, before relying on this gate for releases, perform a
non-publishing acceptance run: keep one validation running, dispatch several
main-commit requests, and verify every admitted request remains pending and all
NAS deployment/test intervals are disjoint. Confirm the runner survives restart
with its mount and ledger intact; deliberately interrupt a test and verify the
quarantine blocks deployment until operator recovery. Inspect the deployment
service's installed executable hash against the staged hash. No release creation
or publication is needed for these private checks.
