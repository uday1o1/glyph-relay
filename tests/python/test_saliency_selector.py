from __future__ import annotations

import copy
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from tools.check_saliency_protocol import ROOT, validate_protocol
from tools.corpus import saliency_selector
from tools.corpus.saliency_selector import (
    GRID_PATH,
    SaliencyConfiguration,
    SaliencySelectionError,
    canonical_json,
    enumerate_grid,
    load_object,
    run_selection,
    select_development_evidence,
    sha256_file,
    verify_protocol_lock,
)
from tools.validate_saliency_selection import validate_artifacts

SHA = "a" * 64
STRATA = (
    "animated_typing_scrolling",
    "browser_documentation",
    "code_editor",
    "mixed_video_text",
    "slide_diagram",
    "spreadsheet_table",
    "terminal",
)


def map_metrics(
    *,
    overall: float = 0.91,
    small: float = 0.81,
    protected: float = 0.30,
    false_protected: float = 0.10,
    change: float = 0.01,
) -> dict[str, float]:
    return {
        "overallGlyphRecall": overall,
        "smallGlyphRecall": small,
        "protectedFraction": protected,
        "falseProtectedFraction": false_protected,
        "falseDiscoveryFraction": false_protected / max(protected, 1e-12),
        "staticMapChangeFraction": change,
    }


def metrics(*, processing: int = 100, **overrides: float) -> dict[str, Any]:
    aggregate = map_metrics(**overrides)
    return {
        **aggregate,
        "processingP95Ns": processing,
        "perStratum": {stratum: dict(aggregate) for stratum in STRATA},
        "p95Sequence": dict(aggregate),
    }


@pytest.fixture(scope="module")
def grid() -> dict[str, Any]:
    return load_object(GRID_PATH)


@pytest.fixture(scope="module")
def configurations(grid: dict[str, Any]) -> tuple[SaliencyConfiguration, ...]:
    return enumerate_grid(grid)


def invalid_candidate(configuration: SaliencyConfiguration) -> dict[str, Any]:
    return {
        "configuration": configuration.json(),
        "status": "INVALID",
        "invalidReason": "target_evaluation_rejected",
        "metrics": None,
    }


def passed_candidate(
    configuration: SaliencyConfiguration, candidate_metrics: dict[str, Any]
) -> dict[str, Any]:
    return {
        "configuration": configuration.json(),
        "status": "PASSED",
        "invalidReason": None,
        "metrics": candidate_metrics,
    }


