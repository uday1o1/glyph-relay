from __future__ import annotations

import pytest

from tools.recording_crash_model import (
    CANONICAL_OPERATIONS,
    CrashModelError,
    Operation,
    verify_crash_protocol,
)


def _swap(first: Operation, second: Operation) -> tuple[Operation, ...]:
    operations = list(CANONICAL_OPERATIONS)
    left = operations.index(first)
    right = operations.index(second)
    operations[left], operations[right] = operations[right], operations[left]
    return tuple(operations)


def _commit_journal_before_media() -> tuple[Operation, ...]:
    operations = list(CANONICAL_OPERATIONS)
    media_sync = operations.index(Operation.SYNC_MEDIA_GROUP)
    operations[media_sync : media_sync + 3] = [
        Operation.WRITE_JOURNAL_GROUP,
        Operation.SYNC_JOURNAL_GROUP,
        Operation.SYNC_MEDIA_GROUP,
    ]
    return tuple(operations)


def test_all_filesystem_crash_cuts_are_safe() -> None:
    cuts = verify_crash_protocol()
    assert len(cuts) == len(CANONICAL_OPERATIONS)
    assert {cut.outcome for cut in cuts} == {
        "ABSENT",
        "PREPARED_INCOMPLETE",
        "INCOMPLETE",
        "COMPLETE",
    }
    assert all(cut.durable_journal_access_units <= cut.durable_media_access_units for cut in cuts)
    assert all(cut.outcome != "ABSENT" for cut in cuts if cut.admitted)


def test_seeded_journal_before_media_sync_fails_for_intended_reason() -> None:
    mutation = _commit_journal_before_media()
    with pytest.raises(CrashModelError, match="journal_commit_precedes_media"):
        verify_crash_protocol(mutation)


def test_seeded_admission_before_prepared_barrier_fails_for_intended_reason() -> None:
    mutation = _swap(Operation.SYNC_PREPARED_DIRECTORY, Operation.ADMIT_MEDIA)
    with pytest.raises(CrashModelError, match="media_admitted_before_prepared_barrier"):
        verify_crash_protocol(mutation)


def test_seeded_marker_before_publication_barrier_fails_for_intended_reason() -> None:
    mutation = _swap(Operation.SYNC_PUBLICATION_DIRECTORY, Operation.RENAME_MARKER)
    with pytest.raises(CrashModelError, match="marker_rename_precedes_publication_barrier"):
        verify_crash_protocol(mutation)
