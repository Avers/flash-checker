# Contributing to flashcheck

All changes to this repository require approval from the project developer before being merged.

## Development Rules

### 1. All Changes Require Developer Agreement

- **No changes are merged without explicit approval from the developer**
- Every commit, PR, and modification must be reviewed and approved
- The developer has final say on all code changes

### 2. Workflow

1. **Create a branch** for your change: `feature/your-feature-name` or `fix/issue-description`
2. **Make your changes** following the existing code style
3. **Ensure all tests pass**: `make test`
4. **Ensure build succeeds**: `make`
5. **Commit with a clear message** describing the change
6. **Open a PR** — do not push directly to `main`
7. **Wait for developer approval** before merging

### 3. Code Style

- C11 standard (`-std=c11`)
- `-Wall -Wextra -Werror -Wshadow -Wpointer-arith -Wcast-qual -Wno-unused-parameter`
- No external dependencies
- Follow existing naming conventions and code patterns
- All new code must compile without warnings

### 4. Testing

Before submitting changes:

```sh
make clean
make          # build release
make test     # run all tests
make debug    # build with sanitizers
make asan     # run tests under sanitizers
```

All tests must pass before any change is considered.

### 5. Version Changes

- Version bumps are done via `make bump part=patch|minor|major`
- CHANGELOG.md must be updated with every change
- VERSION file is the single source of truth

### 6. Commit Rules

- Write clear, descriptive commit messages
- Each commit should be a single logical change
- No direct pushes to `main` branch
- Force-push only with developer approval

### 7. What NOT to Do

- **Do not push to `main` directly**
- **Do not merge without approval**
- **Do not skip tests**
- **Do not add external dependencies**
- **Do not modify build system without approval**
- **Do not change safety interlocks without developer agreement**

### 8. Communication

- Discuss planned changes with the developer before writing code
- If unsure about any aspect, ask before committing
- The developer may request changes to any submitted code

## Safety

flashcheck writes to raw block devices. Any changes that affect:
- The safety interlock system
- The `--destructive` flag behavior
- Device access permissions
- Data integrity verification

**Require extra scrutiny and explicit developer approval.**

## Summary

> **All changes to this repository require developer agreement.** No exceptions.
