from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable
import json


@dataclass(frozen=True)
class BenchmarkAssetManifest:
    manifest_path: str
    version: str
    default_asset: str
    lane_defaults: dict[str, str]
    scene_defaults: dict[str, str]
    lane_metadata: dict[str, dict[str, Any]]
    chunked_asset_ladder: dict[str, dict[str, Any]]
    # Issue #669: minimum splat count each fixture must carry for a benchmark lane to
    # be treated as evidence. Empty when the manifest predates the contract.
    asset_min_splat_counts: dict[str, int]

    def min_splat_count_for(self, asset_path: str) -> int:
        """Return the declared minimum splat count for an asset, or 0 if undeclared."""
        return int(self.asset_min_splat_counts.get(asset_path, 0))


@dataclass(frozen=True)
class LaneAssetPolicy:
    lane_id: str
    scene_id: str
    asset_path: str
    asset_source: str
    resource_kind: str
    asset_classification: str
    evidence_role: str
    notes: str
    require_explicit_lane_default: bool


CHUNKED_LADDER_REF_PREFIX = "chunked_ladder:"
RUNNABLE_CHUNKED_STAGING_STATUS = "materialized"
ALLOWED_CHUNKED_WORLD_CONTRACT_LANES = frozenset({"open_world_corridor_proof"})
PUBLISHED_BASELINE_EVIDENCE_ROLE = "published_baseline"
# Classifications that describe a smoke-sized workload. A lane classified this
# way may not also be the published baseline (#790).
LIGHTWEIGHT_SMOKE_CLASSIFICATIONS = frozenset({"lightweight_smoke", "generated_dummy"})


def resolve_benchmark_asset_manifest_path(
    manifest_arg: str,
    *,
    repo_root: Path,
    project_path: Path,
) -> Path:
    if manifest_arg:
        return Path(manifest_arg).expanduser().resolve()
    project_local = project_path / "tests" / "fixtures" / "benchmark_asset_manifest.json"
    if project_local.exists():
        return project_local
    return (repo_root / "tests" / "fixtures" / "benchmark_asset_manifest.json").resolve()


def scene_id_from_scene_path(scene_path: str) -> str:
    if not scene_path:
        return "unknown_scene"
    scene_id = Path(scene_path).stem.strip().lower()
    if scene_id.startswith("lane_"):
        return "benchmark_suite_lane"
    return scene_id


def load_benchmark_asset_manifest(path: str | Path) -> BenchmarkAssetManifest:
    manifest_path = Path(path)
    try:
        with manifest_path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError as exc:
        raise ValueError(f"--asset-manifest file not found: {manifest_path}") from exc
    except OSError as exc:
        raise ValueError(f"--asset-manifest could not be read: {manifest_path} ({exc})") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"--asset-manifest must be valid JSON: {exc}") from exc

    if not isinstance(data, dict):
        raise ValueError("--asset-manifest must be a JSON object")

    if "lane_defaults" not in data and "scene_defaults" not in data and "default_asset" not in data:
        lane_defaults = _validate_string_map(data, "lane defaults")
        return BenchmarkAssetManifest(
            manifest_path=str(manifest_path),
            version="1-flat",
            default_asset="",
            lane_defaults=lane_defaults,
            scene_defaults={},
            lane_metadata={},
            chunked_asset_ladder={},
            asset_min_splat_counts={},
        )

    lane_defaults = _validate_string_map(data.get("lane_defaults", {}), "lane_defaults")
    scene_defaults = _validate_string_map(data.get("scene_defaults", {}), "scene_defaults")
    lane_metadata = _validate_metadata_map(data.get("lane_metadata", {}))
    default_asset = data.get("default_asset", "")
    if default_asset is None:
        default_asset = ""
    if not isinstance(default_asset, str):
        raise ValueError("--asset-manifest default_asset must be a string")

    version = data.get("version", "")
    if version is None:
        version = ""
    if not isinstance(version, str):
        raise ValueError("--asset-manifest version must be a string")

    chunked_asset_ladder = _validate_nested_object_map(data.get("chunked_asset_ladder", {}), "chunked_asset_ladder")
    asset_min_splat_counts = _validate_int_map(
        data.get("asset_min_splat_counts", {}), "asset_min_splat_counts"
    )

    return BenchmarkAssetManifest(
        manifest_path=str(manifest_path),
        version=version,
        default_asset=default_asset,
        lane_defaults=lane_defaults,
        scene_defaults=scene_defaults,
        lane_metadata=lane_metadata,
        chunked_asset_ladder=chunked_asset_ladder,
        asset_min_splat_counts=asset_min_splat_counts,
    )


