## Summary

<!-- Brief description of what this PR does and why. -->

## Type of change

<!-- Mark with x what applies. -->

- [ ] Bug fix
- [ ] New feature / enhancement
- [ ] Documentation
- [ ] Refactor / cleanup
- [ ] Wire-protocol change

## Checklist

- [ ] Commits are short, bullet-pointed, present-tense
- [ ] Linked the related issue (`Fixes #N` or `Refs #N`)

### If this changes the wire protocol

- [ ] `PROTOCOL.md` updated (verb, args, return shape, capability flag)
- [ ] Agent source updated (`agents/windows-modern/`, `agents/windows-nt/` where applicable)
- [ ] Conformance test added under `tests/conformance/`
- [ ] Conformance suite passes against a healthy agent: `python tests/conformance/run.py <host> 8765`

## Related issues

<!-- e.g. Fixes #12, Refs #34 -->
