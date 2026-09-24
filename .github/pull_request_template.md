## Problem and behavior

Describe the observable problem, changed behavior and ownership/compatibility contract.

## Validation

- [ ] Relevant tests and example builds passed; exact commands/results recorded.
- [ ] Lifetime/concurrency changes have ASan/UBSan and TSan evidence or an explicit environment limitation.
- [ ] Embedded changes consider allocation, capacity, stack, no-exception builds and task/ISR context.
- [ ] Performance-sensitive changes include same-machine baseline/current evidence with five samples, refs and feature settings, or explain why measurement is not applicable.
- [ ] Required safety is retained; costly optional behavior stays opt-in.
- [ ] README features, costs, examples and limitations reflect implemented behavior.
- [ ] Code/docs/issue/PR language is product-agnostic; shared C++23/standard-library paths are reused.

## Performance and limitations

Link captured evidence, explain regressions and list unmet acceptance criteria.
