"""Remove generated Dumper-7 SDK files not consumed by an MSVC build."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys


TLOG_PATTERN = "CL.read.*.tlog"
REQUIRED_DEPENDENCY_NAMES = {"basic.cpp", "basic.hpp"}


class PruneError(RuntimeError):
    """Raised when pruning cannot be proven safe."""


def normalized_path(path: Path) -> str:
    return os.path.normcase(os.path.abspath(path))


def is_within(path: Path, directory: Path) -> bool:
    try:
        return os.path.commonpath(
            [normalized_path(path), normalized_path(directory)]
        ) == normalized_path(directory)
    except ValueError:
        return False


def find_dependency_logs(dependency_path: Path) -> list[Path]:
    if dependency_path.is_file():
        return [dependency_path]
    if dependency_path.is_dir():
        return sorted(dependency_path.rglob(TLOG_PATTERN))
    return []


def read_tlog_lines(tlog_path: Path) -> list[str]:
    data = tlog_path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16").splitlines()

    for encoding in ("utf-8-sig", "utf-16-le"):
        try:
            return data.decode(encoding).splitlines()
        except UnicodeDecodeError:
            continue
    raise PruneError(f"Unable to decode dependency log: {tlog_path}")


def collect_sdk_dependencies(sdk_dir: Path, tlog_paths: list[Path]) -> set[str]:
    dependencies: set[str] = set()
    dependency_names: set[str] = set()

    for tlog_path in tlog_paths:
        for line in read_tlog_lines(tlog_path):
            raw_path = line.strip().lstrip("^")
            if not raw_path:
                continue

            candidate = Path(raw_path)
            if not candidate.is_absolute() or not is_within(candidate, sdk_dir):
                continue

            dependencies.add(normalized_path(candidate))
            dependency_names.add(candidate.name.casefold())

    missing_required = REQUIRED_DEPENDENCY_NAMES - dependency_names
    if missing_required:
        missing = ", ".join(sorted(missing_required))
        raise PruneError(
            "Dependency logs are incomplete for this CppSDK; "
            f"required entries are missing: {missing}"
        )

    return dependencies


def validate_sdk_directory(sdk_dir: Path) -> None:
    if not sdk_dir.is_dir():
        raise PruneError(f"CppSDK directory does not exist: {sdk_dir}")
    if not (sdk_dir / "SDK" / "Basic.hpp").is_file():
        raise PruneError(
            "Refusing to prune a directory that does not look like a "
            f"Dumper-7 CppSDK: {sdk_dir}"
        )


def prune_sdk(sdk_dir: Path, dependencies: set[str], dry_run: bool) -> tuple[int, int, int]:
    sdk_files = sorted(path for path in sdk_dir.rglob("*") if path.is_file())
    unused_files = [
        path for path in sdk_files if normalized_path(path) not in dependencies
    ]

    if not dry_run:
        for path in unused_files:
            if not is_within(path, sdk_dir):
                raise PruneError(f"Refusing to delete outside CppSDK: {path}")
            path.unlink()

    removed_directories = 0
    if not dry_run:
        directories = sorted(
            (path for path in sdk_dir.rglob("*") if path.is_dir()),
            key=lambda path: len(path.parts),
            reverse=True,
        )
        for directory in directories:
            try:
                directory.rmdir()
                removed_directories += 1
            except OSError:
                pass

    return len(sdk_files) - len(unused_files), len(unused_files), removed_directories


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prune a Dumper-7 CppSDK using the exact file dependencies "
            "recorded by MSVC."
        )
    )
    parser.add_argument("sdk_dir", type=Path, help="Trainer CppSDK directory.")
    parser.add_argument(
        "dependency_path",
        type=Path,
        help=(
            "An MSVC CL.read.*.tlog file, or a build directory containing "
            "one or more such files."
        ),
    )
    parser.add_argument(
        "--configuration",
        help="When supplied, pruning runs only when this is Release.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report what would be removed without changing the SDK.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.configuration and args.configuration.casefold() != "release":
        print(
            "CppSDK pruning skipped for "
            f"{args.configuration}; it runs only for Release builds."
        )
        return 0

    try:
        sdk_dir = args.sdk_dir.resolve(strict=False)
        dependency_path = args.dependency_path.resolve(strict=False)
        validate_sdk_directory(sdk_dir)

        tlog_paths = find_dependency_logs(dependency_path)
        if not tlog_paths:
            raise PruneError(
                f"No {TLOG_PATTERN} dependency logs found at: {dependency_path}"
            )

        dependencies = collect_sdk_dependencies(sdk_dir, tlog_paths)
        kept, removed, removed_directories = prune_sdk(
            sdk_dir,
            dependencies,
            args.dry_run,
        )
    except (OSError, PruneError) as error:
        print(f"CppSDK pruning failed: {error}", file=sys.stderr)
        return 1

    action = "Would keep" if args.dry_run else "Kept"
    removal = "would remove" if args.dry_run else "removed"
    print(
        f"{action} {kept} compiler-used CppSDK files; "
        f"{removal} {removed} unused files"
        + (
            "."
            if args.dry_run
            else f" and {removed_directories} empty directories."
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
