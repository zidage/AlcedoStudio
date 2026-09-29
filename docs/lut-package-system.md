# Alcedo LUT metadata and package publication

This document defines the CUBE metadata comment, the package inventory, the
signed LUT package feed, and the offline publication steps. The product plan is
[LUT Library and Package Management Plan](roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md).

Reference implementations:

| Part | C++ | Python |
| --- | --- | --- |
| Metadata comment | `alcedo_studio/src/utils/lut/lut_metadata.cpp` | `scripts/luts/alcedo_lut_metadata.py` |
| Package inventory digest | `alcedo_studio/src/utils/lut/lut_inventory_digest.cpp` | `scripts/luts/lut_inventory.py` |
| Local library scan | `alcedo_studio/src/utils/lut/lut_library_scan.cpp` | — |
| Signed feed | `alcedo_studio/src/app/lut_package_manifest.cpp` | `scripts/luts/prepare_lut_packages.py` |
| Package check and download | `alcedo_studio/src/app/lut_package_service.cpp` | — |
| Archive extraction and activation | `alcedo_studio/src/app/lut_package_install.cpp` | `scripts/luts/lut_package_archive.py` (verification) |

Shared test fixtures live in `alcedo_studio/tests/resources/lut_metadata/`.

## 1. CUBE metadata comment

A classified LUT has exactly one UTF-8 comment line before its numeric table:

```text
# ALCEDO_LUT {"schema":1,"id":"spectral_film_lut:kodak_vision3_250d_5207:kodak_vision_2383","origin":"alcedo","category":"film_simulation","source":{"id":"spectral_film_lut","name":"Spectral Film LUT"},"film":{"id":"kodak_vision3_250d_5207","name":"Vision3 250D 5207","brand":"Kodak"},"print":{"id":"kodak_vision_2383","name":"Vision 2383","brand":"Kodak","kind":"film"},"input_space":"ACEScc","output_space":"ACEScc"}
```

The marker is `ALCEDO_LUT` after `#` and optional spaces, followed by one space
and a compact JSON object. Exporters write the line as the first line of the
file. Attribution and license comments stay unchanged below it. A reader stops
at the first numeric row and never loads the numeric table to classify a file.

### 1.1 Fields

| Field | Required | Rule |
| --- | --- | --- |
| `schema` | yes | Integer `1`. Other values are an unsupported-schema error. |
| `id` | yes | Stable look identifier, at most 160 bytes, matching `[a-z0-9]+([._:-][a-z0-9]+)*`. |
| `origin` | yes | `alcedo` or `user`. The author's declaration, not a signature. |
| `category` | yes | `general` or `film_simulation`. |
| `source` | film simulation | `{"id","name"}`. `id` is a slug (below); `name` is display text. |
| `film` | film simulation | `{"id","name","brand"}`. `name` is the stock name without the brand. |
| `print` | no | `{"id","name","brand","kind"}` with `kind` `film` or `paper`. Absence means no print. |
| `variant` | no | Slug naming an intended calibration or look variant. |
| `input_space`, `output_space` | yes | Declared encoding, at most 64 bytes. Official packages use `ACEScc`. |
| `description` | no | At most 1024 bytes. |
| `aliases` | no | At most 16 strings of at most 128 bytes each. |
| `display_requirement` | no | At most 512 bytes. Preserves an exporter's required DRT/display. |

A slug matches `[a-z0-9]+([._-][a-z0-9]+)*` and is at most 96 bytes. Display
names (`name`, `brand`) are 1-128 bytes. No string may contain a control
character. `film` and `print` are not allowed on a `general` LUT. Unknown fields
are ignored within schema 1.

Limits: the header (bytes before the first numeric row) is at most 64 KiB and
the metadata line is at most 16 KiB. A second `ALCEDO_LUT` line is an error.
Malformed metadata is an error; the file stays visible with that error and is
never reinterpreted as an unannotated general LUT. A file without the comment is
an unannotated user LUT in `general`.

### 1.2 Official identities and canonical file names

For an `origin: alcedo` film simulation the ID and the file name are derived
from the metadata:

```text
id   = <source.id>:<film.id>[:<print.id>][:<variant>]
file = <film.id>[__<print.id>][__<variant>].cube
```

`prepare_lut_packages.py` rejects official files whose ID or file name differs.
Because slugs never contain `__`, a file-name prefix of `<film.id>` finds every
print option and variant of one film. The browser shows official LUTs by film
name and lists the print as an option of that film.

User LUTs keep their own file names; the library uses the file stem as the LUT
name, and the render pipeline refers to the file path. Equal names in different
folders are separate entries.

