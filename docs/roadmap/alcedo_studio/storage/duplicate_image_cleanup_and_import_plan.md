# Import and Project Maintenance coordination

Updated: 2026-11-10

The work is split into two stacked branches. This branch provides import duplicate skipping.
Project Maintenance will follow on top of this branch and use the same full-path normalizer.

## Import duplicate skipping

See the [Import Duplicate Skipping Plan](import_duplicate_skipping_plan.md) for current behavior.
The key is the normalized full source path and byte size. Same-size changes can go undetected.
Changed-size sources keep existing edits, ratings, marks, IDs, and memberships.
The existing editor refresh and project persistence owners apply those edits to the current file.
No source-content hashes or prototype hash migration are retained.

## Shared ownership

`SourceImageFile` contains the full-path normalizer and protected source-size reader here.
The maintenance branch adds its source-status helper to that target. Source sizes remain owned
by Image storage. Maintenance will use focused storage removal operations for their cleanup.

## Scope and verification

This branch includes the preference, request overrides, counters, source refresh, and import tests.
It does not register maintenance services, UI, commands, tests, or source-status checks.
The stacked maintenance branch adds scan, protected-work selection, transactional removal,
verified backup, activation and recovery. Its feature plan will arrive with that implementation.

The combined reference branch is `de_duplicate`; it remains unchanged. The branch commits and
split report record independent build/test evidence. Earlier combined test records in the import
plan are historical evidence and do not claim that maintenance exists in this branch.
