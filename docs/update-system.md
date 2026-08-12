# Alcedo Studio update release steps

The application accepts only a signed schema-1 manifest. Keep the Ed25519 private
key offline. Do not put it in this repository or in GitHub Actions.

## One-time key setup

Build the signer, then generate a key pair on a trusted computer. If a public key
already exists in `alcedo_studio/src/config/update_public_key.txt`, reuse that
pair. Do not generate a new key for an installed app base.

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target alcedo_update_signer
build\debug\alcedo_studio\src\alcedo_update_signer.exe generate `
  --private-key D:\secure\alcedo-update-private.seed `
  --public-key alcedo_studio\src\config\update_public_key.txt
```

Commit the public key file. Store the same Base64 value as the GitHub variable
`ALCEDO_UPDATE_PUBLIC_KEY_BASE64`.

## Build kinds

| Build | Check production feed | Download / install |
|---|---|---|
| Release build-tree (`ALCEDO_UPDATE_ALLOW_INSTALL=OFF`, default) | Yes. Offer UI even when the local build is not older. | No |
| Packaged installer (`ALCEDO_UPDATE_ALLOW_INSTALL=ON` via package scripts) | Yes. Offer only when remote build is greater. | Yes |
| Debug | Use for automated tests, not for UI checks | — |

Validate the update dialog under a Release build-tree binary against the live
feed at `https://static.aoraw.org/updates/v1/stable/<platform>/manifest.json`.

## Update channels

Test and official builds check separate feeds so a beta manifest can never roll
back or shadow a stable install. The channel is baked into the binary at build
time and selects both the manifest URL and the per-channel trusted sequence.

| Channel | Build | Feed | Trusted sequence key |
|---|---|---|---|
| `stable` (default) | Official release | `https://static.aoraw.org/updates/v1/stable/<platform>/manifest.json` | `updates/stable/highestTrustedSequence` |
| `beta` | Test / prerelease | `https://static.aoraw.org/updates/v1/beta/<platform>/manifest.json` | `updates/beta/highestTrustedSequence` |

`<platform>` is `windows-x86_64` or `macos-arm64`. Platform-specific aliases let
the Windows and macOS packaging machines publish independently without one
platform replacing the other platform's live manifest.

Set the channel when packaging a test build:

```powershell
# Windows test build
powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1 -Channel beta

# macOS test build
./scripts/package_macos.sh --channel beta
```

A build-tree binary picks up the channel from the CMake cache; pass
`-DALCEDO_UPDATE_CHANNEL=beta` to the configure preset to validate against the
beta feed. Override `ALCEDO_UPDATE_BASE_URL` for local staging.

## Package

```powershell
# Windows (sets ALCEDO_UPDATE_ALLOW_INSTALL=ON)
powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1

# macOS
./scripts/package_macos.sh
```

Use the NSIS `.exe` for Windows updates. Use the CPack `.zip` for macOS updates.
Keep the DMG for manual download.