### 1.3 Exporter modes

A generator writes `origin: alcedo` only in its explicit Alcedo package export
mode. Every other export writes `origin: user` or no comment. The metadata is
supplied by the export caller from the actual film and print objects.

## 2. Package inventory

Each package archive contains `package-inventory.json` at its root:

```json
{
  "schema": 1,
  "kind": "alcedo-lut-package-inventory",
  "package_id": "spectral_film_lut",
  "revision": "2026.09.1",
  "file_count": 2,
  "inventory_sha256": "<64 lowercase hex>",
  "unpacked_bytes": 123456,
  "luts": [{"id": "...", "path": "kodak_vision3_250d_5207.cube", "size": 1, "sha256": "..."}],
  "auxiliary_files": [{"path": "LICENSE.txt", "size": 1, "sha256": "..."}]
}
```

`file_count` counts LUTs only. `unpacked_bytes` is the sum of LUT and auxiliary
file sizes; the inventory file itself is excluded. Auxiliary files (licenses,
attribution, change logs) are verified but never counted as LUTs.

### 2.1 Inventory digest

For each LUT record encode

```text
id NUL relative-path NUL decimal-byte-size NUL lowercase-sha256 LF
```

in UTF-8. Paths use `/`. Sort records by relative-path bytes, then ID bytes,
concatenate, and hash with SHA-256. Reject NUL or line breaks in an ID or path,
absolute paths, `..` and empty segments, duplicate IDs, and paths that collide
after case folding. `inventory_digest_records.json` in the fixture directory
holds records and the expected digest; the C++ and Python tests both check it.

### 2.2 Local library inventory

The application scans the library root only when the user requests a refresh.
The scan reads every CUBE header and computes SHA-256 only for files that
declare `origin: alcedo`. User LUTs are not hashed. Header reads and hashing run
on a bounded worker set. The result is written as `lut-inventory.json` in the
library root and read on later starts and panel openings without rescanning.
A missing or damaged `lut-inventory.json` is rebuilt from local files at start.

`LutLibraryService` owns the root and these files:

| Root entry | Contents |
| --- | --- |
| `lut-inventory.json` | Scan result: entries with path, size, write time, header status, metadata, SHA-256 (official files only), and the owning package ID |
| `lut-library.json` | User state: favorites as root-relative paths and the previous roots of migrated libraries |
| `lut-migration-cleanup.json` | Present only while a migration's source cleanup is pending: source root and the verified size and SHA-256 of each copied file |
| `user/` | Files imported by the application |
| `packages/<id>/installed.json` | Package activation receipt; the scan reads `package_id` and `content_directory` |
| `packages/<id>/content/<hash>/` | Package content; only the receipt's active directory is scanned |
| `.downloads/` | Partial downloads; never scanned or migrated |

A receipt has the form

```json
{"schema":1,"kind":"alcedo-lut-package-receipt","package_id":"<id>",
 "content_directory":"packages/<id>/content/<inventory_sha256>","revision":"2026.09.1",
 "file_count":42,"inventory_sha256":"<64 lowercase hex>","unpacked_bytes":123456,
 "feed_sequence":20260929120000,
 "artifact":{"url":"https://...7z","size":12345,"sha256":"<64 lowercase hex>"}}
```

The descriptor fields repeat the verified feed descriptor of the active content. Receipts without
them (written before package installation existed) are still read; a present field must be well
formed. An invalid receipt is a scan diagnostic, and its package content is not listed, so the
inventory is reported as incomplete. Only entries inside an active content directory carry a
package ID. A loose file that declares `origin: alcedo` is not package-owned.

Root migration copies every regular file except `.downloads`, verifies each copy with SHA-256,
writes the state files, and renames a staging directory beside the destination into place.
Persisting the new root preference is the commit point. Afterwards the source cleanup deletes
only files that still hash to the copied bytes, and resumes from `lut-migration-cleanup.json`
after an interruption.

## 3. Signed package feed

```json
{
  "schema": 1,
  "kind": "alcedo-lut-packages",
  "sequence": 20260929120000,
  "packages": [
    {
      "id": "spectral_film_lut",
      "revision": "2026.09.1",
      "file_count": 42,
      "inventory_sha256": "<64 lowercase hex>",
      "unpacked_bytes": 123456,
      "artifact": {
        "url": "https://static.aoraw.org/luts/v1/packages/spectral_film_lut/2026.09.1/spectral_film_lut-2026.09.1.7z",
        "size": 12345,
        "sha256": "<64 lowercase hex>"
      }
    }
  ]
}
```

