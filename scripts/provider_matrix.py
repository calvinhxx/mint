"""Load and fingerprint the committed provider profile matrix."""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


PROFILE_ID = re.compile(r"^[a-z0-9][a-z0-9._-]*$")


class ProviderMatrixError(ValueError):
    pass


@dataclass(frozen=True)
class ProviderProfile:
    id: str
    config: Path


def load_provider_matrix(path: Path) -> list[ProviderProfile]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ProviderMatrixError(f"cannot read provider manifest: {path}") from error

    if not isinstance(document, dict) or document.get("schema_version") != 1 or not isinstance(
        document.get("profiles"), list
    ):
        raise ProviderMatrixError("provider manifest must use schema_version 1 and contain profiles")

    root = path.resolve().parent
    profiles: list[ProviderProfile] = []
    ids: set[str] = set()
    paths: set[Path] = set()
    for item in document["profiles"]:
        if not isinstance(item, dict) or set(item) != {"id", "config"}:
            raise ProviderMatrixError("each provider profile must contain only id and config")
        profile_id = item["id"]
        config_name = item["config"]
        if not isinstance(profile_id, str) or PROFILE_ID.fullmatch(profile_id) is None:
            raise ProviderMatrixError(f"invalid provider profile id: {profile_id!r}")
        if not isinstance(config_name, str) or not config_name:
            raise ProviderMatrixError(f"invalid config path for provider profile: {profile_id}")

        config = (root / config_name).resolve()
        try:
            config.relative_to(root)
        except ValueError as error:
            raise ProviderMatrixError(
                f"provider config escapes manifest directory: {config_name}"
            ) from error
        if not config.is_file():
            raise ProviderMatrixError(f"provider config does not exist: {config_name}")
        if profile_id in ids or config in paths:
            raise ProviderMatrixError(f"duplicate provider profile: {profile_id}")
        ids.add(profile_id)
        paths.add(config)
        profiles.append(ProviderProfile(profile_id, config))

    if not profiles:
        raise ProviderMatrixError("provider manifest has no profiles")
    return profiles


def provider_matrix_digest(manifest: Path, profiles: Sequence[ProviderProfile]) -> str:
    digest = hashlib.sha256()
    for path in (manifest, *(profile.config for profile in profiles)):
        try:
            content = path.read_bytes()
        except OSError as error:
            raise ProviderMatrixError(f"cannot hash provider matrix file: {path}") from error
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)
    return digest.hexdigest()


def profile_by_id(profiles: Sequence[ProviderProfile], profile_id: str) -> ProviderProfile:
    for profile in profiles:
        if profile.id == profile_id:
            return profile
    raise ProviderMatrixError(f"unknown provider profile: {profile_id}")


def profile_by_config(profiles: Sequence[ProviderProfile], config: Path) -> ProviderProfile:
    resolved = config.resolve()
    for profile in profiles:
        if profile.config == resolved:
            return profile
    raise ProviderMatrixError("provider config is not part of the committed matrix")
