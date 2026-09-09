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

Bootstrap evidence: published prerequisite
`72867a102a263d07d0a3de2b9c0ec52fe6841040` preserves the 228-commit engine
baseline and its native-CI fixes. GitHub Actions run `34326903928` passed the
required job at that exact SHA on the dedicated runner. AQ task and candidate
heads must still pass the same required check before promotion; evidence for
the prerequisite SHA is not evidence for a different candidate SHA.
