# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-09-26

### Added
- Initial release
- Multi-stage testing: benchmark, sparse probe, retention, full verify
- macOS support (DKIOCGETBLOCKCOUNT/BKIOCGETBLOCKSIZE)
- Color-coded PASS/FAIL/INCONCLUSIVE output
- Fake-device backend for CI testing
- Checkpoint/resume support
- JSON output
- Unit + integration test suite with fake device policies
- Makefile build system (release, debug, asan, lint targets)
