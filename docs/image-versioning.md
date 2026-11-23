# Image builds and versioning

This repository publishes `anthonysautomations/dante-client`, a multi-platform
Alpine image containing the patched `libdsocks.so`. It does not install an
application package: downstream images can add their own packages and
configuration in later layers.

## Build and release

The `Build and Publish Image` workflow publishes `linux/amd64` and `linux/arm64`
images to Docker Hub. It runs:

- on changes to `Dockerfile` or `dante-connectchild-workers.patch` on `main`;
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
<UTC YYYY.MM.DD>.<GitHub workflow run number>
```

For example, `2026.11.23.42`. The workflow run number provides a unique,
increasing sequence for this workflow. Before building, it checks that the tag
does not already exist and fails closed if the registry cannot verify that.
Rerunning a workflow for an already-published tag will fail; start a new
workflow run to allocate a new tag.

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

The scheduled Renovate workflow updates the Alpine base image and pinned GitHub
Actions. Dockerfile changes are grouped and may automerge after the
build-only `Validate Image Build` workflow succeeds on both architectures.
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