def _validate_int_map(value: Any, label: str) -> dict[str, int]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"--asset-manifest {label} must be a JSON object")
    out: dict[str, int] = {}
    for key, raw in value.items():
        if not isinstance(key, str):
            raise ValueError(f"--asset-manifest {label} keys must be strings")
        if isinstance(raw, bool) or not isinstance(raw, int):
            raise ValueError(f"--asset-manifest {label}[{key}] must be an integer")
        if raw < 0:
            raise ValueError(f"--asset-manifest {label}[{key}] must be non-negative")
        out[key] = raw
    return out


def resolve_lane_asset_policy(
    manifest: BenchmarkAssetManifest,
    *,
    lane_id: str,
    scene_path: str,
    generated_asset_path: str = "",
) -> LaneAssetPolicy:
    scene_id = scene_id_from_scene_path(scene_path)
    metadata = manifest.lane_metadata.get(lane_id, {})
    resource_kind = str(metadata.get("resource_kind", "")).strip() or "gaussian_splat_asset"

    if generated_asset_path:
        return LaneAssetPolicy(
            lane_id=lane_id,
            scene_id=scene_id,
            asset_path=generated_asset_path,
            asset_source="generated_dummy",
            resource_kind="gaussian_splat_asset",
            asset_classification="generated_dummy",
            evidence_role="generated_dummy",
            notes="Generated by --generate-dummy-assets for local benchmark smoke runs.",
            require_explicit_lane_default=False,
        )

    asset_path = ""
    asset_source = "none"
    if lane_id in manifest.lane_defaults:
        asset_path = manifest.lane_defaults[lane_id]
        asset_source = "lane_default"
    elif scene_id in manifest.scene_defaults:
        asset_path = manifest.scene_defaults[scene_id]
        asset_source = "scene_default"
    elif manifest.default_asset:
        asset_path = manifest.default_asset
        asset_source = "default_asset"

    asset_classification = str(metadata.get("asset_classification", "")).strip()
    if not asset_classification:
        asset_classification = _classify_asset_path(asset_path)

    evidence_role = str(metadata.get("evidence_role", "")).strip() or "unspecified"
    notes = str(metadata.get("notes", "")).strip()

    chunked_entry = _resolve_chunked_asset_entry(manifest, asset_path)
    if chunked_entry is not None:
        resolved_asset_path = ""
        if resource_kind == "gaussian_world_contract" and lane_id in ALLOWED_CHUNKED_WORLD_CONTRACT_LANES:
            resolved_asset_path = _resolve_chunked_world_contract_path(chunked_entry)
            if resolved_asset_path:
                asset_source = "chunked_world_contract"
        else:
            resolved_asset_path = _resolve_runnable_chunked_asset_path(chunked_entry)
            if resolved_asset_path:
                asset_source = "chunked_ladder"
        if resolved_asset_path:
            asset_path = resolved_asset_path
        if not asset_classification:
            asset_classification = str(chunked_entry.get("asset_classification", "")).strip() or "custom_asset"
        if evidence_role == "unspecified":
            evidence_role = str(chunked_entry.get("evidence_role", "")).strip() or evidence_role
        if not notes:
            notes = str(chunked_entry.get("notes", "")).strip()

    return LaneAssetPolicy(
        lane_id=lane_id,
        scene_id=scene_id,
        asset_path=asset_path,
        asset_source=asset_source,
        resource_kind=resource_kind,
        asset_classification=asset_classification,
        evidence_role=evidence_role,
        notes=notes,
        require_explicit_lane_default=bool(metadata.get("require_explicit_lane_default", False)),
    )


