# GitHub configuration and releases

The repository contains executable GitHub Actions workflows. Commit and push
them to `main` to activate CI; no repository secret is required.

## Repository settings

Set the default branch to `main`. In a branch ruleset for `main`, require pull
requests and the **CI required** status check. Allow that check to run once
before selecting it in GitHub's ruleset UI. It fails if workflow lint or any
Linux/macOS verification job fails or is cancelled. Avoid path filters on
required workflows, because skipped workflows can leave checks pending.

Keep the default Actions token read-only. Workflows grant `contents: write`
only to the final release publication job. SHA-pinned official actions are
updated by Dependabot. Fork pull requests use `pull_request`, without secrets
or privileged `pull_request_target` execution.

Release tags are a publication boundary. Limit creation/deletion of `v*` tags
to maintainers using a tag ruleset. GitHub settings live outside this checkout
and must be applied by a repository administrator.

## CI

`tests.yml` handles pushes to `main`, pull requests, and manual runs.
`build.yml` is reused by CI and releases. It pins LLVM 21, verifies generated
property vectors, builds release/debug compilers, runs every CTest suite and
debug smoke checks, and verifies all six benchmark suites. Memory, subobject,
owner-tree, owned-value, and tooling checks run alongside matched C/Rust
workloads. ASan/UBSan and independent runtime counter checks are untimed.

Reports are retained for 14 days. Cancelled superseded CI runs save resources;
release runs are serialized by tag and are not cancelled midway.

## Release

1. Update `VERSION` in a reviewed commit. It must be a semantic version such
   as `0.1.0` or `0.1.0-rc.1`, without build metadata.
2. Merge and push that commit to `main`; wait for **CI required**.
3. Create an annotated tag matching `VERSION`, then push the tag:

   ```sh
   git tag -a "v$(cat VERSION)" -m "Kawa $(cat VERSION)"
   git push origin "v$(cat VERSION)"
   ```

Pushing a `v*` version tag starts `release.yml`. A manual run can use an
existing tag; it does not create one. The workflow rejects tags outside the
`main` history and tags that disagree with `VERSION`.

Release builds set `KAWA_NATIVE_CPU=OFF`, repeat all verification, install the
compiler into a temporary prefix, and compile/run the managed-memory example
from the **unpacked** binary archive. Packages include Apache/MIT notices,
documentation, and a build manifest. The source archive includes the exact
pinned submodule contents, not a dangling Git submodule.

The publication job waits for both verified platform archives and the source
archive, validates SHA-256 sidecars, then uploads them to a draft and publishes
only after every upload succeeds. Tags with a prerelease suffix are marked
as prereleases. Retries can complete a draft; they cannot replace assets in
an already published release.

Local package verification, from a clean committed checkout:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAWA_NATIVE_CPU=OFF
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure --parallel 2
python3 scripts/package_release.py --build-dir build-release --output dist --source --binary
```

Archive timestamps and ownership are normalized to the source commit.
The source archive is reproducible for the same commits; binary reproducibility
across different SDKs or toolchains is not claimed.
