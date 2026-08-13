from copy import deepcopy

from tools.check_dependency_lock import ROOT, load_json, validate_lock


def locked_dependencies() -> dict[str, object]:
    return load_json(ROOT / "dependencies.lock.json")


def test_repository_dependency_lock_is_consistent() -> None:
    assert validate_lock(locked_dependencies()) == []


def test_dependency_lock_rejects_unpinned_commit() -> None:
    candidate = deepcopy(locked_dependencies())
    candidate["libdatachannel"]["commit"] = "main"  # type: ignore[index]
    assert "libdatachannel.commit has an invalid format" in validate_lock(candidate)


def test_dependency_lock_rejects_unsafe_transport_drift() -> None:
    candidate = deepcopy(locked_dependencies())
    candidate["libdatachannel"]["transport_contract"]["builtin_nack_responder_allowed"] = True  # type: ignore[index]
    assert "built-in NACK responder is True, expected False" in validate_lock(candidate)