Pass `-Channel beta` (Windows) or `--channel beta` (macOS) to package a test
build that checks the beta feed; see [Update channels](#update-channels).

Package build numbers increment automatically and independently per platform.
The last successful numbers live under the ignored
`build/tmp/update-build-number/` directory. A number is recorded only after the
package and install-tree checks succeed; retrying a failed run reuses its pending
number. If that local state is missing, the package script seeds from the project
version default and any existing `ALCEDO_BUILD_NUMBER` in the platform CMake
cache. Use `-BuildNumber N` on Windows or `--build-number N` on macOS only when
you intentionally need an exact build identity.

VS Code exposes stable and beta package tasks for both platforms. All four tasks
use automatic build-number allocation.

## Sign and publish (one command)

```powershell
# Windows package machine
python scripts\update\publish_update.py --platform windows `
  --private-key D:\secure\alcedo-update-private.seed

# macOS package machine
python3 scripts/update/publish_update.py --platform macos \
  --private-key /Volumes/secure/alcedo-update-private.seed
```

This script:

1. Reads version, build number, and update channel from the platform's CMake
   cache.
2. Finds the package in the fixed output directory: Windows uses
   `build/release/package/*.exe`; macOS uses
   `build/macos-release/package/*.zip` plus its `.dmg`.
3. Copies the selected platform packages under
   `build/tmp/update/<channel>/<platform>/artifacts`.
4. Writes a platform-specific `update-manifest.json` (schema 1).
5. Adds optional `changelog` from `CHANGELOG.md` when a `## [version]` section
   exists. The field is omitted when missing.
6. Signs with `alcedo_update_signer` and verifies the signature and package
   hashes.
7. Uploads immutable files first, the platform channel signature next, and the
   platform channel manifest last.

## Publish to R2

Dry run (no credentials):

```powershell
python scripts\update\publish_update.py `
  --platform windows `
  --private-key D:\secure\alcedo-update-private.seed `
  --dry-run
```

Real upload (reads `R2_*` from the environment or `rust/puerh_mind/.env.test`):

```powershell
python scripts\update\publish_update.py `
  --platform windows `
  --private-key D:\secure\alcedo-update-private.seed
```

The channel is read from the package's CMake cache, so the normal upload command
still requires only platform and private key. `--channel` is an optional safety
override and must match the packaged channel. Upload order puts the channel
signature before the channel manifest, so a client never accepts an unsigned or
partially published update. Stable also repoints this platform's
`releases/latest/` aliases; beta only updates its beta/platform feed. The script
generates a UTC timestamp sequence for each run.

The `Sync release assets to R2` workflow publishes prereleases to the beta
channel and full releases to the stable channel automatically. You may also run
`publish_update.py` by hand. Because the GitHub workflow has both platform
packages, it also updates the legacy `updates/v1/<channel>/manifest.json` feed
for clients installed before platform-specific aliases existed. The local
single-platform publisher deliberately does not overwrite that shared legacy
feed.

## Packaged end-to-end install test

Use the beta channel so the test cannot replace the stable feed. The package
script prints the selected build number; the second successful package gets the
next number automatically.

### Windows

1. Package A, then install the generated NSIS `.exe` before packaging B:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1 `
     -Channel beta
   ```

2. Start installed A. Open Settings > About. Confirm the Updates row has readable
   text and says it is on the beta channel before/while checking.
3. Package B. This intentionally replaces the same-version package file in the
   fixed package directory:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1 `
     -Channel beta
   ```

4. Validate the complete signing and upload plan without R2 credentials:

   ```powershell
   python scripts\update\publish_update.py --platform windows `
     --private-key D:\secure\alcedo-update-private.seed --dry-run
   ```

   Inspect
   `build/tmp/update/beta/windows-x86_64/update-manifest.json`: its `build` must
   equal the number printed by the B package run and be greater than A; it must
   contain only `windows-x86_64`, and the printed live alias must end in
   `updates/v1/beta/windows-x86_64/manifest.json`.
5. Put `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, and
   `R2_SECRET_ACCESS_KEY` in the environment or the ignored
   `rust/puerh_mind/.env.test`, then publish:

   ```powershell
   python scripts\update\publish_update.py --platform windows `
     --private-key D:\secure\alcedo-update-private.seed
   ```

6. In installed A, click Check for updates. Verify all of these behaviors:

   - the offer dialog text and changelog are readable;
   - Later closes the dialog and leaves the toolbar update indicator available;
   - View update reopens the same offer;
   - Update downloads the `.exe`, rejects any size/hash mismatch, and changes to
     Install and restart only after verification;
   - Install and restart closes A, runs the helper, installs B, and relaunches;
   - checking again from B reports up to date because its local build equals the
     signed remote build.

### macOS

Repeat the same A/B flow with:

```bash
./scripts/package_macos.sh --channel beta
# Install/open A before replacing the package output.
./scripts/package_macos.sh --channel beta
python3 scripts/update/publish_update.py --platform macos \
  --private-key /Volumes/secure/alcedo-update-private.seed --dry-run
python3 scripts/update/publish_update.py --platform macos \
  --private-key /Volumes/secure/alcedo-update-private.seed
```

The manifest and live alias are under `beta/macos-arm64`; the ZIP is the
automatic-update package and the DMG remains the manual-download package. The
installed `.app` directory must be writable for automatic replacement.

## Manifest fields

Required: `schema` (1), `sequence`, `version`, `build`, `publishedAt`,
`expiresAt`, and the selected platform entry under `artifacts` with `url`,
`sha256`, and `size`. A GitHub release manifest may contain both supported
platform entries; a platform-machine upload contains only its own entry.

Optional: `notesUrl`, `changelog` (plain text, max 16 KiB).

## Platform limits

This design does not use Authenticode, Developer ID, or Apple notarization.
Windows may show an unknown-publisher warning. macOS may require manual open
approval. A macOS app in a folder the user cannot write cannot replace itself.
