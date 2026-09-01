#!/usr/bin/env python3

import json
import re
import sys
from pathlib import Path


PLACEHOLDER = re.compile(r"\{([a-z][a-z0-9_]*)\}")
TYPED_MESSAGE = re.compile(r"\bMessage::([a-z][a-z0-9_]*)")
TYPED_PLACEHOLDER = re.compile(r"\bPlaceholder::([a-z][a-z0-9_]*)")
RAW_MESSAGE_CALL = re.compile(r'\b(?:message|localized|text)\s*\(\s*"')
RAW_ARGUMENT_CALL = re.compile(r'\barg\s*\(\s*"')
HAN = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
HUMAN_TEXT_LITERAL = re.compile(
    r'(?:std::(?:runtime_error|invalid_argument|logic_error)|error_result|'
    r'console\.(?:write|write_line|write_error|write_block))\s*\(\s*"([^"]*)"',
    re.MULTILINE,
)


def load_catalog(path: Path) -> tuple[str, dict[str, str]]:
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate key: {key}")
            result[key] = value
        return result

    try:
        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        raise AssertionError(f"{path}: {error}") from error

    if set(document) != {"locale", "messages"}:
        raise AssertionError(f"{path}: expected only locale and messages")
    if not isinstance(document["locale"], str) or not isinstance(document["messages"], dict):
        raise AssertionError(f"{path}: invalid catalog shape")
    for key, value in document["messages"].items():
        if not isinstance(key, str) or not key or not isinstance(value, str) or not value:
            raise AssertionError(f"{path}: messages must contain non-empty string pairs")
        placeholders = PLACEHOLDER.findall(value)
        if value.count("{") != len(placeholders) or value.count("}") != len(placeholders):
            raise AssertionError(f"{path}: malformed placeholder in {key}")
    return document["locale"], document["messages"]


def source_files(root: Path):
    for directory in (root / "src", root / "include"):
        for path in directory.rglob("*"):
            if path.suffix in {".cpp", ".h", ".hpp"}:
                yield path


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parents[2]).resolve()
    en_locale, english = load_catalog(root / "locales" / "en.json")
    zh_locale, chinese = load_catalog(root / "locales" / "zh-CN.json")
    if en_locale != "en" or zh_locale != "zh-CN":
        raise AssertionError("catalog locale metadata must be en and zh-CN")

    if english.keys() != chinese.keys():
        missing_english = sorted(chinese.keys() - english.keys())
        missing_chinese = sorted(english.keys() - chinese.keys())
        raise AssertionError(
            f"catalog key mismatch; missing en={missing_english}, missing zh-CN={missing_chinese}"
        )

    for key in english:
        en_fields = set(PLACEHOLDER.findall(english[key]))
        zh_fields = set(PLACEHOLDER.findall(chinese[key]))
        if en_fields != zh_fields:
            raise AssertionError(
                f"placeholder mismatch for {key}: en={sorted(en_fields)}, zh-CN={sorted(zh_fields)}"
            )

    message_identifiers = {}
    for key in english:
        identifier = re.sub(r"[^a-zA-Z0-9_]", "_", key)
        if identifier in message_identifiers:
            raise AssertionError(
                f"catalog keys map to the same C++ identifier: "
                f"{message_identifiers[identifier]} and {key}"
            )
        message_identifiers[identifier] = key
    placeholder_identifiers = {
        field for value in english.values() for field in PLACEHOLDER.findall(value)
    }

    referenced_messages = set()
    referenced_placeholders = set()
    hardcoded_han = []
    hardcoded_human_text = []
    raw_localization_ids = []
    for path in source_files(root):
        content = path.read_text(encoding="utf-8")
        referenced_messages.update(TYPED_MESSAGE.findall(content))
        referenced_placeholders.update(TYPED_PLACEHOLDER.findall(content))
        for line_number, line in enumerate(content.splitlines(), 1):
            stripped = line.lstrip()
            is_comment = stripped.startswith(("//", "/*", "*", "*/"))
            if not is_comment and HAN.search(line):
                hardcoded_han.append(f"{path.relative_to(root)}:{line_number}")
        for match in HUMAN_TEXT_LITERAL.finditer(content):
            if path == root / "src" / "cli" / "main.cpp" and match.group(1) == "mint ":
                continue
            line_number = content.count("\n", 0, match.start()) + 1
            hardcoded_human_text.append(f"{path.relative_to(root)}:{line_number}")
        for pattern in (RAW_MESSAGE_CALL, RAW_ARGUMENT_CALL):
            for match in pattern.finditer(content):
                line_number = content.count("\n", 0, match.start()) + 1
                raw_localization_ids.append(f"{path.relative_to(root)}:{line_number}")

    missing_messages = sorted(referenced_messages - message_identifiers.keys())
    if missing_messages:
        raise AssertionError(f"source references missing typed messages: {missing_messages}")
    missing_placeholders = sorted(referenced_placeholders - placeholder_identifiers)
    if missing_placeholders:
        raise AssertionError(f"source references missing typed placeholders: {missing_placeholders}")
    if hardcoded_han:
        raise AssertionError(
            "Chinese production literals must live in locales/zh-CN.json: "
            + ", ".join(hardcoded_han)
        )
    if hardcoded_human_text:
        raise AssertionError(
            "Human-facing production text must use the localization catalog: "
            + ", ".join(hardcoded_human_text)
        )
    if raw_localization_ids:
        raise AssertionError(
            "Production localization calls must use generated Message and Placeholder IDs: "
            + ", ".join(raw_localization_ids)
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
