# Native Windows CI for AQ integration

The `Native Windows build and tests` job runs on exact pushed `main` and
`aq/**` commits. AQ must bind this actual check name after its first verified
run. A local build or a synthetic pull-request merge is not CI evidence for
a different candidate SHA.

The dedicated Windows x64 runner requires the `matter-engine-msvc` label
and the toolchain resolved by `tools/windows/MatterWindowsToolchain.psm1`.
Use a separate Actions work directory, never the interactive engine checkout.
The job limits native compilation to four workers and CTest to two. One
runner executes one job at a time; no shared concurrency group replaces
pending candidate jobs. The workflow grants read-only repository access and
does not persist checkout credentials.

`tools/ci-windows.ps1` runs the canonical RelWithDebInfo build, the matching
CTest preset (empty test sets fail), and the canonical distribution build
and package validation. It writes a transcript, resolved toolchain, and JUnit
results under `artifacts/ci/`, uploaded even when the job fails. Run it from
native PowerShell to reproduce CI locally.

Bootstrap status: workflow prepared, runner installation and remote CI
verification pending. The baseline is f912eaf75a805dc898af0a17051baf0378105123,
which includes 228 commits beyond the previously published main. Preserve
that ancestry when publishing the baseline for review. Feature task origins
must use the accepted current baseline. Keep AQ feature execution paused
until real check evidence and the integration policy are configured.
