# Alcedo Studio localization

Alcedo Studio uses Qt Linguist catalogs for the QML shell and its C++ services.
The source catalogs are versioned here:

- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts`
- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts`

The `.qm` files are build outputs. They are generated and embedded by
`qt_add_translations`; do not commit or edit them.

## Update the catalogs

Run the extraction target from the repository root through the Windows MSVC
wrapper:

```text
cmd /c scripts\msvc_env.cmd --build build/release-test --target alcedo_main_lupdate -j 4
```

This scans the C++ translation targets and the canonical `ALCEDO_MAIN_QML_FILES`
list. It uses relative source locations and removes obsolete messages. If a new
QML file belongs to `Alcedo.Main`, add it to `ALCEDO_MAIN_QML_FILES`; it will then
be included automatically on the next extraction.

Open the `.ts` file in Qt Linguist and translate only the unfinished entries.
Keep every `%1`, `%2`, and similar placeholder, and use the source context to
choose the right wording. English is the source language, so an English entry
can use the source text as its runtime fallback.

Compile the catalogs with:

```text
cmd /c scripts\msvc_env.cmd --build build/release-test --target alcedo_main_lrelease -j 4
```

The global `update_translations` and `release_translations` targets remain
available as well. A normal application build generates the release catalogs
as needed.

## Source markup rules

Every user-visible QML string belongs in `qsTr()` with a literal source string:

```qml
text: qsTr("Export %1 Files").arg(fileCount)
```

Use `%1` placeholders instead of concatenating translated fragments. Use a
disambiguation comment when identical source text has different meanings. C++
uses `QCoreApplication::translate()` or the existing translation helper macros.

After adding or changing source text, run `alcedo_main_lupdate`, translate the
new entries, and run `alcedo_main_lrelease`. This keeps source changes,
translation review, and generated runtime catalogs separate.
