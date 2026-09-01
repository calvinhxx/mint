#!/usr/bin/env python3

import re
import sys
from dataclasses import dataclass
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
HASH_COMMENT_SUFFIXES = {".cmake", ".py", ".sh", ".yaml", ".yml"}
ENGLISH_MARKER = re.compile(r"(?:^|\s)EN:\s*\S", re.MULTILINE)
CHINESE_MARKER = re.compile(r"(?:^|\s)ZH-CN:\s*\S", re.MULTILINE)
NAMESPACE_COMMENT = re.compile(r"namespace(?:\s+[A-Za-z_][A-Za-z0-9_:]*)?")
MACHINE_DIRECTIVE = re.compile(
    r"(?:NOLINT|NOSONAR|IWYU\s+pragma|clang-format|shellcheck|yamllint|noqa|"
    r"type:\s*ignore|pragma:|coverage:|SPDX-License-Identifier:|[-*]-\s*coding:)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Comment:
    line: int
    text: str
    standalone: bool


def is_bilingual(text: str) -> bool:
    return ENGLISH_MARKER.search(text) is not None and CHINESE_MARKER.search(text) is not None


def is_exempt(text: str) -> bool:
    value = text.strip().lstrip("*").strip()
    return not value or NAMESPACE_COMMENT.fullmatch(value) is not None or MACHINE_DIRECTIVE.match(value)


def cpp_comments(source: str) -> list[Comment]:
    comments = []
    index = 0
    line = 1
    size = len(source)
    while index < size:
        if source.startswith("//", index):
            start = index
            end = source.find("\n", index + 2)
            if end == -1:
                end = size
            line_start = source.rfind("\n", 0, start) + 1
            comments.append(Comment(line, source[index + 2 : end], not source[line_start:start].strip()))
            index = end
            continue
        if source.startswith("/*", index):
            start_line = line
            end = source.find("*/", index + 2)
            if end == -1:
                end = size - 2
            text = source[index + 2 : end]
            line_start = source.rfind("\n", 0, index) + 1
            comments.append(Comment(start_line, text, not source[line_start:index].strip()))
            consumed = source[index : end + 2]
            line += consumed.count("\n")
            index = end + 2
            continue
        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2, min(size, index + 20))
            if delimiter_end != -1:
                delimiter = source[index + 2 : delimiter_end]
                if not re.search(r"[\s\\()]", delimiter):
                    terminator = ")" + delimiter + '"'
                    raw_end = source.find(terminator, delimiter_end + 1)
                    if raw_end != -1:
                        consumed = source[index : raw_end + len(terminator)]
                        line += consumed.count("\n")
                        index = raw_end + len(terminator)
                        continue
        if source[index] in {'"', "'"}:
            quote = source[index]
            index += 1
            while index < size:
                if source[index] == "\\" and index + 1 < size:
                    if source[index + 1] == "\n":
                        line += 1
                    index += 2
                    continue
                if source[index] == quote:
                    index += 1
                    break
                if source[index] == "\n":
                    line += 1
                index += 1
            continue
        if source[index] == "\n":
            line += 1
        index += 1
    return comments


def grouped_line_comments(comments: list[Comment]):
    group = []
    for comment in comments:
        if not comment.standalone:
            if group:
                yield group
                group = []
            yield [comment]
            continue
        if group and comment.line != group[-1].line + 1:
            yield group
            group = []
        group.append(comment)
    if group:
        yield group


def check_cpp(path: Path) -> list[int]:
    comments = cpp_comments(path.read_text(encoding="utf-8"))
    failures = []
    for group in grouped_line_comments(comments):
        text = "\n".join(comment.text for comment in group)
        if not is_exempt(text) and not is_bilingual(text):
            failures.append(group[0].line)
    return failures


def check_hash_comments(path: Path) -> list[int]:
    groups = []
    current = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = re.match(r"^\s*#(.*)$", line)
        if match and not match.group(1).startswith("!"):
            if current and line_number != current[-1][0] + 1:
                groups.append(current)
                current = []
            current.append((line_number, match.group(1)))
        elif current:
            groups.append(current)
            current = []
    if current:
        groups.append(current)

    failures = []
    for group in groups:
        text = "\n".join(value for _, value in group)
        if not is_exempt(text) and not is_bilingual(text):
            failures.append(group[0][0])
    return failures


def source_files(root: Path):
    for directory in (root / "src", root / "include", root / "tests"):
        for path in directory.rglob("*"):
            if path.suffix in CPP_SUFFIXES:
                yield path, check_cpp
    for directory in (root / "scripts", root / "tests", root / ".github", root / "cmake"):
        for path in directory.rglob("*"):
            if path.suffix in HASH_COMMENT_SUFFIXES or path.name == "CMakeLists.txt":
                yield path, check_hash_comments
    yield root / "CMakeLists.txt", check_hash_comments


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parents[2]).resolve()
    failures = []
    seen = set()
    for path, checker in source_files(root):
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        failures.extend(f"{path.relative_to(root)}:{line}" for line in checker(path))
    if failures:
        raise AssertionError(
            "Natural-language code comments must contain EN: and ZH-CN: in the same block: "
            + ", ".join(failures)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
