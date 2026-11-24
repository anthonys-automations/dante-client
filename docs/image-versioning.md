# Image builds and versioning

This repository publishes `anthonysautomations/dante-client`, a multi-platform
Alpine image containing the patched `libdsocks.so`. It does not install an
application package: downstream images can add their own packages and
configuration in later layers.

## Build and release

The `Build and Publish Image` workflow publishes `linux/amd64` and `linux/arm64`
images to Docker Hub. It runs:

- on changes to `Dockerfile`, any `dante-*.patch`, the tests under `test/`
  or the workflow itself on `main`;
- every Sunday at 00:00 UTC, to rebuild from the pinned Alpine release tag;
- when manually dispatched.

Both architecture builds push by digest. The workflow publishes a release tag
only after both builds succeed and the assembled manifest contains both
platforms. It then moves the `latest` convenience tag. The immutable release
tag, not `latest`, is the deployment reference.

The release workflow requires repository secrets `DOCKERHUB_USERNAME` and
`DOCKERHUB_TOKEN`, with permission to push to the Docker Hub repository. It
allocates tags as:

```text
<UTC YYYY.MM.DD>.<n>
```

where `n` counts that day's releases, starting at 1: `2026.11.24.1`,
`2026.11.24.2`, ..., then `2026.11.25.1`. The workflow takes `n` from the
tags already in the registry (the highest of the day plus one), so a run that
fails before publishing does not use up a number. Releases run one at a time
(workflow concurrency group), and before building the workflow still checks
that the tag does not exist yet. It fails closed if the registry cannot list
the tags or verify the new one. Rerunning a run whose tag has meanwhile been
published fails; start a new run to allocate the next tag.

The published image includes these OCI labels:

- `org.opencontainers.image.title`
- `org.opencontainers.image.version`
- `org.opencontainers.image.revision`
- `org.opencontainers.image.source`
- `org.opencontainers.image.created`
- `io.anthonysautomations.base.image`

BuildKit also produces an SBOM attestation. The source commit is available in
both `org.opencontainers.image.revision` and the `GIT_COMMIT` environment
variable. Treat these as traceability metadata, not as a substitute for
recording the immutable image digest.

## Renovate

The scheduled Renovate workflow updates the Alpine base image, Dante source
version and checksum, and pinned GitHub Actions. Dante versions and SHA-256
checksums are taken together from the upstream download page. Dante and Alpine
updates are eligible for automerge after `Validate Image Build` passes on both
architectures. Each image build applies the Alpine and local patches without
fuzz, then runs the worker and IPv4/IPv6 gateway tests on the resulting
library. A failure blocks the update and leaves the PR open for investigation.
GitHub Actions updates appear on Renovate's dependency dashboard for manual
application because the workflow's built-in `GITHUB_TOKEN` cannot push changes
to `.github/workflows`.

The release workflow runs on a schedule as well as on source changes. This
ensures that changes merged by Renovate using `GITHUB_TOKEN`—which do not
trigger a follow-up push workflow—are published by the next scheduled build.

Consumers that use Renovate to track releases can use a regex versioning rule
like:

```text
# renovate: datasource=docker depName=anthonysautomations/dante-client versioning=regex:^(?<major>\d{4})\.(?<minor>\d{2})\.(?<patch>\d{2})\.(?<build>\d+)$
```

Pin the immutable release tag, preferably with its digest. Do not deploy
`latest`, which is a moving alias.
