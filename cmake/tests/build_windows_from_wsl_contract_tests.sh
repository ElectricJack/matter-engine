#!/usr/bin/env bash
set -euo pipefail

repository_root="$1"
fixture_root="$(mktemp -d "${repository_root}/MatterEditor/build/wrapper-wsl-fixture.XXXXXX")"
trap 'rm -rf -- "$fixture_root"' EXIT

mkdir -p "$fixture_root/tools" "$fixture_root/MatterEditor/build/windows-msvc"
cp "$repository_root/tools/build-windows-from-wsl.sh" "$fixture_root/tools/"

fake_powershell="$fixture_root/fake-powershell"
cat >"$fake_powershell" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
target=''
previous=''
for argument in "$@"; do
    if [[ "$previous" == '-Target' ]]; then target="$argument"; fi
    previous="$argument"
done
if [[ -z "$target" || "$target" == 'matter_editor' || "$target" == 'editor' || "$target" == 'all' ]]; then
    printf 'MATTER_WINDOWS_ARTIFACT=%s\n' 'C:\fixture\MatterEditor\build\windows-msvc\editor.exe'
elif [[ "$target" == 'matter_dist' ]]; then
    printf 'MATTER_WINDOWS_PACKAGE=%s\n' 'C:\fixture\MatterEditor\build\dist\world_demo'
fi
EOF
chmod +x "$fake_powershell"

run_wrapper() {
    local target="$1"
    MATTER_WINDOWS_POWERSHELL="$fake_powershell" \
        "$fixture_root/tools/build-windows-from-wsl.sh" RelWithDebInfo "$target" 2>&1
}

touch "$fixture_root/MatterEditor/build/windows-msvc/editor.exe"
non_editor_output="$(run_wrapper compiler_portability_tests)"
if grep -q 'MATTER_WINDOWS_ARTIFACT=' <<<"$non_editor_output" ||
        grep -q 'MATTER_WSL_ARTIFACT=' <<<"$non_editor_output"; then
    echo "non-editor WSL target emitted an editor artifact marker" >&2
    echo "$non_editor_output" >&2
    exit 1
fi

rm "$fixture_root/MatterEditor/build/windows-msvc/editor.exe"
set +e
missing_output="$(run_wrapper matter_editor)"
missing_status=$?
set -e
if [[ $missing_status -eq 0 ]] || grep -q 'MATTER_WSL_ARTIFACT=' <<<"$missing_output"; then
    echo "missing WSL editor artifact was not rejected" >&2
    echo "$missing_output" >&2
    exit 1
fi

touch "$fixture_root/MatterEditor/build/windows-msvc/editor.exe"
editor_output="$(run_wrapper matter_editor)"
windows_count="$(grep -c '^MATTER_WINDOWS_ARTIFACT=' <<<"$editor_output")"
wsl_count="$(grep -c '^MATTER_WSL_ARTIFACT=' <<<"$editor_output")"
if [[ $windows_count -ne 1 || $wsl_count -ne 1 ]]; then
    echo "editor WSL target must pass through one Windows marker and add one WSL marker" >&2
    echo "$editor_output" >&2
    exit 1
fi

package="$fixture_root/MatterEditor/build/dist/world_demo"
rm -rf -- "$package"
set +e
missing_package_output="$(run_wrapper matter_dist)"
missing_package_status=$?
set -e
if [[ $missing_package_status -eq 0 ]] || grep -q 'MATTER_WSL_PACKAGE=' <<<"$missing_package_output"; then
    echo "missing WSL package was not rejected" >&2
    echo "$missing_package_output" >&2
    exit 1
fi

mkdir -p "$package"
touch "$package/editor.exe" "$package/build_features.json"
package_output="$(run_wrapper matter_dist)"
windows_package_count="$(grep -c '^MATTER_WINDOWS_PACKAGE=' <<<"$package_output")"
wsl_package_count="$(grep -c '^MATTER_WSL_PACKAGE=' <<<"$package_output")"
if [[ $windows_package_count -ne 1 || $wsl_package_count -ne 1 ]] ||
        grep -q 'MATTER_.*_ARTIFACT=' <<<"$package_output"; then
    echo "matter_dist WSL target must pass through one Windows package marker and add one WSL package marker" >&2
    echo "$package_output" >&2
    exit 1
fi

echo 'WSL build-wrapper contract passed'
