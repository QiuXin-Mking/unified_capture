# Task 1 report: product configuration parser and deployment templates

## Scope delivered

- Added dependency-free `core/product_config.h` and `core/product_config.cpp`.
  The parser accepts only the `mango` and `banana` product profiles, handles
  comments and ASCII whitespace, validates product and camera-map syntax,
  reports open/syntax/duplicate/validation failures as errors, and validates
  the required banana wrist-device settings.
- Added editable deployment templates at `deploy/product.conf.example` and
  `deploy/camera-map.conf.example`.  The camera map documents its installed
  `/etc/unified_capture/` destination and that `SL`/`JHHSW` are exact,
  editable Nori `iProduct` values.
- Added `tests/test_product_config.cpp`, which uses a unique temporary
  directory and checks valid banana parsing, an unknown product, and a missing
  `wrist_right.product` key.
- Added the parser source to both production source lists and the host-only
  `make test_product_config` target to the Makefile.  `make test` now runs the
  parser test before the existing host tests.

## TDD evidence

1. RED: after creating the test target but before adding the implementation,
   `make test_product_config` failed because the required
   `core/product_config.cpp` prerequisite did not exist.
2. GREEN: after adding the parser, `make test_product_config` compiled and ran
   `build/tests/test_product_config` successfully.

## Verification

Command:

```sh
make test && git diff --check
```

Result: exit code 0.  The parser test and all existing host-only tests passed:
`test_output_path`, `test_time_utils`, `test_video_capture_control`,
`test_socket_command`, and `test_source_layout`.  `git diff --check` produced
no whitespace errors.

## Commit

- `66f80d9edfafeb6cadac911858fef4698d413e98` — `feat: load capture product configuration`

## Concerns

None.  This slice deliberately does not change runtime default paths,
microphone behavior, Nori/MPP/FFmpeg integration, or later wrist-profile work.
