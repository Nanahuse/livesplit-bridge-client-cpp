#!/usr/bin/env python3
"""Deterministic generator for the LiveSplit.Bridge JSON v4 wire models.

The official schemas are pinned by tools/protocol-source.json and
schemas/v4/SOURCE.json. The generated header is committed; normal builds do
not require Python. Run ``python tools/generate_models.py --check`` in CI.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCHEMA_DIR = ROOT / "schemas" / "v4"
OUTPUT_PATH = ROOT / "include" / "livesplit" / "bridge" / "protocol.hpp"
GENERATOR = "tools/generate_models.py"


def fail(message: str) -> None:
    print(f"generate_models: {message}", file=sys.stderr)
    raise SystemExit(1)


def stable(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def read_json(file: Path) -> Any:
    try:
        return json.loads(file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {file.relative_to(ROOT)}: {error}")


def sha256(file: Path) -> str:
    # Git may check out text files with CRLF on Windows. Hash the canonical LF
    # form so the pinned official schemas remain verifiable.
    text = file.read_text(encoding="utf-8").replace("\r\n", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


protocol_source = read_json(ROOT / "tools" / "protocol-source.json")
source = read_json(SCHEMA_DIR / "SOURCE.json")
if protocol_source["revision"] != source["revision"]:
    fail("tools/protocol-source.json and schemas/v4/SOURCE.json revisions differ")

schema_names = [
    "rpc-request.schema.json",
    "rpc-response.schema.json",
    "bridge-event.schema.json",
]
documents: dict[str, Any] = {}
for name in schema_names:
    file = SCHEMA_DIR / name
    expected = source["files"].get(name)
    if not expected:
        fail(f"schemas/v4/SOURCE.json has no hash for {name}")
    actual = sha256(file)
    if actual != expected:
        fail(f"{name} differs from the pinned official schema (expected {expected}, got {actual})")
    documents[name] = read_json(file)

# Merge the official definitions and require exact agreement for shared names.
defs: dict[str, Any] = {}
for name in schema_names:
    for definition_name, definition in documents[name].get("$defs", {}).items():
        if definition_name in defs:
            if stable(defs[definition_name]) != stable(definition):
                fail(f"definition {definition_name} differs between official schemas")
        else:
            defs[definition_name] = definition

roots = {
    "rpc_request": documents["rpc-request.schema.json"],
    "rpc_response": documents["rpc-response.schema.json"],
    "bridge_event": documents["bridge-event.schema.json"],
}


def definition_name_from_ref(ref: Any) -> str:
    prefix = "#/$defs/"
    if not isinstance(ref, str) or not ref.startswith(prefix):
        fail(f"unsupported $ref: {json.dumps(ref)}")
    name = ref[len(prefix):]
    if name not in defs:
        fail(f"unresolved $ref: {name}")
    return name


def snake(name: str) -> str:
    value = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    value = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", value)
    value = re.sub(r"[^A-Za-z0-9]+", "_", value)
    return value.strip("_").lower()


def enum_value_name(value: str) -> str:
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    name = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
    return f"_{name}" if name[:1].isdigit() else name


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


definition_cpp_names = {"CompletedCount": "completed_count_value"}


def cpp_definition_name(name: str) -> str:
    return definition_cpp_names.get(name, snake(name))


def is_alias_definition(name: str) -> bool:
    definition = defs[name]
    return definition.get("type") == "array" or (
        definition.get("type") == "object"
        and "properties" not in definition
        and definition.get("additionalProperties")
    )


def classify_definition(name: str) -> str:
    definition = defs[name]
    if "enum" in definition:
        return "enum"
    if definition.get("type") == "object" and definition.get("properties"):
        return "object"
    if is_alias_definition(name):
        return "alias"
    fail(f"unsupported definition {name}")


def type_of(schema: Any, context: str) -> dict[str, Any]:
    if not isinstance(schema, dict):
        fail(f"missing schema at {context}")

    if "$ref" in schema:
        name = definition_name_from_ref(schema["$ref"])
        kind = classify_definition(name)
        if kind == "alias":
            return type_of(defs[name], f"{context} -> {name}")
        if kind == "enum":
            return {"kind": "enum", "name": cpp_definition_name(name), "definition": name}
        return {"kind": "object", "name": cpp_definition_name(name), "definition": name}

    if "oneOf" in schema:
        fail(f"inline oneOf is not supported at {context}")

    if "anyOf" in schema:
        entries = schema["anyOf"]
        non_null = [entry for entry in entries if entry.get("type") != "null"]
        has_null = any(entry.get("type") == "null" for entry in entries)
        if len(non_null) == 1 and (has_null or len(entries) == 1):
            inner = type_of(non_null[0], f"{context}.anyOf")
            return {"kind": "optional", "item": inner} if has_null else inner
        fail(f"unsupported anyOf at {context}")

    if "const" in schema:
        if schema["const"] is None:
            return {"kind": "null"}
        if isinstance(schema["const"], str):
            return {"kind": "const_string", "value": schema["const"]}
        fail(f"unsupported const at {context}")

    if "enum" in schema:
        fail(f"inline enum is not supported at {context}")

    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        non_null = [entry for entry in schema_type if entry != "null"]
        if len(non_null) != 1:
            fail(f"unsupported type array at {context}")
        if "null" in schema_type:
            inner_schema = dict(schema)
            inner_schema["type"] = non_null[0]
            return {"kind": "optional", "item": type_of(inner_schema, context)}
        schema_type = non_null[0]

    if schema_type == "string":
        return {"kind": "string"}
    if schema_type == "boolean":
        return {"kind": "bool"}
    if schema_type == "integer":
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        min_value = None if minimum is None else int(minimum)
        max_value = None if maximum is None else int(maximum)
        if (min_value is None or min_value >= 0) and max_value is not None and max_value <= 4294967295:
            return {"kind": "uint32", "min": min_value if min_value is not None else 0, "max": max_value}
        if (
            (min_value is None or min_value >= -2147483648)
            and max_value is not None
            and max_value <= 2147483647
        ):
            return {
                "kind": "int32",
                "min": min_value if min_value is not None else -2147483648,
                "max": max_value,
            }
        if min_value is not None and max_value is not None and (
            min_value < -2147483648 or max_value > 2147483647
        ):
            if min_value >= -9223372036854775808 and max_value <= 9223372036854775807:
                return {"kind": "int64", "min": min_value, "max": max_value}
        fail(f"unsupported integer bounds at {context}")
    if schema_type == "array":
        return {"kind": "vector", "item": type_of(schema.get("items"), f"{context}[]")}
    if schema_type == "object":
        additional = schema.get("additionalProperties")
        if isinstance(additional, dict):
            return {"kind": "map", "value": type_of(additional, f"{context}.*")}
        fail(f"inline object is not supported at {context}")

    fail(f"unsupported schema type at {context}: {json.dumps(schema.get('type'))}")


def cpp_type(desc: dict[str, Any]) -> str:
    kind = desc["kind"]
    if kind == "string":
        return "std::string"
    if kind == "bool":
        return "bool"
    if kind == "uint32":
        return "std::uint32_t"
    if kind == "int32":
        return "std::int32_t"
    if kind == "int64":
        return "std::int64_t"
    if kind in ("enum", "object"):
        return desc["name"]
    if kind == "vector":
        return f"std::vector<{cpp_type(desc['item'])}>"
    if kind == "map":
        return f"std::map<std::string, {cpp_type(desc['value'])}>"
    if kind == "optional":
        return f"std::optional<{cpp_type(desc['item'])}>"
    if kind == "null":
        return "std::nullptr_t"
    if kind == "const_string":
        return "std::string"
    fail(f"cannot render C++ type for {kind}")


enums = [
    {"name": name, "cppName": cpp_definition_name(name), "definition": definition}
    for name, definition in defs.items()
    if "enum" in definition
]

object_names = [
    name
    for name, definition in defs.items()
    if definition.get("type") == "object" and definition.get("properties")
]


def refs_in_schema(schema: Any, output: set[str], expanded_aliases: set[str] | None = None) -> None:
    if expanded_aliases is None:
        expanded_aliases = set()
    if not isinstance(schema, dict):
        return
    ref = schema.get("$ref")
    if ref:
        name = definition_name_from_ref(ref)
        output.add(name)
        if classify_definition(name) == "alias" and name not in expanded_aliases:
            expanded_aliases.add(name)
            refs_in_schema(defs[name], output, expanded_aliases)
    for value in schema.values():
        if isinstance(value, list):
            for entry in value:
                refs_in_schema(entry, output, expanded_aliases)
        elif isinstance(value, dict):
            refs_in_schema(value, output, expanded_aliases)


object_order: list[str] = []
visiting: set[str] = set()
visited: set[str] = set()


def visit_object(name: str) -> None:
    if name in visited:
        return
    if name in visiting:
        fail(f"cyclic object definitions involving {name}")
    visiting.add(name)
    refs: set[str] = set()
    refs_in_schema(defs[name], refs)
    for ref in sorted(refs):
        if classify_definition(ref) == "object":
            visit_object(ref)
    visiting.remove(name)
    visited.add(name)
    object_order.append(name)


for name in sorted(object_names):
    visit_object(name)


def conditional_array_property(definition: dict[str, Any], context: str) -> str | None:
    if "if" not in definition and "then" not in definition:
        return None
    if "if" not in definition or "then" not in definition:
        fail(f"unsupported if/then at {context}")
    if_properties = list(definition["if"].get("properties", {}).items())
    required = set(definition["if"].get("required", []))
    if len(if_properties) != 1:
        fail(f"unsupported if condition at {context}")
    prop, condition = if_properties[0]
    if condition.get("const") is not None or prop not in required:
        fail(f"unsupported if condition at {context}")
    then_property = definition["then"].get("properties", {}).get(prop)
    if not then_property:
        fail(f"unsupported then clause at {context}")
    then_type = type_of(then_property, f"{context}.then.{prop}")
    if cpp_type(then_type) != "std::vector<std::string>":
        fail(f"unexpected then type for {context}.{prop}")
    return prop


def properties_of(name: str) -> list[dict[str, Any]]:
    definition = defs[name]
    required = set(definition.get("required", []))
    conditional = conditional_array_property(definition, name)
    fields: list[dict[str, Any]] = []
    for wire_name, schema in definition["properties"].items():
        desc = type_of(schema, f"{name}.{wire_name}")
        if wire_name == conditional:
            if desc["kind"] != "optional" or desc["item"]["kind"] != "vector":
                fail(f"unexpected conditional property type at {name}.{wire_name}")
            desc = desc["item"]
        is_required = wire_name in required
        if not is_required and desc["kind"] != "optional":
            desc = {"kind": "optional", "item": desc}
        default_value = "{}"
        if desc["kind"] == "const_string":
            default_value = "{" + cpp_string(desc["value"]) + "}"
        if desc["kind"] == "null":
            default_value = "{nullptr}"
        fields.append(
            {
                "wireName": wire_name,
                "desc": desc,
                "required": is_required,
                "defaultValue": default_value,
            }
        )
    return fields


def cpp_signed_bound(bound: int, bits: int) -> str:
    if bits == 64:
        if bound == -9223372036854775808:
            return "std::numeric_limits<std::int64_t>::min()"
        if bound == 9223372036854775807:
            return "std::numeric_limits<std::int64_t>::max()"
    if bits == 32:
        if bound == -2147483648:
            return "std::numeric_limits<std::int32_t>::min()"
        if bound == 2147483647:
            return "std::numeric_limits<std::int32_t>::max()"
    return str(bound)


def parse_simple(desc: dict[str, Any], source: str, path_expr: str) -> str:
    kind = desc["kind"]
    if kind == "string":
        return f"std::string(detail::get_string({source}, {path_expr}))"
    if kind == "bool":
        return f"detail::get_bool({source}, {path_expr})"
    if kind == "uint32":
        return f"detail::get_uint32({source}, {path_expr}, {desc['min']}u, {desc['max']}u)"
    if kind == "int32":
        return (
            f"detail::get_int32({source}, {path_expr}, "
            f"{cpp_signed_bound(desc['min'], 32)}, {cpp_signed_bound(desc['max'], 32)})"
        )
    if kind == "int64":
        return (
            f"detail::get_int64({source}, {path_expr}, "
            f"{cpp_signed_bound(desc['min'], 64)}, {cpp_signed_bound(desc['max'], 64)})"
        )
    if kind in ("enum", "object"):
        return f"{source}.get<{desc['name']}>()"
    if kind == "const_string":
        return source
    if kind == "null":
        return "nullptr"
    fail(f"cannot parse {kind} as an expression")


def emit_assign(
    desc: dict[str, Any],
    target: str,
    source: str,
    path_expr: str,
    indent: int,
    lines: list[str],
) -> None:
    pad = " " * indent
    kind = desc["kind"]
    if kind in ("string", "bool", "uint32", "int32", "int64", "enum", "object"):
        lines.append(f"{pad}{target} = {parse_simple(desc, source, path_expr)};")
        return
    if kind == "const_string":
        lines.append(
            f"{pad}detail::require_const_string({source}, {cpp_string(desc['value'])}, {path_expr});"
        )
        lines.append(f"{pad}{target} = {cpp_string(desc['value'])};")
        return
    if kind == "null":
        lines.append(f"{pad}detail::require_null({source}, {path_expr});")
        lines.append(f"{pad}{target} = nullptr;")
        return
    if kind == "optional":
        lines.append(f"{pad}if ({source}.is_null()) {{")
        lines.append(f"{pad}    {target}.reset();")
        lines.append(f"{pad}}} else {{")
        if desc["item"]["kind"] in ("object", "vector", "map"):
            lines.append(f"{pad}    {target}.emplace();")
            emit_assign(desc["item"], f"*{target}", source, path_expr, indent + 4, lines)
        else:
            lines.append(f"{pad}    {target} = {parse_simple(desc['item'], source, path_expr)};")
        lines.append(f"{pad}}}")
        return
    if kind == "vector":
        lines.append(f"{pad}{{")
        lines.append(f"{pad}    const auto& array = detail::require_array({source}, {path_expr});")
        lines.append(f"{pad}    {target}.clear();")
        lines.append(f"{pad}    {target}.reserve(array.size());")
        lines.append(f"{pad}    for (std::size_t index = 0; index < array.size(); ++index) {{")
        lines.append(f"{pad}        const auto& element = array[index];")
        lines.append(
            f"{pad}        const auto element_path = std::string({path_expr}) + "
            f'"[" + std::to_string(index) + "]";'
        )
        item = desc["item"]
        if item["kind"] == "optional":
            lines.append(f"{pad}        if (element.is_null()) {{")
            lines.append(f"{pad}            {target}.push_back(std::nullopt);")
            lines.append(f"{pad}        }} else {{")
            value_type = cpp_type(item["item"])
            lines.append(f"{pad}            {value_type} item{{}};")
            emit_assign(item["item"], "item", "element", "element_path", indent + 12, lines)
            lines.append(f"{pad}            {target}.push_back(std::move(item));")
            lines.append(f"{pad}        }}")
        elif item["kind"] in ("object", "vector", "map"):
            value_type = cpp_type(item)
            lines.append(f"{pad}        {value_type} item{{}};")
            emit_assign(item, "item", "element", "element_path", indent + 8, lines)
            lines.append(f"{pad}        {target}.push_back(std::move(item));")
        else:
            lines.append(
                f"{pad}        {target}.push_back({parse_simple(item, 'element', 'element_path')});"
            )
        lines.append(f"{pad}    }}")
        lines.append(f"{pad}}}")
        return
    if kind == "map":
        lines.append(f"{pad}{{")
        lines.append(f"{pad}    const auto& object = detail::require_object({source}, {path_expr});")
        lines.append(f"{pad}    {target}.clear();")
        lines.append(
            f"{pad}    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {{"
        )
        lines.append(
            f'{pad}        const auto value_path = std::string({path_expr}) + "." + iterator.key();'
        )
        value = desc["value"]
        if value["kind"] == "string":
            lines.append(
                f"{pad}        {target}.emplace(iterator.key(), "
                f"detail::get_string(iterator.value(), value_path));"
            )
        else:
            value_type = cpp_type(value)
            lines.append(f"{pad}        {value_type} item{{}};")
            emit_assign(value, "item", "iterator.value()", "value_path", indent + 8, lines)
            lines.append(f"{pad}        {target}.emplace(iterator.key(), std::move(item));")
        lines.append(f"{pad}    }}")
        lines.append(f"{pad}}}")
        return
    fail(f"cannot assign {kind}")


def emit_serialize(
    desc: dict[str, Any],
    value_expr: str,
    wire_name: str,
    indent: int,
    lines: list[str],
    required: bool = True,
) -> None:
    pad = " " * indent
    key = cpp_string(wire_name)
    kind = desc["kind"]
    if kind in ("string", "bool", "uint32", "int32", "int64", "enum", "object", "const_string"):
        lines.append(f"{pad}j[{key}] = {value_expr};")
        return
    if kind == "null":
        lines.append(f"{pad}j[{key}] = nullptr;")
        return
    if kind == "optional":
        lines.append(f"{pad}if ({value_expr}) {{")
        emit_serialize(desc["item"], f"*{value_expr}", wire_name, indent + 4, lines)
        lines.append(f"{pad}}}")
        if required:
            lines.append(f"{pad}else {{")
            lines.append(f"{pad}    j[{key}] = nullptr;")
            lines.append(f"{pad}}}")
        return
    if kind == "vector":
        lines.append(f"{pad}{{")
        lines.append(f"{pad}    auto array = nlohmann::json::array();")
        lines.append(f"{pad}    for (const auto& element : {value_expr}) {{")
        if desc["item"]["kind"] == "optional":
            lines.append(f"{pad}        if (element) {{")
            lines.append(f"{pad}            array.push_back(*element);")
            lines.append(f"{pad}        }} else {{")
            lines.append(f"{pad}            array.push_back(nullptr);")
            lines.append(f"{pad}        }}")
        else:
            lines.append(f"{pad}        array.push_back(element);")
        lines.append(f"{pad}    }}")
        lines.append(f"{pad}    j[{key}] = std::move(array);")
        lines.append(f"{pad}}}")
        return
    if kind == "map":
        lines.append(f"{pad}j[{key}] = {value_expr};")
        return
    fail(f"cannot serialize {kind}")


def emit_struct(name: str, lines: list[str]) -> None:
    cpp_name = cpp_definition_name(name)
    fields = properties_of(name)
    lines.append(f"struct {cpp_name} {{")
    for field in fields:
        default = "" if field["defaultValue"] == "{}" else f" = {field['defaultValue']}"
        lines.append(f"    {cpp_type(field['desc'])} {field['wireName']}{default or '{}'};")
    lines.append(f"    bool operator==(const {cpp_name}&) const = default;")
    lines.append("};")
    lines.append("")


def emit_enum(enum_info: dict[str, Any], lines: list[str]) -> None:
    name = enum_info["name"]
    cpp_name = enum_info["cppName"]
    definition = enum_info["definition"]
    values = definition.get("enum")
    if not isinstance(values, list) or not values:
        fail(f"enum {name} is empty")
    seen: set[str] = set()
    lines.append(f"enum class {cpp_name} : std::uint8_t {{")
    for value in values:
        if not isinstance(value, str):
            fail(f"non-string enum value in {name}")
        enumerator = enum_value_name(value)
        if enumerator in seen:
            fail(f"duplicate enumerator {enumerator} in {name}")
        seen.add(enumerator)
        lines.append(f"    {enumerator},")
    lines.append("};")
    lines.append("")
    lines.append(f"inline void from_json(const nlohmann::json& j, {cpp_name}& value) {{")
    lines.append(f"    const auto text = detail::get_string(j, {cpp_string(name)});")
    for index, value in enumerate(values):
        keyword = "if" if index == 0 else "else if"
        lines.append(
            f"    {keyword} (text == {cpp_string(value)}) "
            f"value = {cpp_name}::{enum_value_name(value)};"
        )
    lines.append(
        f'    else throw detail::schema_error(std::string({cpp_string(name)}) + '
        f'": unknown enum value \\"" + std::string(text) + "\\"");'
    )
    lines.append("}")
    lines.append("")
    lines.append(f"inline void to_json(nlohmann::json& j, {cpp_name} value) {{")
    lines.append("    switch (value) {")
    for value in values:
        lines.append(f"        case {cpp_name}::{enum_value_name(value)}: j = {cpp_string(value)}; break;")
    lines.append("    }")
    lines.append("}")
    lines.append("")


def emit_struct_functions(name: str, lines: list[str]) -> None:
    cpp_name = cpp_definition_name(name)
    fields = properties_of(name)
    allowed = ", ".join(cpp_string(field["wireName"]) for field in fields)
    lines.append(f"inline void from_json(const nlohmann::json& j, {cpp_name}& value) {{")
    lines.append("    try {")
    lines.append(f"        detail::require_object(j, {cpp_string(name)});")
    lines.append(f"        detail::reject_unknown(j, {{{allowed}}}, {cpp_string(name)});")
    for field in fields:
        wire_name = field["wireName"]
        path_expr = f"std::string({cpp_string(name)}) + \".\" + {cpp_string(wire_name)}"
        if field["required"]:
            lines.append("        {")
            lines.append(
                f"            const auto& member = detail::require_member("
                f"j, {cpp_string(wire_name)}, {path_expr});"
            )
            emit_assign(field["desc"], f"value.{wire_name}", "member", path_expr, 12, lines)
            lines.append("        }")
        else:
            lines.append(
                f"        if (const auto* member = detail::optional_member(j, {cpp_string(wire_name)})) {{"
            )
            emit_assign(field["desc"], f"value.{wire_name}", "*member", path_expr, 12, lines)
            lines.append("        } else {")
            lines.append(f"            value.{wire_name}.reset();")
            lines.append("        }")
    lines.append("    } catch (const detail::schema_error&) {")
    lines.append("        throw;")
    lines.append("    } catch (const std::exception& error) {")
    lines.append(
        f"        throw detail::schema_error(std::string({cpp_string(name)}) + \": \" + error.what());"
    )
    lines.append("    }")
    lines.append("}")
    lines.append("")
    lines.append(f"inline void to_json(nlohmann::json& j, const {cpp_name}& value) {{")
    lines.append("    j = nlohmann::json::object();")
    for field in fields:
        emit_serialize(
            field["desc"],
            f"value.{field['wireName']}",
            field["wireName"],
            4,
            lines,
            field["required"],
        )
    lines.append("}")
    lines.append("")


def root_alternatives(root_name: str) -> list[dict[str, str]]:
    root_schema = roots[root_name]
    if "oneOf" not in root_schema:
        fail(f"{root_name} root has no oneOf")
    alternatives: list[dict[str, str]] = []
    for entry in root_schema["oneOf"]:
        name = definition_name_from_ref(entry.get("$ref"))
        if classify_definition(name) != "object":
            fail(f"{root_name} branch {name} is not an object")
        alternatives.append({"definition": name, "cppName": cpp_definition_name(name)})
    return alternatives


lines: list[str] = []
lines.append(f"// Generated by {GENERATOR}. Do not edit.")
lines.append("//")
lines.append(f"// Official schemas: {protocol_source['repository']}")
lines.append(f"// Revision: {protocol_source['revision']}")
lines.append("")
lines.append("#pragma once")
lines.append("")
for include in (
    "cstddef",
    "cstdint",
    "limits",
    "map",
    "optional",
    "string",
    "string_view",
    "utility",
    "variant",
    "vector",
):
    lines.append(f"#include <{include}>")
lines.append("")
lines.append("#include <nlohmann/json.hpp>")
lines.append("")
lines.append('#include "livesplit/bridge/detail/schema.hpp"')
lines.append("")
lines.append("namespace livesplit::bridge {")
lines.append("")

for enum_info in enums:
    emit_enum(enum_info, lines)

for name in object_order:
    emit_struct(name, lines)

request_alternatives = root_alternatives("rpc_request")
response_alternatives = root_alternatives("rpc_response")
event_alternatives = root_alternatives("bridge_event")


def render_variant(name: str, alternatives: list[dict[str, str]]) -> None:
    alternatives_text = ", ".join(entry["cppName"] for entry in alternatives)
    lines.append(f"using {name} = std::variant<{alternatives_text}>;")
    lines.append("")


render_variant("rpc_request", request_alternatives)
render_variant("rpc_response", response_alternatives)
render_variant("bridge_event", event_alternatives)

for name in object_order:
    emit_struct_functions(name, lines)

for name, alternatives in (
    ("rpc_request", request_alternatives),
    ("rpc_response", response_alternatives),
    ("bridge_event", event_alternatives),
):
    title = {
        "rpc_request": "RpcRequest",
        "rpc_response": "RpcResponse",
        "bridge_event": "BridgeEvent",
    }[name]
    alternatives_text = ", ".join(entry["cppName"] for entry in alternatives)
    lines.append(f"inline {name} parse_{name}(std::string_view text) {{")
    lines.append("    const auto document = detail::parse_strict(text);")
    lines.append(
        f"    return detail::parse_variant<{name}, {alternatives_text}>(document, {cpp_string(title)});"
    )
    lines.append("}")
    lines.append("")
    lines.append(f"inline void from_json(const nlohmann::json& document, {name}& value) {{")
    lines.append(
        f"    value = detail::parse_variant<{name}, {alternatives_text}>(document, {cpp_string(title)});"
    )
    lines.append("}")
    lines.append("")

lines.append("}  // namespace livesplit::bridge")
lines.append("")

generated = "\n".join(lines)

if "--check" in sys.argv:
    try:
        existing = OUTPUT_PATH.read_text(encoding="utf-8")
    except OSError:
        fail(f"{OUTPUT_PATH.relative_to(ROOT)} does not exist")
    if existing.replace("\r\n", "\n") != generated:
        fail(f"{OUTPUT_PATH.relative_to(ROOT)} is out of date; run python {GENERATOR}")
    print("protocol models are up to date")
else:
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_bytes(generated.encode("utf-8"))
    print(f"wrote {OUTPUT_PATH.relative_to(ROOT)}")
