# ADR 0003: Scope frozen evaluation build locks

## Status

Accepted.

## Context

The initial `uniform_aq_v1` and `saliency_validation_v1` locks included the repository's complete root `CMakeLists.txt`.
That root file also wires unrelated product and later-milestone targets.
Consequently, an unrelated target addition changed both evaluation protocol identities even though their source, compile policy, inputs, schemas, and execution contract were unchanged.
Repeatedly resealing those identities would make an immutable evaluation protocol depend on unrelated build growth.

## Decision

The two protocols bind `cmake/FrozenEvaluationTargets.cmake` instead of the root build file.
The dedicated fragment owns the complete target definitions, core linkage, warning policy, and platform compiler policy for both frozen evaluators.
The root build includes that fragment but is no longer protocol material.

Any change that affects either evaluator's compilation still changes both locks.
An unrelated target or test registration outside the fragment does not.

## Consequences

The protocol identities are resealed once before target selection or validation access.
Future implementation milestones can add unrelated targets without invalidating the frozen evaluation contracts.
Moving evaluator wiring out of the fragment or weakening its compile policy remains a detectable protocol change.
