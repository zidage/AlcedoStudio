# Alcedo Studio update release steps

The application accepts only a signed schema-1 manifest. Keep the Ed25519 private
key offline. Do not put it in this repository or in GitHub Actions.

R2 is the source of truth for both automatic updates and website downloads.
GitHub Releases are a public archive of an already-live stable pair. The archive
workflow does not publish an update.

## One-time key setup

Build the signer, then generate a key pair on a trusted computer. If a public key
already exists in `alcedo_studio/src/config/update_public_key.txt`, reuse that
pair. Do not generate a new key for an installed app base. Windows and macOS
share one Ed25519 pair; generate it on either platform and keep the private seed
offline.

### Windows

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target alcedo_update_signer
build\debug\alcedo_studio\src\alcedo_update_signer.exe generate `
  --private-key D:\secure\alcedo-update-private.seed `
  --public-key alcedo_studio\src\config\update_public_key.txt
```

### macOS

```bash
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_update_signer
./build/macos-debug/alcedo_studio/src/alcedo_update_signer generate \
  --private-key /Volumes/secure/alcedo-update-private.seed \
  --public-key alcedo_studio/src/config/update_public_key.txt
```

A release-tree signer also works when you already have a packaged build:

```bash
cmake --build --preset macos_release --target alcedo_update_signer
./build/macos-release/alcedo_studio/src/alcedo_update_signer generate \
  --private-key /Volumes/secure/alcedo-update-private.seed \
  --public-key alcedo_studio/src/config/update_public_key.txt
```

Commit the public key file. Store the same Base64 value as the GitHub variable
`ALCEDO_UPDATE_PUBLIC_KEY_BASE64`. Windows and macOS sign with that same offline
private key. Each platform still writes and uploads its own manifest.

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
back or shadow a stable install. The channel is baked into the binary at package
time by the VS Code package tasks (or the same flags on the package scripts).
It selects both the manifest URL and the per-channel trusted sequence.

| Channel | How you get it | Feed |
|---|---|---|
| `stable` | VS Code **Package Windows Release** / **Package macOS Release** | `https://static.aoraw.org/updates/v1/stable/<platform>/manifest.json` |
| `beta` | VS Code **Package Windows Beta** / **Package macOS Beta** | `https://static.aoraw.org/updates/v1/beta/<platform>/manifest.json` |

`<platform>` is `windows-x86_64` or `macos-arm64`. Each platform has its own
signed manifest, build number, and live pointer. Windows and macOS never share
one manifest.

`beta` exists only so you can verify the updater against a feed that cannot
touch installed stable users. Do not treat it as a public prerelease channel.
The GitHub archive workflow reads only the stable feeds.

A build-tree binary picks up the channel from the CMake cache; pass
`-DALCEDO_UPDATE_CHANNEL=beta` to the configure preset to validate against the
beta feed. Override `ALCEDO_UPDATE_BASE_URL` for local staging.

## Package

```powershell
# Windows (sets ALCEDO_UPDATE_ALLOW_INSTALL=ON, stable feed)
powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1

# macOS
./scripts/package_macos.sh
```

Use the NSIS `.exe` for Windows updates. Use the CPack `.zip` for macOS updates.
Keep the DMG for manual / website download.

Pass `-Channel beta` (Windows) or `--channel beta` (macOS) only when you are
testing the updater. The matching VS Code tasks already pass those flags.

Package build numbers increment automatically and independently per platform.
The last successful numbers live under the ignored
`build/tmp/update-build-number/` directory. A number is recorded only after the
package and install-tree checks succeed; retrying a failed run reuses its pending
number. If that local state is missing, the package script seeds from the project
version default and any existing `ALCEDO_BUILD_NUMBER` in the platform CMake
cache. Use `-BuildNumber N` on Windows or `--build-number N` on macOS only when
you intentionally need an exact build identity.

The same marketing version may ship different Windows and macOS build numbers,
including a later one-platform hotfix. Each platform only compares against its
own previous build.

## Release notes

