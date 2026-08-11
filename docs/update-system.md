# Alcedo Studio update release steps

The application accepts only a manifest that has a valid Ed25519 signature. The private key must
stay offline. Do not add it to this repository or to GitHub Actions.

## One-time key setup

Build the signing tool. Then run it on a trusted computer:

If an updater-enabled build already exists, do not generate a replacement key. Copy the public
key that matches the existing private key to `alcedo_studio/src/config/update_public_key.txt`.
Only use `generate` when no update key exists yet.

```powershell
cmd /c scripts\msvc_env.cmd --build build\debug --target alcedo_update_signer
build\debug\alcedo_studio\src\alcedo_update_signer.exe generate `
  --private-key D:\secure\alcedo-update-private.seed `
  --public-key alcedo_studio\src\config\update_public_key.txt
```

The tool prints the public key as Base64. Commit `update_public_key.txt`. The public key is not a
secret. CMake reads this file automatically for VS Code, command-line, and CI configurations.
Save the same value as the GitHub repository variable `ALCEDO_UPDATE_PUBLIC_KEY_BASE64`.

On Windows, restrict the private key file to your user account. On macOS, use `chmod 600`.
Keep a separate offline backup. If you lose this key, installed applications cannot trust a new key.

```powershell
Get-Content alcedo_studio\src\config\update_public_key.txt
```

Select `win_release` or `macos_release` in the VS Code CMake Tools extension. No extra CMake
argument is required. The private key is a secret and must stay outside the repository.

Use `Tasks: Run Task` in VS Code to run `Alcedo: Package Windows Release` or
`Alcedo: Package macOS Release`. These tasks use the existing package scripts. The scripts
configure, install, verify the install tree, and run CPack.

CMake uses the project version for the app display version and the Windows file version. It uses a
monotonic numeric value for `CFBundleVersion` and update comparison. The default value for 0.2.9 is
2009. Set `-DALCEDO_BUILD_NUMBER=...` when two builds use the same display version.

## Prepare one release

Use the NSIS `.exe` for Windows. Use the CPack `.zip` for the macOS automatic update. Keep the DMG
as the manual download on the website.

Use Python 3 to create the manifest. If `python` is not on `PATH`, use the full path to the
Python 3 executable.

```powershell
python scripts\update\create_update_manifest.py `
  --version 0.2.9 --build 2009 --sequence 2026081101 --tag v0.2.9 `
  --windows build\release\AlcedoStudio-0.2.9-Windows-AMD64.exe `
  --macos-arm64 AlcedoStudio-0.2.9-Darwin-arm64.zip `
  --output update-manifest.json

build\debug\alcedo_studio\src\alcedo_update_signer.exe sign `
  --private-key D:\secure\alcedo-update-private.seed `
  --manifest update-manifest.json `
  --signature update-manifest.json.sig
```

Upload these files to the GitHub release before you publish it:

- The Windows NSIS `.exe`.
- The macOS `.dmg`.
- The macOS `.zip`.
- `update-manifest.json`.
- `update-manifest.json.sig`.

The R2 workflow checks the signature and every package hash. It uploads immutable release files
first. It publishes the stable manifest last.

Increase `sequence` for every published manifest. Never reuse a sequence. The build number must
also increase when the version can replace an older build.

The default manifest lifetime is 30 days. Refresh the stable metadata before it expires when no
new application release is ready. Create and sign a new manifest for the same package files. Keep
the same version and build. Use a larger sequence and new timestamps. Replace the two manifest
assets on the GitHub release. Then run `Sync release assets to R2` manually with `promote_latest`
enabled. The workflow stores each sequence at a new immutable R2 path.

## Current platform limitation

This design does not use Authenticode, Developer ID, or Apple notarization. Windows can show an
unknown-publisher warning. macOS can block the new application or require manual approval. A macOS
application in a directory that the current user cannot write cannot replace itself. In that case,
the user must install the ZIP or DMG manually.
