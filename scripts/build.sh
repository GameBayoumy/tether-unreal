#!/usr/bin/env bash
# Packages TetherXR for each installed engine version with RunUAT BuildPlugin.
#
#   scripts/build.sh            # every version found (5.8, 5.3)
#   scripts/build.sh 5.8        # just one
#
# Output: out/TetherXR-UE<version>/ (plugin ready to drop into a project's Plugins/ folder),
# with the full log in out/TetherXR-UE<version>.log. Override the engine location with
# UE_ROOT_<major>_<minor>, e.g. UE_ROOT_5_8="/path/to/UE_5.8".

set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
plugin="$repo/TetherXR/TetherXR.uplugin"
versions=("$@")
[ ${#versions[@]} -eq 0 ] && versions=(5.8 5.3)

# UE 5.3 targets Xcode 15. Newer Apple clang (16+) rejects a few of its engine headers, so for 5.3 only we
# relax those diagnostics through clang's driver override hook instead of touching the engine install:
# raise the deployment target the new libc++ requires, silence two header warnings, and skip access
# checks one engine template now fails at definition time.
legacy_overrides='# s/-mmacosx-version-min=10.15$/-mmacosx-version-min=14.0/ +-Wno-deprecated-literal-operator +-Wno-#warnings +-Wno-invalid-unevaluated-string +-fno-access-control'
clang_major=$(xcrun clang --version 2>/dev/null | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)

mkdir -p "$repo/out"
failed=0
summary=()

# Readies a package for Engine/Plugins/Marketplace/TetherXR, where Tether installs it:
# - UBT bakes rpaths relative to where the plugin was built; point them at the engine relative to the install.
# - BuildPlugin drops EnabledByDefault; engine plugins need it to load in every project.
make_installable() {
    local package="$1" dylib rpath
    for dylib in "$package"/Binaries/Mac/*.dylib; do
        otool -l "$dylib" | awk '/LC_RPATH/ {getline; getline; print $2}' | while read -r rpath; do
            case "$rpath" in
                @loader_path/|@executable_path/*) ;;
                *) install_name_tool -delete_rpath "$rpath" "$dylib" ;;
            esac
        done
        install_name_tool -add_rpath "@loader_path/../../../../../Binaries/Mac" "$dylib"
        install_name_tool -add_rpath "@loader_path/../../../../Runtime/XRBase/Binaries/Mac" "$dylib"
        codesign --force --sign - "$dylib"
    done
    /usr/bin/python3 - "$package/TetherXR.uplugin" <<'PY'
import collections, json, sys
path = sys.argv[1]
plugin = json.load(open(path), object_pairs_hook=collections.OrderedDict)
plugin["EnabledByDefault"] = True
json.dump(plugin, open(path, "w"), indent="\t")
PY
}

for version in "${versions[@]}"; do
    override="UE_ROOT_${version//./_}"
    engine="${!override:-/Users/Shared/Epic Games/UE_$version}"
    uat="$engine/Engine/Build/BatchFiles/RunUAT.sh"
    package="$repo/out/TetherXR-UE$version"
    log="$repo/out/TetherXR-UE$version.log"

    if [ ! -x "$uat" ]; then
        summary+=("UE $version: skipped (no engine at $engine)")
        continue
    fi

    overrides=""
    if [ "$version" = "5.3" ] && [ "${clang_major:-0}" -ge 16 ]; then
        overrides="$legacy_overrides"
        echo "==> Building TetherXR for UE $version (Apple clang $clang_major: using 5.3 compatibility overrides)"
    else
        echo "==> Building TetherXR for UE $version"
    fi

    rm -rf "$package"
    start=$(date +%s)
    if [ -n "$overrides" ]; then
        CCC_OVERRIDE_OPTIONS="$overrides" "$uat" BuildPlugin -Plugin="$plugin" -Package="$package" -TargetPlatforms=Mac >"$log" 2>&1
    else
        "$uat" BuildPlugin -Plugin="$plugin" -Package="$package" -TargetPlatforms=Mac >"$log" 2>&1
    fi
    status=$?
    elapsed=$(( $(date +%s) - start ))

    if [ $status -eq 0 ]; then
        make_installable "$package"
        summary+=("UE $version: OK in ${elapsed}s -> out/TetherXR-UE$version")
    else
        failed=1
        summary+=("UE $version: FAILED (exit $status) after ${elapsed}s, see out/TetherXR-UE$version.log")
        grep -E "error:" "$log" | sort -u | head -20
    fi
done

echo
printf '%s\n' "${summary[@]}"
exit $failed