Before any stable upload, invoke the repository skill `$alcedo-release-notes`.
It drafts equivalent English and Simplified Chinese text from the Git range,
then stops for review. It writes the tracked pair only after explicit approval.

For a version release, commit one pair as the last commit of that version on
`main`:

- `docs/changelog/<version>.en.txt`
- `docs/changelog/<version>.zh-CN.txt`

The exact first line of both files is `Alcedo Studio <version>`. Each platform
publisher then stamps its own build into the signed manifest heading
(`Alcedo Studio <version> (Build <n>)` / `Alcedo Studio <version>（构建 <n>）`).

A later one-platform hotfix may add `docs/changelog/<build>.en.txt` and
`docs/changelog/<build>.zh-CN.txt`. If that pair exists, it overrides the
version files for that upload only.

Both files must be UTF-8 plain text with LF endings, a blank line after the
heading, and lines no longer than 88 characters. Tabs, trailing whitespace,
Markdown headings/fences, URLs, files larger than 16 KiB, and missing final
newlines are rejected. Do not put download links in these files. The GitHub
archive job appends official update URLs when it composes the GitHub body.

The root `CHANGELOG.md` only points to this tracked directory.

The skill runs this evidence command internally:

```powershell
python scripts\update\export_release_prs.py `
  --from-ref <previous-release-tag-or-commit> `
  --to-ref <release-commit-on-main> `
  --build <optional-hotfix-build>
```

## What actually publishes a stable update

Uploading with `publish_update.py` on a **Release** (stable) package is what
makes that platform live. Installed apps and the website start using that
manifest as soon as the live pointer is written.

Do not run a real stable upload unless you intend to ship that platform. A
`--dry-run` does not publish. Packaging a Release build on a feature branch
also does not publish; it only bakes the stable feed URL into that binary.

```powershell
# Windows package machine
python scripts\update\publish_update.py --platform windows `
  --private-key D:\secure\alcedo-update-private.seed

# macOS package machine
python3 scripts/update/publish_update.py --platform macos \
  --private-key /Volumes/secure/alcedo-update-private.seed
```

Dry run (no credentials, no upload):

```powershell
python scripts\update\publish_update.py `
  --platform windows `
  --private-key D:\secure\alcedo-update-private.seed `
  --dry-run