The feed has no expiration field. It is signed with the software-update Ed25519
key (`alcedo_update_signer sign`) as a detached base64 signature over the exact
manifest bytes. The client rejects a sequence older than the trusted LUT
sequence; that setting is separate from the software-update sequence.
Artifact URLs must be HTTPS on the feed host and end in `.7z`.

## 4. Archives

One 7z archive per package, LZMA2 preset 9, no encryption, created with
`py7zr` (`pip install py7zr cryptography`; the feed signature is verified with `cryptography`). The archive contains only regular files with
relative paths: the LUTs, the auxiliary files, and `package-inventory.json`.

## 5. Publication

```powershell
python scripts/luts/prepare_lut_packages.py `
  --package spectral_film_lut=D:\lut_out\spectral_film_lut `
  --package spektrafilm_lut=D:\lut_out\spektrafilm_lut `
  --revision 2026.09.1 `
  --private-key D:\secure\alcedo-update-private.seed
python scripts/luts/publish_lut_packages.py            # plan only, no upload
python scripts/luts/publish_lut_packages.py --upload   # live upload
```

`prepare_lut_packages.py` validates every file, builds the archives under
`build/tmp/luts/`, re-reads each archive, creates and signs `manifest.json`, and
verifies the signature. Without `--private-key` it stops after the local
package validation. `publish_lut_packages.py` lists every object key and uploads
nothing unless `--upload` is given. Upload order: package archives (immutable),
archived manifest signature and manifest (immutable), then the live
`luts/v1/manifest.json.sig` and `luts/v1/manifest.json`. A client that reads a
mismatched pair during the short window rejects the signature and retries later.
The public prefix `luts/v1` is a deployment value supplied by the release
operator.

## 6. Client check and installation

The build reads the feed URL from the CMake cache value `ALCEDO_LUT_PACKAGE_FEED_URL` (HTTPS,
empty by default) and verifies it with the software-update public key. An empty value disables
package checks and downloads. The highest accepted feed sequence is stored in the setting
`lut/packages/highestTrustedSequence`, separate from the update sequence.

`LutPackageService` makes no network request at startup. `CheckPackages()` fetches
`manifest.json` (at most 256 KiB) and `manifest.json.sig` (at most 1 KiB) from the feed host only,
verifies them, and compares each listed package with the published local inventory without
reading or hashing files:

| Status | Rule |
| --- | --- |
| Not installed | No receipt for the package |
| Update available | The receipt's `inventory_sha256` differs from the feed descriptor |
| Current | The installed entries reproduce the descriptor's LUT count and inventory digest |
| Repair required | The receipt matches the descriptor, but the installed entries do not (changed bytes, missing files, or a file whose origin is now `user`) |

A check also requests one inventory refresh when the local inventory is incomplete.
Checking never downloads. `InstallPackage(id)` performs one package action:

1. Download `artifact.url` to `.downloads/<id>-<sha256 prefix>.7z` through the shared
   `DownloadService`. A running application or model download rejects the request (busy); no
   second transfer starts.
2. As a `LutLibraryService` operation (serialized with refresh, import, and root changes): check
   the archive size and SHA-256, remove content left by an interrupted installation, and extract
   into a new `packages/<id>/content/<inventory_sha256>[-n]` directory. Extraction uses the bundled
   libarchive (7z/LZMA2) and accepts only regular files and directories with safe relative paths.
   Links, special files, `..`, absolute or drive paths, backslashes, case-colliding duplicates, and
   bytes or files beyond the signed limits stop the extraction.
3. Compare `package-inventory.json` with the signed descriptor and every extracted file with its
   listed size and SHA-256. The archive must hold exactly the listed files and the inventory.
4. Write `feed-manifest.json` and `feed-manifest.json.sig` (the exact signed feed bytes) into the
   content directory, then atomically replace `installed.json`. This replacement is the commit
   point.
5. Retire the previous content directory. A `.cube` file in it that explicitly declares
   `origin: user` is first moved to `user/<id>/<path>` and kept as a user entry. Files that still
   declare `origin: alcedo` are replaced without a copy or a prompt.
6. Rescan, write `lut-inventory.json`, and publish the inventory.

Before the commit point, every failure and every cancellation removes the new directory and keeps
the previous receipt and content active. After it, the installation completes; if the inventory
cannot be written, the result reports that the package is installed and an inventory refresh is
required, and the next start rebuilds the inventory because it no longer agrees with the receipts.
The next start also retires content directories that no receipt names.
