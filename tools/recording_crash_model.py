from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum


class CrashModelError(ValueError):
    pass


class Operation(StrEnum):
    CREATE_JOURNAL = "create_journal"
    CREATE_MEDIA_TEMPORARY = "create_media_temporary"
    CREATE_SIDECAR_TEMPORARY = "create_sidecar_temporary"
    CREATE_MARKER_TEMPORARY = "create_marker_temporary"
    WRITE_JOURNAL_HEADER = "write_journal_header"
    SYNC_JOURNAL_HEADER = "sync_journal_header"
    SYNC_MEDIA_TEMPORARY = "sync_media_temporary"
    SYNC_SIDECAR_TEMPORARY = "sync_sidecar_temporary"
    SYNC_MARKER_TEMPORARY = "sync_marker_temporary"
    SYNC_PREPARED_DIRECTORY = "sync_prepared_directory"
    ADMIT_MEDIA = "admit_media"
    WRITE_MEDIA_ACCESS_UNIT = "write_media_access_unit"
    SYNC_MEDIA_GROUP = "sync_media_group"
    WRITE_JOURNAL_GROUP = "write_journal_group"
    SYNC_JOURNAL_GROUP = "sync_journal_group"
    WRITE_SIDECAR_GROUP = "write_sidecar_group"
    SYNC_SIDECAR_GROUP = "sync_sidecar_group"
    WRITE_SIDECAR_SUFFIX = "write_sidecar_suffix"
    SYNC_SIDECAR = "sync_sidecar"
    RENAME_MEDIA = "rename_media"
    RENAME_SIDECAR = "rename_sidecar"
    SYNC_PUBLICATION_DIRECTORY = "sync_publication_directory"
    WRITE_MARKER = "write_marker"
    SYNC_MARKER = "sync_marker"
    RENAME_MARKER = "rename_marker"
    SYNC_MARKER_DIRECTORY = "sync_marker_directory"
    REMOVE_JOURNAL = "remove_journal"
    SYNC_CLEANUP_DIRECTORY = "sync_cleanup_directory"


CANONICAL_OPERATIONS = (
    Operation.CREATE_JOURNAL,
    Operation.CREATE_MEDIA_TEMPORARY,
    Operation.CREATE_SIDECAR_TEMPORARY,
    Operation.CREATE_MARKER_TEMPORARY,
    Operation.WRITE_JOURNAL_HEADER,
    Operation.SYNC_JOURNAL_HEADER,
    Operation.SYNC_MEDIA_TEMPORARY,
    Operation.SYNC_SIDECAR_TEMPORARY,
    Operation.SYNC_MARKER_TEMPORARY,
    Operation.SYNC_PREPARED_DIRECTORY,
    Operation.ADMIT_MEDIA,
    Operation.WRITE_MEDIA_ACCESS_UNIT,
    Operation.SYNC_MEDIA_GROUP,
    Operation.WRITE_JOURNAL_GROUP,
    Operation.SYNC_JOURNAL_GROUP,
    Operation.WRITE_SIDECAR_GROUP,
    Operation.SYNC_SIDECAR_GROUP,
    Operation.WRITE_SIDECAR_SUFFIX,
    Operation.SYNC_SIDECAR,
    Operation.RENAME_MEDIA,
    Operation.RENAME_SIDECAR,
    Operation.SYNC_PUBLICATION_DIRECTORY,
    Operation.WRITE_MARKER,
    Operation.SYNC_MARKER,
    Operation.RENAME_MARKER,
    Operation.SYNC_MARKER_DIRECTORY,
    Operation.REMOVE_JOURNAL,
    Operation.SYNC_CLEANUP_DIRECTORY,
)


@dataclass
class FileState:
    live_version: int = 0
    durable_version: int = 0


@dataclass
class CrashState:
    live_names: dict[str, int] = field(default_factory=dict)
    durable_names: dict[str, int] = field(default_factory=dict)
    files: dict[int, FileState] = field(default_factory=dict)
    next_inode: int = 1
    admitted: bool = False
    pending_access_units: int = 0
    durable_media_access_units: int = 0
    live_journal_access_units: int = 0
    durable_journal_access_units: int = 0

    def create(self, name: str) -> None:
        if name in self.live_names:
            raise CrashModelError("no_clobber_name_reused")
        self.live_names[name] = self.next_inode
        self.files[self.next_inode] = FileState()
        self.next_inode += 1

    def write(self, name: str) -> None:
        if name not in self.live_names:
            raise CrashModelError(f"write_missing_{name}")
        self.files[self.live_names[name]].live_version += 1

    def sync_file(self, name: str) -> None:
        if name not in self.live_names:
            raise CrashModelError(f"sync_missing_{name}")
        file_state = self.files[self.live_names[name]]
        file_state.durable_version = file_state.live_version

    def sync_directory(self) -> None:
        self.durable_names = dict(self.live_names)

    def rename(self, source: str, destination: str) -> None:
        if source not in self.live_names or destination in self.live_names:
            raise CrashModelError("rename_noreplace_contract_broken")
        self.live_names[destination] = self.live_names.pop(source)

    def remove(self, name: str) -> None:
        if name not in self.live_names:
            raise CrashModelError(f"remove_missing_{name}")
        self.live_names.pop(name)

    def durable_version(self, name: str) -> int:
        return self.files[self.durable_names[name]].durable_version


@dataclass(frozen=True)
class CrashCut:
    operation: Operation
    outcome: str
    admitted: bool
    durable_media_access_units: int
    durable_journal_access_units: int