```

This script:

1. Reads version, build number, and update channel from the platform's CMake
   cache.
2. Records `git rev-parse HEAD` as `commit`. A dirty worktree is refused.
3. Finds the package in the fixed output directory: Windows uses the NSIS
   `build/release/package/*.exe`; macOS uses
   `build/macos-release/package/*.zip` plus its `.dmg`.
4. Writes a **single-platform** `update-manifest.json` (schema 1) that embeds
   the approved notes, this platform's build, and the packaged commit.
5. Signs with `alcedo_update_signer` and verifies the signature and package
   hashes.
6. For a real **stable** upload, refuses unless `commit` is already an ancestor
   of `origin/main`. Merge the release commit first, then package from that
   merge commit. This repository does not squash or rebase merge commits.
7. Uploads immutable build files first, the platform live signature next, and
   the platform live manifest last.

Real upload reads `R2_*` from the environment or `rust/puerh_mind/.env.test`.

The channel is read from the package's CMake cache, so the normal upload command
still requires only platform and private key. `--channel` is an optional safety
override and must match the packaged channel.

Every object lives under `updates/v1/<channel>/`. Website downloads use the same
live manifest the app checks. There is no parallel `releases/` tree.

| Purpose | Object key |
|---|---|
| Update package | `updates/v1/<channel>/builds/<build>/<platform>/<package>` |
| Manual macOS DMG | `updates/v1/<channel>/builds/<build>/macos-arm64/<dmg>` |
| Checksums | `updates/v1/<channel>/builds/<build>/<platform>/SHA256SUMS-<platform>.txt` |
| Signed archive manifest | `updates/v1/<channel>/builds/<build>/<platform>/manifests/<sequence>/manifest.*` |
| Live feed pointer | `updates/v1/<channel>/<platform>/manifest.*` |

A signed stable or beta manifest must point at the package in its own
`builds/<build>/<platform>/` directory. The live pointer is the only mutable
object. Re-running a failed publish reuses an existing immutable object when
its key, exact size, and recorded SHA-256 match.

The local publisher shows transfer progress for packages larger than 1 MiB. It
disables the AWS CLI socket read timeout and enables ten standard retry attempts
for slow upstream connections.

## GitHub archive (stable only, after both platforms are live)

The `Archive stable update to GitHub` workflow is manual. It is not a publisher.

It reads the two public live stable manifests, verifies both signatures, and
fails unless they share the same `version` and the same `commit` on
`origin/main`. Build numbers may differ. It then creates `v<version>` on that
commit, writes a GitHub release titled
`Alcedo Studio <version> (windows <win-build>/macOS <mac-build>)`, copies the
reviewed notes, appends the official `updates/` download URLs, and attaches the
packages as archive assets.

It does not have R2 credentials. It never reads a beta feed. If you have not
uploaded a stable pair, this workflow must fail.

A Windows-only or macOS-only hotfix stays on R2 until the other platform is
built from the same commit. Do not run the archive job against a mixed pair.

## Official stable sequence

1. Merge the last feature work to `main`.
2. Generate and approve the version notes. Commit them as the last commit of
   that version.
3. On each package machine, check out that merge commit on `main`. Run the
   **Release** package task (stable).
4. Upload that platform with `publish_update.py`. That platform is now live.
5. After both live stable manifests show the same version and commit, run
   **Archive stable update to GitHub** by hand.

Until step 4 happens for a platform, nothing user-facing changes. Running the
archive workflow does not perform step 4.

## Packaged end-to-end install test

Use the **Beta** package tasks so the test cannot replace the stable feed. The
package script prints the selected build number; the second successful package
gets the next number automatically.

### Windows

1. Package A, then install the generated NSIS `.exe` before packaging B:

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\package_windows.ps1 `
     -Channel beta
   ```

2. Start installed A. Open Settings > Updates. Confirm the status card has
   readable text before/while checking.
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
   contain only `windows-x86_64`, a 40-character `commit`, and the printed live
   alias must end in `updates/v1/beta/windows-x86_64/manifest.json`. Its package
   URL must start with
   `https://static.aoraw.org/updates/v1/beta/builds/<build>/windows-x86_64/`.
5. Put `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, and
   `R2_SECRET_ACCESS_KEY` in the environment or the ignored
   `rust/puerh_mind/.env.test`, then publish:

   ```powershell
   python scripts\update\publish_update.py --platform windows `
     --private-key D:\secure\alcedo-update-private.seed
   ```

6. In installed A, open Settings > Updates and click Check for updates. Verify
   all of these behaviors directly on the Updates page:

   - the installed and available versions include their build numbers;
   - the release notes appear in the page below the update actions, without a
     dialog or external link;
   - Download update shows percentage, transferred bytes, speed, and remaining time;
   - Cancel download reports a user cancellation, and a later check and download
     can start normally and resume the partial package;
   - the downloader rejects any size/hash mismatch and changes to Install and
     restart only after verification;
   - a copy of A installed outside `C:\Program Files` is replaced in that same
     directory; the updater must not create a second installation on drive C;
   - the same directory is retained when A uses the legacy helper that launches
     NSIS with `/S` only, because the new installer inherits A's registered path;
   - Install and restart closes A, runs the helper, installs B, and relaunches;
   - checking again from B reports up to date because its local build equals the
     signed remote build.

### macOS

The ZIP is the automatic-update package. The DMG remains the manual-download
package. Both are written under `build/macos-release/package/` and both are
archived under `updates/v1/beta/builds/<build>/macos-arm64/` when you publish a
beta build. Install A from the DMG into a **user-writable** folder before
packaging B. `/Applications` is often not writable without elevation; use
`~/Applications` or another directory you own so the helper can rename and
replace the `.app` in place.

1. Package A, then install it before packaging B:

   ```bash
   ./scripts/package_macos.sh --channel beta
   ```

   Open the generated DMG under `build/macos-release/package/`, copy
   `AlcedoStudio.app` to a writable location (for example `~/Applications`), and
   launch that copy. If Gatekeeper blocks the ad-hoc-signed build, open it once
   with Finder **Open** (right-click) and confirm. Note the build number printed
   by the package script.

2. Start installed A. Open Settings > About. Confirm the Updates row has readable
   text and says it is on the beta channel before/while checking.

3. Package B. This intentionally replaces the same-version package files in the
   fixed package directory:

   ```bash
   ./scripts/package_macos.sh --channel beta
   ```

4. Validate the complete signing and upload plan without R2 credentials:

   ```bash
   python3 scripts/update/publish_update.py --platform macos \
     --private-key /Volumes/secure/alcedo-update-private.seed --dry-run
   ```

   Inspect
   `build/tmp/update/beta/macos-arm64/update-manifest.json`: its `build` must
   equal the number printed by the B package run and be greater than A; it must
   contain only `macos-arm64`, a 40-character `commit`, and the printed live
   alias must end in `updates/v1/beta/macos-arm64/manifest.json`. Its package URL
   must start with
   `https://static.aoraw.org/updates/v1/beta/builds/<build>/macos-arm64/` and
   point at the `.zip`, not the `.dmg`. The DMG is listed as `manualUrl`.

5. Put `R2_ACCOUNT_ID`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, and
   `R2_SECRET_ACCESS_KEY` in the environment or the ignored
   `rust/puerh_mind/.env.test`, then publish:

   ```bash
   python3 scripts/update/publish_update.py --platform macos \
     --private-key /Volumes/secure/alcedo-update-private.seed
   ```

6. In installed A, open Settings > About and click Check for updates. Verify all
   of these behaviors directly in the About page:

   - the installed and available versions include their build numbers;
   - the changelog and release notes are readable without opening another dialog;
   - Download update shows percentage, transferred bytes, speed, and remaining time;
   - Cancel download reports a user cancellation, and a later check and download
     can start normally and resume the partial package;
   - the downloader rejects any size/hash mismatch and changes to Install and
     restart only after verification;
   - Install and restart closes A, runs
     `AlcedoStudio.app/Contents/Helpers/alcedo_update_installer`, extracts the
     ZIP with `ditto`, replaces the same `.app` path, and relaunches with
     `/usr/bin/open`;
   - the updated app remains at the same path as A (for example under
     `~/Applications`); the helper must not create a second copy under
     `/Applications` or another default location;
   - a backup bundle named
     `AlcedoStudio.app.update-backup-<UTC timestamp>` remains next to the new
     app after a successful install;
   - if A was installed in a non-writable folder, Install and restart fails
     cleanly, opens the verified ZIP for manual install, and does not delete the
     existing app;
   - checking again from B reports up to date because its local build equals the
     signed remote build.

## Manifest fields

Required: `schema` (1), `sequence`, `version`, `build`, `commit` (40-character
SHA of the packaged `main` commit), `publishedAt`, `expiresAt`, and the selected
platform entry under `artifacts` with `url`, `sha256`, and `size`. Each upload
contains only its own platform entry. The macOS artifact may also include
`manualUrl`, `manualSha256`, and `manualSize` for the DMG.

Required release information: `changelogs`, containing validated `en` and
`zh-CN` plain-text values (max 16 KiB each). `changelog` duplicates the English
value for compatibility with clients released before localized notes.

`notesUrl` remains accepted by older clients for schema-1 compatibility, but
new manifests do not generate it and the Updates page does not open GitHub.
The Updates page selects `changelogs.zh-CN` when the user's effective language
is Simplified Chinese and `changelogs.en` otherwise. Changing the language
updates the displayed text immediately without another network request.

The website should fetch
`https://static.aoraw.org/updates/v1/stable/<platform>/manifest.json` and use
`artifacts.<platform>.url` (Windows exe or macOS zip) or
`artifacts.macos-arm64.manualUrl` (DMG).

## Platform limits

This design does not use Authenticode, Developer ID, or Apple notarization.
Windows may show an unknown-publisher warning. macOS may require manual open
approval. A macOS app in a folder the user cannot write cannot replace itself.
