# Releasing HPActor Python Package

## Prerequisites

- [ ] All Phase 1D acceptance gates pass
- [ ] Four-platform wheel CI green (`.github/workflows/python-wheels.yml`)
- [ ] Cross-minor CPython 3.11–3.14 compatibility verified
- [ ] Performance comparison shows no >20% regression
- [ ] Documentation builds without warnings

## Release Process

### 1. TestPyPI Rehearsal

1. Merge all changes to `main`
2. Run the `Publish Python Package` workflow with `workflow_dispatch`
   selecting `TestPyPI`
3. Verify the package installs from TestPyPI:

```bash
pip install --index-url https://test.pypi.org/simple/ hpactor
python3 -c "import hpactor; print(hpactor.__version__)"
```

### 2. Tag and Release

1. Tag the release commit on `main`:

```bash
git tag -a v0.1.0 -m "HPActor Python binding v0.1.0"
git push origin v0.1.0
```

2. The `Publish Python Package` workflow triggers automatically on the tag
3. Required reviewers approve the `pypi` environment deployment
4. Verify the package installs from PyPI:

```bash
pip install hpactor
python3 -c "import hpactor; print(hpactor.__version__)"
```

### 3. Post-Release

1. Create a GitHub release from the tag, attaching:
   - Four platform wheels
   - Source distribution
   - SHA256SUMS
   - Wheel audit reports
   - Performance report

2. Update documentation with the release link

3. Close the release milestone in the issue tracker

## Recovery

### Yanking a Bad Release

Never replace an artifact.  A published filename is immutable.

1. If caught before publish: stop the workflow
2. If already published: yank the bad version

```bash
# Yank the bad version
pip install --upgrade twine
twine yank hpactor==BAD.VERSION --reason "Bug description"

# Fix on a new commit
git commit -m "fix: ..."
git tag -a v0.1.1 -m "HPActor Python binding v0.1.1"
git push origin v0.1.1
```

3. Release a new patch version with the fix

### Never

- Overwrite a published wheel or sdist
- Delete a tag that has been pushed
- Use a long-lived API token for publishing
- Publish without the acceptance gate passing