def _crash_outcome(state: CrashState) -> str:
    if "marker.final" in state.durable_names:
        if "media.final" not in state.durable_names or "sidecar.final" not in state.durable_names:
            raise CrashModelError("completion_marker_precedes_final_companions")
        if state.durable_version("marker.final") == 0:
            raise CrashModelError("completion_marker_not_durable")
        if state.durable_version("media.final") == 0:
            raise CrashModelError("complete_media_not_durable")
        if state.durable_version("sidecar.final") == 0:
            raise CrashModelError("complete_sidecar_not_durable")
        return "COMPLETE"
    if "journal" in state.durable_names:
        if state.durable_version("journal") == 0:
            return "PREPARED_HEADER_INCOMPLETE"
        if state.durable_journal_access_units > state.durable_media_access_units:
            raise CrashModelError("journal_commit_precedes_media")
        if state.durable_journal_access_units == 0:
            return "PREPARED_INCOMPLETE"
        return "INCOMPLETE"
    if state.admitted:
        raise CrashModelError("media_admitted_without_durable_journal_anchor")
    return "ABSENT"


def verify_crash_protocol(
    operations: tuple[Operation, ...] = CANONICAL_OPERATIONS,
) -> tuple[CrashCut, ...]:
    state = CrashState()
    cuts: list[CrashCut] = []
    for operation in operations:
        if operation == Operation.CREATE_JOURNAL:
            state.create("journal")
        elif operation == Operation.CREATE_MEDIA_TEMPORARY:
            state.create("media.temporary")
        elif operation == Operation.CREATE_SIDECAR_TEMPORARY:
            state.create("sidecar.temporary")
        elif operation == Operation.CREATE_MARKER_TEMPORARY:
            state.create("marker.temporary")
        elif operation == Operation.WRITE_JOURNAL_HEADER:
            state.write("journal")
        elif operation == Operation.SYNC_JOURNAL_HEADER:
            state.sync_file("journal")
        elif operation == Operation.SYNC_MEDIA_TEMPORARY:
            state.sync_file("media.temporary")
        elif operation == Operation.SYNC_SIDECAR_TEMPORARY:
            state.sync_file("sidecar.temporary")
        elif operation == Operation.SYNC_MARKER_TEMPORARY:
            state.sync_file("marker.temporary")
        elif operation == Operation.SYNC_PREPARED_DIRECTORY:
            required = {"journal", "media.temporary", "sidecar.temporary", "marker.temporary"}
            if not required.issubset(state.live_names):
                raise CrashModelError("prepared_directory_missing_companion")
            state.sync_directory()
        elif operation == Operation.ADMIT_MEDIA:
            required = {"journal", "media.temporary", "sidecar.temporary", "marker.temporary"}
            if not required.issubset(state.durable_names) or state.durable_version("journal") == 0:
                raise CrashModelError("media_admitted_before_prepared_barrier")
            state.admitted = True
        elif operation == Operation.WRITE_MEDIA_ACCESS_UNIT:
            if not state.admitted:
                raise CrashModelError("media_write_before_admission")
            state.write("media.temporary")
            state.pending_access_units += 1
        elif operation == Operation.SYNC_MEDIA_GROUP:
            state.sync_file("media.temporary")
            state.durable_media_access_units = state.pending_access_units
        elif operation == Operation.WRITE_JOURNAL_GROUP:
            state.write("journal")
            state.live_journal_access_units = state.pending_access_units
        elif operation == Operation.SYNC_JOURNAL_GROUP:
            if state.live_journal_access_units > state.durable_media_access_units:
                raise CrashModelError("journal_commit_precedes_media")
            state.sync_file("journal")
            state.durable_journal_access_units = state.live_journal_access_units
        elif operation == Operation.WRITE_SIDECAR_GROUP:
            state.write("sidecar.temporary")
        elif operation == Operation.SYNC_SIDECAR_GROUP:
            state.sync_file("sidecar.temporary")
        elif operation == Operation.WRITE_SIDECAR_SUFFIX:
            state.write("sidecar.temporary")
        elif operation == Operation.SYNC_SIDECAR:
            state.sync_file("sidecar.temporary")
        elif operation == Operation.RENAME_MEDIA:
            state.rename("media.temporary", "media.final")
        elif operation == Operation.RENAME_SIDECAR:
            state.rename("sidecar.temporary", "sidecar.final")
        elif operation == Operation.SYNC_PUBLICATION_DIRECTORY:
            state.sync_directory()
        elif operation == Operation.WRITE_MARKER:
            state.write("marker.temporary")
        elif operation == Operation.SYNC_MARKER:
            state.sync_file("marker.temporary")
        elif operation == Operation.RENAME_MARKER:
            if (
                "media.final" not in state.durable_names
                or "sidecar.final" not in state.durable_names
            ):
                raise CrashModelError("marker_rename_precedes_publication_barrier")
            state.rename("marker.temporary", "marker.final")
        elif operation == Operation.SYNC_MARKER_DIRECTORY:
            state.sync_directory()
        elif operation == Operation.REMOVE_JOURNAL:
            if "marker.final" not in state.durable_names:
                raise CrashModelError("journal_removed_before_marker_barrier")
            state.remove("journal")
        elif operation == Operation.SYNC_CLEANUP_DIRECTORY:
            state.sync_directory()
        else:
            raise CrashModelError(f"unknown_operation_{operation}")
        cuts.append(
            CrashCut(
                operation=operation,
                outcome=_crash_outcome(state),
                admitted=state.admitted,
                durable_media_access_units=state.durable_media_access_units,
                durable_journal_access_units=state.durable_journal_access_units,
            )
        )
    return tuple(cuts)


def main() -> int:
    cuts = verify_crash_protocol()
    print(f"recording crash model passed {len(cuts)} persistence cuts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