def make_evidence(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> dict[str, Any]:
    return {
        "schemaVersion": 1,
        "protocol": "saliency_v1",
        "split": "development",
        "sourceBundleId": SHA,
        "automaticMapImplementationSha256": "b" * 64,
        "processingPlatformSha256": "c" * 64,
        "corpusProtocolSha256": grid["corpusProtocolSha256"],
        "developmentManifestSha256": grid["developmentManifestSha256"],
        "developmentRenderIndexSha256": grid["developmentRenderIndexSha256"],
        "gridSha256": sha256_file(GRID_PATH),
        "candidates": [invalid_candidate(configuration) for configuration in configurations],
    }


def fake_lock() -> dict[str, Any]:
    return {
        "files": [
            {
                "path": "tools/corpus/saliency_selector.py",
                "sha256": SHA,
            }
        ]
    }


def candidate_index(evidence: dict[str, Any], configuration: SaliencyConfiguration) -> int:
    wanted = configuration.json()
    return next(
        index
        for index, candidate in enumerate(evidence["candidates"])
        if candidate["configuration"] == wanted
    )


def two_candidate_selection(
    configurations: tuple[SaliencyConfiguration, ...],
    grid: dict[str, Any],
    first_metrics: dict[str, Any],
    second_metrics: dict[str, Any],
) -> tuple[dict[str, Any], SaliencyConfiguration, SaliencyConfiguration]:
    lexical = sorted(configurations, key=lambda configuration: canonical_json(configuration.json()))
    first, second = lexical[:2]
    evidence = make_evidence(configurations, grid)
    evidence["candidates"][candidate_index(evidence, first)] = passed_candidate(
        first, first_metrics
    )
    evidence["candidates"][candidate_index(evidence, second)] = passed_candidate(
        second, second_metrics
    )
    return select_development_evidence(evidence, grid, fake_lock()), first, second


def test_frozen_grid_has_exact_candidate_identity(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    weight_tuples = {
        (
            configuration.gradient_weight,
            configuration.contrast_weight,
            configuration.edge_pair_weight,
            configuration.small_structure_weight,
        )
        for configuration in configurations
    }
    assert len(weight_tuples) == 31
    assert len(configurations) == grid["expectedCandidateCount"] == 2511
    assert all(
        configuration.exit_threshold < configuration.entry_threshold
        for configuration in configurations
    )


@pytest.mark.parametrize(
    ("first", "second"),
    [
        (metrics(small=0.82), metrics(small=0.81)),
        (metrics(false_protected=0.09), metrics(false_protected=0.10)),
        (metrics(protected=0.29), metrics(protected=0.30)),
        (metrics(change=0.009), metrics(change=0.01)),
        (metrics(processing=99), metrics(processing=100)),
        (metrics(), metrics()),
    ],
)
def test_selector_applies_every_declared_tie_break_in_order(
    configurations: tuple[SaliencyConfiguration, ...],
    grid: dict[str, Any],
    first: dict[str, Any],
    second: dict[str, Any],
) -> None:
    selection, expected, _ = two_candidate_selection(configurations, grid, first, second)
    assert selection["configuration"] == expected.json()
    assert selection["eligibleCandidateCount"] == 2


def test_selector_discards_candidate_that_misses_a_threshold(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    selection, _, expected = two_candidate_selection(
        configurations,
        grid,
        metrics(small=0.799),
        metrics(small=0.80),
    )
    assert selection["configuration"] == expected.json()
    assert selection["eligibleCandidateCount"] == 1


def test_selector_requires_the_complete_unique_grid(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    missing = make_evidence(configurations, grid)
    missing["candidates"].pop()
    with pytest.raises(SaliencySelectionError, match="schema_invalid:candidates"):
        select_development_evidence(missing, grid, fake_lock())

    duplicate = make_evidence(configurations, grid)
    duplicate["candidates"][-1] = copy.deepcopy(duplicate["candidates"][0])
    with pytest.raises(SaliencySelectionError, match="development_candidate_duplicate"):
        select_development_evidence(duplicate, grid, fake_lock())


def test_selector_rejects_incomplete_stratum_evidence(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    evidence = make_evidence(configurations, grid)
    evidence["candidates"][0] = passed_candidate(configurations[0], metrics())
    del evidence["candidates"][0]["metrics"]["perStratum"]["terminal"]
    with pytest.raises(SaliencySelectionError, match="schema_invalid"):
        select_development_evidence(evidence, grid, fake_lock())


def test_selector_rejects_wrong_split_and_frozen_identity(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    evidence = make_evidence(configurations, grid)
    evidence["split"] = "validation"
    with pytest.raises(SaliencySelectionError, match="schema_invalid:split"):
        select_development_evidence(evidence, grid, fake_lock())

    evidence = make_evidence(configurations, grid)
    evidence["gridSha256"] = "d" * 64
    with pytest.raises(SaliencySelectionError, match="gridSha256"):
        select_development_evidence(evidence, grid, fake_lock())


def test_selector_fails_when_no_candidate_passes(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    evidence = make_evidence(configurations, grid)
    with pytest.raises(SaliencySelectionError, match="no_development_candidate"):
        select_development_evidence(evidence, grid, fake_lock())


def test_user_facing_selector_refuses_leaked_or_existing_output(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    validation = tmp_path / "validation"
    validation.mkdir()
    monkeypatch.setattr(saliency_selector, "FORBIDDEN_RENDER_OUTPUTS", (validation,))
    with pytest.raises(SaliencySelectionError, match="refuses_open_validation"):
        run_selection(tmp_path / "evidence.json", tmp_path / "selection.json")

    monkeypatch.setattr(saliency_selector, "FORBIDDEN_RENDER_OUTPUTS", ())
    output = tmp_path / "selection.json"
    output.write_text("reserved\n", encoding="utf-8")
    with pytest.raises(SaliencySelectionError, match="selection_output_exists"):
        run_selection(tmp_path / "evidence.json", output)


def test_public_cli_freezes_and_reproduces_selection(
    tmp_path: Path,
    configurations: tuple[SaliencyConfiguration, ...],
    grid: dict[str, Any],
) -> None:
    evidence = make_evidence(configurations, grid)
    evidence["candidates"][0] = passed_candidate(configurations[0], metrics())
    evidence_path = tmp_path / "development-evidence.json"
    selection_path = tmp_path / "selected-configuration.json"
    evidence_path.write_text(json.dumps(evidence), encoding="utf-8")

    selected = subprocess.run(
        [
            sys.executable,
            "tools/corpus/saliency_selector.py",
            "--development-evidence",
            str(evidence_path),
            "--output",
            str(selection_path),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert selected.returncode == 0, selected.stdout + selected.stderr
    frozen = load_object(selection_path)
    assert frozen["configuration"] == configurations[0].json()

    reproduced = subprocess.run(
        [
            sys.executable,
            "tools/validate_saliency_selection.py",
            "--development-evidence",
            str(evidence_path),
            "--selection",
            str(selection_path),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert reproduced.returncode == 0, reproduced.stdout + reproduced.stderr
    assert '"status": "VALID"' in reproduced.stdout

    no_clobber = subprocess.run(
        [
            sys.executable,
            "tools/corpus/saliency_selector.py",
            "--development-evidence",
            str(evidence_path),
            "--output",
            str(selection_path),
        ],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert no_clobber.returncode == 1
    assert "selection_output_exists" in no_clobber.stdout


def test_protocol_and_independent_validator_reject_seeded_selection_defect(
    configurations: tuple[SaliencyConfiguration, ...], grid: dict[str, Any]
) -> None:
    errors, protocol_sha256 = validate_protocol(ROOT)
    assert errors == []
    assert protocol_sha256 is not None

    evidence = make_evidence(configurations, grid)
    evidence["candidates"][0] = passed_candidate(configurations[0], metrics())
    selection = select_development_evidence(evidence, grid, verify_protocol_lock())
    validate_artifacts(evidence, selection)

    changed = copy.deepcopy(selection)
    changed["configurationSha256"] = "d" * 64
    with pytest.raises(SaliencySelectionError, match="does_not_reproduce"):
        validate_artifacts(evidence, changed)