def validate_manifest_lane_policies(
    manifest: BenchmarkAssetManifest,
    lanes: Iterable[tuple[str, str]],
) -> list[str]:
    failures: list[str] = []
    for lane_id, scene_path in lanes:
        metadata = manifest.lane_metadata.get(lane_id)
        if metadata is None:
            failures.append(f"lane={lane_id}: missing lane_metadata entry in asset manifest")
            continue

        asset_classification = str(metadata.get("asset_classification", "")).strip()
        evidence_role = str(metadata.get("evidence_role", "")).strip()
        if not asset_classification:
            failures.append(f"lane={lane_id}: missing lane_metadata.asset_classification")
        if not evidence_role:
            failures.append(f"lane={lane_id}: missing lane_metadata.evidence_role")

        policy = resolve_lane_asset_policy(manifest, lane_id=lane_id, scene_path=scene_path)
        raw_lane_asset = manifest.lane_defaults.get(lane_id, "")
        explicit_lane_default_ok = policy.asset_source == "lane_default" or (
            raw_lane_asset.startswith(CHUNKED_LADDER_REF_PREFIX)
            and policy.asset_source in {"chunked_ladder", "chunked_world_contract"}
        )
        if policy.require_explicit_lane_default and not explicit_lane_default_ok:
            failures.append(
                f"lane={lane_id}: require_explicit_lane_default=true but resolved via {policy.asset_source}"
            )
        if raw_lane_asset.startswith(CHUNKED_LADDER_REF_PREFIX) and policy.asset_source not in {
            "chunked_ladder",
            "chunked_world_contract",
        }:
            chunked_entry = _resolve_chunked_asset_entry(manifest, raw_lane_asset)
            if policy.resource_kind == "gaussian_world_contract":
                if lane_id not in ALLOWED_CHUNKED_WORLD_CONTRACT_LANES:
                    failures.append(
                        f"lane={lane_id}: chunked world-contract lanes are only allowed for "
                        f"{', '.join(sorted(ALLOWED_CHUNKED_WORLD_CONTRACT_LANES))}"
                    )
                if chunked_entry is None or not _resolve_chunked_world_contract_path(chunked_entry):
                    failures.append(
                        f"lane={lane_id}: chunked world-contract lane is missing a stage manifest bootstrap path"
                    )
            elif chunked_entry is not None and not _resolve_runnable_chunked_asset_path(chunked_entry):
                failures.append(
                    f"lane={lane_id}: chunked ladder entry is not benchmark-consumable yet; "
                    "wait for materialized staged assets before wiring a runnable lane"
                )
            failures.append(
                f"lane={lane_id}: chunked ladder asset reference did not resolve through chunked_ladder"
            )
        if policy.asset_classification == "real_chunked" and _is_lightweight_smoke_asset(policy.asset_path):
            failures.append(
                f"lane={lane_id}: classified as real_chunked but resolves to lightweight smoke asset {policy.asset_path}"
            )
    failures.extend(validate_published_baseline_policy(manifest))
    return failures


def validate_published_baseline_policy(manifest: BenchmarkAssetManifest) -> list[str]:
    """Check the published-baseline lane actually describes a shippable workload (#790).

    The published headline figure is, by construction, whatever the lane
    carrying ``evidence_role: published_baseline`` measured. That makes the
    role a claim about the world, and there are two ways it becomes a lie:

    * the role sits on a lane whose workload is the lightweight canonical
      smoke fixture, so the published number describes a scene nobody ships
      (this is exactly the state #790 documents: ``static_baseline``, 10k
      splats, published at ~455 FPS while the resident path managed ~12);
    * more than one lane claims the role, so "the published number" is
      ambiguous and whichever lane is quoted becomes a matter of convenience.

    This is manifest-wide rather than per-selected-lane on purpose: the defect
    is a property of the manifest, and a run that happens not to select the
    offending lane must not launder it. Zero published-baseline lanes is
    permitted here because satellite manifests (the Steam Deck project) carry
    only handheld smoke lanes and publish nothing; the canonical manifests are
    pinned to exactly one in ``tests/ci/test_benchmark_fixture_contract.py``.
    """
    failures: list[str] = []
    baseline_lane_ids = sorted(
        lane_id
        for lane_id, metadata in manifest.lane_metadata.items()
        if isinstance(metadata, dict)
        and str(metadata.get("evidence_role", "")).strip() == PUBLISHED_BASELINE_EVIDENCE_ROLE
    )
    if len(baseline_lane_ids) > 1:
        failures.append(
            "manifest declares more than one "
            f"evidence_role={PUBLISHED_BASELINE_EVIDENCE_ROLE} lane "
            f"({', '.join(baseline_lane_ids)}); exactly one lane may define the published figure"
        )
    for lane_id in baseline_lane_ids:
        metadata = manifest.lane_metadata.get(lane_id, {})
        classification = str(metadata.get("asset_classification", "")).strip()
        if classification in LIGHTWEIGHT_SMOKE_CLASSIFICATIONS:
            failures.append(
                f"lane={lane_id}: evidence_role={PUBLISHED_BASELINE_EVIDENCE_ROLE} but "
                f"asset_classification={classification}; the published baseline may not be a "
                "lightweight smoke lane"
            )
        policy = resolve_lane_asset_policy(manifest, lane_id=lane_id, scene_path="")
        if _is_lightweight_smoke_asset(policy.asset_path):
            failures.append(
                f"lane={lane_id}: evidence_role={PUBLISHED_BASELINE_EVIDENCE_ROLE} but resolves to "
                f"lightweight smoke asset {policy.asset_path}; the published figure would describe "
                "the smoke fixture, not a shippable workload"
            )
    return failures


