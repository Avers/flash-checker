# Agent Rules for flashcheck

## Primary Rule

**ALL changes to this repository require explicit developer approval before committing or pushing.**

No AI agent may commit, push, merge, or modify any file without:
1. Developer reviewing the proposed change
2. Developer explicitly approving the change
3. Agent waiting for approval before proceeding

## When Working on This Repository

### Before Any Change
- Read the full context: `README.md`, `CONTRIBUTING.md`, `Makefile`, relevant source files
- Explain the proposed change to the developer
- Wait for explicit approval ("yes", "approved", "proceed") before making any modification

### Making Changes
- Never push to `main` directly
- Create a branch: `ai/feature-name` or `ai/fix-description`
- Do not commit without approval
- Do not amend or force-push without approval
- Do not run `make install` or modify system files without approval

### Testing
- Run `make clean && make` to verify build
- Run `make test` to verify tests pass
- Report results to the developer
- Do not skip any test or ignore any failure

### What You Cannot Do Without Approval
- ❌ Cannot commit or push
- ❌ Cannot create or delete branches
- ❌ Cannot modify `VERSION`, `CHANGELOG.md`, or `Makefile`
- ❌ Cannot modify safety-critical code (`safety.c`, `device.c`, `io/sync.c`)
- ❌ Cannot add external dependencies
- ❌ Cannot change the build system
- ❌ Cannot modify the safety interlock
- ❌ Cannot push to `main`
- ❌ Cannot create PRs
- ❌ Cannot merge any branch

### What You Can Do Without Approval
- ✅ Read files and analyze code
- ✅ Explain code and architecture
- ✅ Run `make` and `make test` to verify
- ✅ Suggest changes
- ✅ Create branches (after approval)
- ✅ Edit files as long as you don't commit/push

## Communication Protocol

1. **Before every action**: Explain what you're about to do
2. **Before every commit**: Show the diff and wait for approval
3. **Before every push**: Confirm the branch and target
4. **Always report**: Test results, build status, any errors

## Safety

flashcheck writes to raw block devices (`/dev/disk*`). Never:
- Test on a real device without explicit developer approval
- Run `--destructive` mode without approval
- Modify or write to `/dev/` paths

## Release Checklist

Before tagging a release, verify:
1. `make clean && make` passes
2. `make test` passes (all checks green)
3. `CHANGELOG.md` has an entry for the new version
4. `VERSION` matches the tag (use `make bump part=patch|minor|major`)
5. The tag is pushed to origin (`git push origin vX.Y.Z`)
6. GitHub Actions workflow runs successfully (check `gh run list --workflow=release.yml`)
7. Release has all 8 assets (4 platforms x tar.gz + sha256)

**Never** modify `.github/workflows/release.yml` without understanding the full matrix (Linux x86_64/arm64 with `FLASHCHECK_IO_URING=1`, macOS x86_64/arm64 with `=0`). The workflow must run tests before packaging and include `LICENSE`, `README.md`, and `install.sh` in each tarball.

## Summary

> **No commit, no push, no merge without developer approval.**
> **When in doubt, ask.**