def _validate_string_map(value: Any, label: str) -> dict[str, str]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"--asset-manifest {label} must be a JSON object")
    out: dict[str, str] = {}
    malformed_entries: list[str] = []
    for key, item in value.items():
        if not isinstance(key, str) or not isinstance(item, str):
            malformed_entries.append(repr(key))
            continue
        out[key] = item
    if malformed_entries:
        raise ValueError(
            f"--asset-manifest {label} entries must map string keys to string values; "
            f"invalid keys: {', '.join(malformed_entries)}"
        )
    return out


def _validate_metadata_map(value: Any) -> dict[str, dict[str, Any]]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError("--asset-manifest lane_metadata must be a JSON object")
    out: dict[str, dict[str, Any]] = {}
    malformed_entries: list[str] = []
    for key, item in value.items():
        if not isinstance(key, str) or not isinstance(item, dict):
            malformed_entries.append(repr(key))
            continue
        clean_item: dict[str, Any] = {}
        for item_key, item_value in item.items():
            if not isinstance(item_key, str):
                malformed_entries.append(repr(key))
                clean_item = {}
                break
            if not isinstance(item_value, (str, bool, int, float)) and item_value is not None:
                malformed_entries.append(f"{key}.{item_key}")
                clean_item = {}
                break
            clean_item[item_key] = item_value
        if clean_item:
            out[key] = clean_item
    if malformed_entries:
        raise ValueError(
            "--asset-manifest lane_metadata entries must be objects with scalar values; "
            f"invalid entries: {', '.join(malformed_entries)}"
        )
    return out


def _validate_nested_object_map(value: Any, label: str) -> dict[str, dict[str, Any]]:
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"--asset-manifest {label} must be a JSON object")
    out: dict[str, dict[str, Any]] = {}
    malformed_entries: list[str] = []
    for key, item in value.items():
        if not isinstance(key, str) or not isinstance(item, dict):
            malformed_entries.append(repr(key))
            continue
        out[key] = item
    if malformed_entries:
        raise ValueError(
            f"--asset-manifest {label} entries must map string keys to objects; "
            f"invalid keys: {', '.join(malformed_entries)}"
        )
    return out


def _resolve_chunked_asset_entry(
    manifest: BenchmarkAssetManifest,
    asset_path: str,
) -> dict[str, Any] | None:
    if not asset_path.startswith(CHUNKED_LADDER_REF_PREFIX):
        return None
    asset_id = asset_path[len(CHUNKED_LADDER_REF_PREFIX) :].strip()
    if not asset_id:
        return None
    entry = manifest.chunked_asset_ladder.get(asset_id)
    if not isinstance(entry, dict):
        return None
    return entry


def _resolve_runnable_chunked_asset_path(entry: dict[str, Any]) -> str:
    if str(entry.get("staging_status", "")).strip() != RUNNABLE_CHUNKED_STAGING_STATUS:
        return ""
    staging = entry.get("staging", {})
    if not isinstance(staging, dict):
        return ""
    benchmark_asset_path = str(staging.get("project_benchmark_asset_path", "")).strip()
    return benchmark_asset_path


def _resolve_chunked_world_contract_path(entry: dict[str, Any]) -> str:
    bootstrap_builder = entry.get("bootstrap_world_builder", {})
    if not isinstance(bootstrap_builder, dict) or not bootstrap_builder:
        return ""
    staging = entry.get("staging", {})
    if not isinstance(staging, dict):
        return ""
    # Prefer pre-built staged world when the entry is materialized.
    if str(entry.get("staging_status", "")).strip() == RUNNABLE_CHUNKED_STAGING_STATUS:
        staged_world = str(staging.get("project_staged_world_path", "")).strip()
        if staged_world:
            return staged_world
    # Fall back to the bootstrap stage-manifest path for runtime synthesis.
    return str(staging.get("project_stage_manifest_path", "")).strip()


def _classify_asset_path(asset_path: str) -> str:
    if not asset_path:
        return "missing"
    normalized = asset_path.replace("\\", "/").lower()
    if _is_lightweight_smoke_asset(normalized):
        return "lightweight_smoke"
    if "synthetic_" in normalized:
        return "deterministic_synthetic"
    if "benchmark_assets_generated/" in normalized:
        return "generated_dummy"
    return "custom_asset"


def _is_lightweight_smoke_asset(asset_path: str) -> bool:
    normalized = asset_path.replace("\\", "/").lower()
    return normalized.endswith("/test_splats.ply") or normalized.endswith("test_splats.ply")
