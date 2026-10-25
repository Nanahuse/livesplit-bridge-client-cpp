#!/usr/bin/env node

// Deterministic generator for the LiveSplit.Bridge JSON v4 wire models.
//
// The official schemas are pinned by tools/protocol-source.json and
// schemas/v4/SOURCE.json. The generated header is committed; normal builds do
// not require Node.js. Run `node tools/generate-models.mjs --check` in CI.

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..");
const schemaDir = path.join(root, "schemas", "v4");
const outputPath = path.join(root, "include", "livesplit", "bridge", "protocol.hpp");

function fail(message) {
  console.error(`generate-models: ${message}`);
  process.exit(1);
}

function stable(value) {
  if (Array.isArray(value)) return `[${value.map(stable).join(",")}]`;
  if (value && typeof value === "object") {
    const entries = Object.keys(value)
      .sort()
      .map((key) => `${JSON.stringify(key)}:${stable(value[key])}`);
    return `{${entries.join(",")}}`;
  }
  return JSON.stringify(value);
}

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch (error) {
    fail(`cannot read ${path.relative(root, file)}: ${error.message}`);
  }
}

function sha256(file) {
  // Git may check out text files with CRLF on Windows. Hash and compare the
  // canonical LF form so the pinned official schemas remain verifiable.
  const text = fs.readFileSync(file, "utf8").replace(/\r\n/g, "\n");
  return crypto.createHash("sha256").update(text, "utf8").digest("hex");
}

const protocolSource = readJson(path.join(root, "tools", "protocol-source.json"));
const source = readJson(path.join(schemaDir, "SOURCE.json"));
if (protocolSource.revision !== source.revision) {
  fail("tools/protocol-source.json and schemas/v4/SOURCE.json revisions differ");
}

const schemaNames = [
  "rpc-request.schema.json",
  "rpc-response.schema.json",
  "bridge-event.schema.json",
];
const documents = {};
for (const name of schemaNames) {
  const file = path.join(schemaDir, name);
  const expected = source.files[name];
  if (!expected) fail(`schemas/v4/SOURCE.json has no hash for ${name}`);
  const actual = sha256(file);
  if (actual !== expected) {
    fail(`${name} differs from the pinned official schema (expected ${expected}, got ${actual})`);
  }
  documents[name] = readJson(file);
}

// Merge the official definitions and require exact agreement for shared names.
const defs = new Map();
for (const name of schemaNames) {
  for (const [definitionName, definition] of Object.entries(documents[name].$defs ?? {})) {
    if (defs.has(definitionName)) {
      if (stable(defs.get(definitionName)) !== stable(definition)) {
        fail(`definition ${definitionName} differs between official schemas`);
      }
    } else {
      defs.set(definitionName, definition);
    }
  }
}

const roots = {
  rpc_request: documents["rpc-request.schema.json"],
  rpc_response: documents["rpc-response.schema.json"],
  bridge_event: documents["bridge-event.schema.json"],
};

function definitionNameFromRef(ref) {
  const prefix = "#/$defs/";
  if (typeof ref !== "string" || !ref.startsWith(prefix)) {
    fail(`unsupported $ref: ${JSON.stringify(ref)}`);
  }
  const name = ref.slice(prefix.length);
  if (!defs.has(name)) fail(`unresolved $ref: ${name}`);
  return name;
}

function snake(name) {
  return name
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/([A-Z]+)([A-Z][a-z])/g, "$1_$2")
    .replace(/[^A-Za-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "")
    .toLowerCase();
}

function enumValueName(value) {
  const name = value
    .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
    .replace(/[^A-Za-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "")
    .toUpperCase();
  return /^[0-9]/.test(name) ? `_${name}` : name;
}

function cppString(value) {
  return JSON.stringify(value);
}

const definitionCppNames = new Map([["CompletedCount", "completed_count_value"]]);

function cppDefinitionName(name) {
  return definitionCppNames.get(name) ?? snake(name);
}

function isAliasDefinition(name) {
  const definition = defs.get(name);
  return definition.type === "array" ||
    (definition.type === "object" && definition.properties === undefined && definition.additionalProperties);
}

function classifyDefinition(name) {
  const definition = defs.get(name);
  if (definition.enum) return "enum";
  if (definition.type === "object" && definition.properties) return "object";
  if (isAliasDefinition(name)) return "alias";
  fail(`unsupported definition ${name}`);
}

function typeOf(schema, context) {
  if (!schema || typeof schema !== "object") fail(`missing schema at ${context}`);

  if (schema.$ref) {
    const name = definitionNameFromRef(schema.$ref);
    const kind = classifyDefinition(name);
    if (kind === "alias") return typeOf(defs.get(name), `${context} -> ${name}`);
    if (kind === "enum") return { kind: "enum", name: cppDefinitionName(name), definition: name };
    return { kind: "object", name: cppDefinitionName(name), definition: name };
  }

  if (schema.oneOf) {
    fail(`inline oneOf is not supported at ${context}`);
  }

  if (schema.anyOf) {
    const nonNull = schema.anyOf.filter((entry) => entry.type !== "null");
    const hasNull = schema.anyOf.some((entry) => entry.type === "null");
    if (nonNull.length === 1 && (hasNull || schema.anyOf.length === 1)) {
      const inner = typeOf(nonNull[0], `${context}.anyOf`);
      return hasNull ? { kind: "optional", item: inner } : inner;
    }
    fail(`unsupported anyOf at ${context}`);
  }

  if (Object.prototype.hasOwnProperty.call(schema, "const")) {
    if (schema.const === null) return { kind: "null" };
    if (typeof schema.const === "string") {
      return { kind: "const_string", value: schema.const };
    }
    fail(`unsupported const at ${context}`);
  }

  if (schema.enum) {
    fail(`inline enum is not supported at ${context}`);
  }

  let type = schema.type;
  if (Array.isArray(type)) {
    const nonNull = type.filter((entry) => entry !== "null");
    if (nonNull.length !== 1) fail(`unsupported type array at ${context}`);
    if (type.includes("null")) {
      return { kind: "optional", item: typeOf({ ...schema, type: nonNull[0] }, context) };
    }
    type = nonNull[0];
  }

  switch (type) {
    case "string":
      return { kind: "string" };
    case "boolean":
      return { kind: "bool" };
    case "integer": {
      const minimum = schema.minimum;
      const maximum = schema.maximum;
      // JSON.parse represents the int64 schema bounds as doubles. The official
      // schemas use the full int64 range, whose maximum rounds above 2^63 - 1.
      const min = minimum === undefined
        ? null
        : minimum <= Number.MIN_SAFE_INTEGER ? -9223372036854775808n : BigInt(minimum);
      const max = maximum === undefined
        ? null
        : maximum >= Number.MAX_SAFE_INTEGER ? 9223372036854775807n : BigInt(maximum);
      if ((min === null || min >= 0n) && max !== null && max <= 4294967295n) {
        return { kind: "uint32", min: min ?? 0n, max };
      }
      if ((min === null || min >= -2147483648n) && max !== null && max <= 2147483647n) {
        return { kind: "int32", min: min ?? -2147483648n, max };
      }
      if (min !== null && max !== null && (min < -2147483648n || max > 2147483647n)) {
        if (min >= -9223372036854775808n && max <= 9223372036854775807n) {
          return { kind: "int64", min, max };
        }
      }
      fail(`unsupported integer bounds at ${context}`);
      break;
    }
    case "array":
      return { kind: "vector", item: typeOf(schema.items, `${context}[]`) };
    case "object":
      if (schema.additionalProperties && typeof schema.additionalProperties === "object") {
        return {
          kind: "map",
          value: typeOf(schema.additionalProperties, `${context}.*`),
        };
      }
      fail(`inline object is not supported at ${context}`);
      break;
    default:
      fail(`unsupported schema type at ${context}: ${JSON.stringify(schema.type)}`);
  }
}

function cppType(desc) {
  switch (desc.kind) {
    case "string":
      return "std::string";
    case "bool":
      return "bool";
    case "uint32":
      return "std::uint32_t";
    case "int32":
      return "std::int32_t";
    case "int64":
      return "std::int64_t";
    case "enum":
    case "object":
      return desc.name;
    case "vector":
      return `std::vector<${cppType(desc.item)}>`;
    case "map":
      return `std::map<std::string, ${cppType(desc.value)}>`;
    case "optional":
      return `std::optional<${cppType(desc.item)}>`;
    case "null":
      return "std::nullptr_t";
    case "const_string":
      return "std::string";
    default:
      fail(`cannot render C++ type for ${desc.kind}`);
  }
}

const enums = [...defs.entries()]
  .filter(([, definition]) => definition.enum)
  .map(([name]) => ({ name, cppName: cppDefinitionName(name), definition: defs.get(name) }));

const objectNames = [...defs.entries()]
  .filter(([, definition]) => definition.type === "object" && definition.properties)
  .map(([name]) => name);

function refsInSchema(schema, output, expandedAliases = new Set()) {
  if (!schema || typeof schema !== "object") return;
  if (schema.$ref) {
    const name = definitionNameFromRef(schema.$ref);
    output.add(name);
    if (classifyDefinition(name) === "alias" && !expandedAliases.has(name)) {
      expandedAliases.add(name);
      refsInSchema(defs.get(name), output, expandedAliases);
    }
  }
  for (const value of Object.values(schema)) {
    if (Array.isArray(value)) {
      for (const entry of value) refsInSchema(entry, output, expandedAliases);
    } else if (value && typeof value === "object") {
      refsInSchema(value, output, expandedAliases);
    }
  }
}

const objectOrder = [];
const visiting = new Set();
const visited = new Set();

function visitObject(name) {
  if (visited.has(name)) return;
  if (visiting.has(name)) fail(`cyclic object definitions involving ${name}`);
  visiting.add(name);
  const refs = new Set();
  refsInSchema(defs.get(name), refs);
  for (const ref of [...refs].sort()) {
    if (classifyDefinition(ref) === "object") visitObject(ref);
  }
  visiting.delete(name);
  visited.add(name);
  objectOrder.push(name);
}
for (const name of [...objectNames].sort()) visitObject(name);

function conditionalArrayProperty(definition, context) {
  if (!definition.if && !definition.then) return null;
  if (!definition.if || !definition.then) fail(`unsupported if/then at ${context}`);
  const ifProperties = Object.entries(definition.if.properties ?? {});
  const required = new Set(definition.if.required ?? []);
  if (ifProperties.length !== 1) fail(`unsupported if condition at ${context}`);
  const [property, condition] = ifProperties[0];
  if (condition.const !== null || !required.has(property)) {
    fail(`unsupported if condition at ${context}`);
  }
  const thenProperty = definition.then.properties?.[property];
  if (!thenProperty) fail(`unsupported then clause at ${context}`);
  const thenType = typeOf(thenProperty, `${context}.then.${property}`);
  if (cppType(thenType) !== "std::vector<std::string>") {
    fail(`unexpected then type for ${context}.${property}`);
  }
  return property;
}

function propertiesOf(name) {
  const definition = defs.get(name);
  const required = new Set(definition.required ?? []);
  const conditional = conditionalArrayProperty(definition, name);
  const fields = [];
  for (const [wireName, schema] of Object.entries(definition.properties)) {
    let desc = typeOf(schema, `${name}.${wireName}`);
    if (wireName === conditional) {
      if (desc.kind !== "optional" || desc.item.kind !== "vector") {
        fail(`unexpected conditional property type at ${name}.${wireName}`);
      }
      desc = desc.item;
    }
    const isRequired = required.has(wireName);
    if (!isRequired && desc.kind !== "optional") {
      desc = { kind: "optional", item: desc };
    }
    let defaultValue = "{}";
    if (desc.kind === "const_string") defaultValue = `{${cppString(desc.value)}}`;
    if (desc.kind === "null") defaultValue = "{nullptr}";
    fields.push({ wireName, desc, required: isRequired, defaultValue });
  }
  return fields;
}

function cppSignedBound(bound, bits) {
  if (bits === 64) {
    if (bound === -9223372036854775808n) return "std::numeric_limits<std::int64_t>::min()";
    if (bound === 9223372036854775807n) return "std::numeric_limits<std::int64_t>::max()";
  }
  if (bits === 32) {
    if (bound === -2147483648n) return "std::numeric_limits<std::int32_t>::min()";
    if (bound === 2147483647n) return "std::numeric_limits<std::int32_t>::max()";
  }
  return bound.toString();
}

function parseSimple(desc, source, pathExpr) {
  switch (desc.kind) {
    case "string":
      return `std::string(detail::get_string(${source}, ${pathExpr}))`;
    case "bool":
      return `detail::get_bool(${source}, ${pathExpr})`;
    case "uint32":
      return `detail::get_uint32(${source}, ${pathExpr}, ${desc.min}u, ${desc.max}u)`;
    case "int32":
      return `detail::get_int32(${source}, ${pathExpr}, ${cppSignedBound(desc.min, 32)}, ${cppSignedBound(desc.max, 32)})`;
    case "int64":
      return `detail::get_int64(${source}, ${pathExpr}, ${cppSignedBound(desc.min, 64)}, ${cppSignedBound(desc.max, 64)})`;
    case "enum":
      return `${source}.get<${desc.name}>()`;
    case "object":
      return `${source}.get<${desc.name}>()`;
    case "const_string":
      return `${source}`;
    case "null":
      return "nullptr";
    default:
      fail(`cannot parse ${desc.kind} as an expression`);
  }
}

function emitAssign(desc, target, source, pathExpr, indent, lines) {
  const pad = " ".repeat(indent);
  switch (desc.kind) {
    case "string":
    case "bool":
    case "uint32":
    case "int32":
    case "int64":
    case "enum":
    case "object":
      lines.push(`${pad}${target} = ${parseSimple(desc, source, pathExpr)};`);
      return;
    case "const_string":
      lines.push(`${pad}detail::require_const_string(${source}, ${cppString(desc.value)}, ${pathExpr});`);
      lines.push(`${pad}${target} = ${cppString(desc.value)};`);
      return;
    case "null":
      lines.push(`${pad}detail::require_null(${source}, ${pathExpr});`);
      lines.push(`${pad}${target} = nullptr;`);
      return;
    case "optional": {
      lines.push(`${pad}if (${source}.is_null()) {`);
      lines.push(`${pad}    ${target}.reset();`);
      lines.push(`${pad}} else {`);
      if (desc.item.kind === "object" || desc.item.kind === "vector" || desc.item.kind === "map") {
        lines.push(`${pad}    ${target}.emplace();`);
        emitAssign(desc.item, `*${target}`, source, pathExpr, indent + 4, lines);
      } else {
        lines.push(`${pad}    ${target} = ${parseSimple(desc.item, source, pathExpr)};`);
      }
      lines.push(`${pad}}`);
      return;
    }
    case "vector": {
      lines.push(`${pad}{`);
      lines.push(`${pad}    const auto& array = detail::require_array(${source}, ${pathExpr});`);
      lines.push(`${pad}    ${target}.clear();`);
      lines.push(`${pad}    ${target}.reserve(array.size());`);
      lines.push(`${pad}    for (std::size_t index = 0; index < array.size(); ++index) {`);
      lines.push(`${pad}        const auto& element = array[index];`);
      lines.push(`${pad}        const auto element_path = std::string(${pathExpr}) + "[" + std::to_string(index) + "]";`);
      if (desc.item.kind === "optional") {
        lines.push(`${pad}        if (element.is_null()) {`);
        lines.push(`${pad}            ${target}.push_back(std::nullopt);`);
        lines.push(`${pad}        } else {`);
        const valueType = cppType(desc.item.item);
        lines.push(`${pad}            ${valueType} item{};`);
        emitAssign(desc.item.item, "item", "element", "element_path", indent + 12, lines);
        lines.push(`${pad}            ${target}.push_back(std::move(item));`);
        lines.push(`${pad}        }`);
      } else if (desc.item.kind === "object" || desc.item.kind === "vector" || desc.item.kind === "map") {
        const valueType = cppType(desc.item);
        lines.push(`${pad}        ${valueType} item{};`);
        emitAssign(desc.item, "item", "element", "element_path", indent + 8, lines);
        lines.push(`${pad}        ${target}.push_back(std::move(item));`);
      } else {
        lines.push(`${pad}        ${target}.push_back(${parseSimple(desc.item, "element", "element_path")});`);
      }
      lines.push(`${pad}    }`);
      lines.push(`${pad}}`);
      return;
    }
    case "map": {
      lines.push(`${pad}{`);
      lines.push(`${pad}    const auto& object = detail::require_object(${source}, ${pathExpr});`);
      lines.push(`${pad}    ${target}.clear();`);
      lines.push(`${pad}    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {`);
      lines.push(`${pad}        const auto value_path = std::string(${pathExpr}) + "." + iterator.key();`);
      if (desc.value.kind === "string") {
        lines.push(`${pad}        ${target}.emplace(iterator.key(), detail::get_string(iterator.value(), value_path));`);
      } else {
        const valueType = cppType(desc.value);
        lines.push(`${pad}        ${valueType} item{};`);
        emitAssign(desc.value, "item", "iterator.value()", "value_path", indent + 8, lines);
        lines.push(`${pad}        ${target}.emplace(iterator.key(), std::move(item));`);
      }
      lines.push(`${pad}    }`);
      lines.push(`${pad}}`);
      return;
    }
    default:
      fail(`cannot assign ${desc.kind}`);
  }
}

function emitSerialize(desc, valueExpr, wireName, indent, lines, required = true) {
  const pad = " ".repeat(indent);
  const key = cppString(wireName);
  switch (desc.kind) {
    case "string":
    case "bool":
    case "uint32":
    case "int32":
    case "int64":
    case "enum":
    case "object":
    case "const_string":
      lines.push(`${pad}j[${key}] = ${valueExpr};`);
      return;
    case "null":
      lines.push(`${pad}j[${key}] = nullptr;`);
      return;
    case "optional": {
      lines.push(`${pad}if (${valueExpr}) {`);
      emitSerialize(desc.item, `*${valueExpr}`, wireName, indent + 4, lines);
      lines.push(`${pad}}`);
      if (required) {
        lines.push(`${pad}else {`);
        lines.push(`${pad}    j[${key}] = nullptr;`);
        lines.push(`${pad}}`);
      }
      return;
    }
    case "vector": {
      lines.push(`${pad}{`);
      lines.push(`${pad}    auto array = nlohmann::json::array();`);
      lines.push(`${pad}    for (const auto& element : ${valueExpr}) {`);
      if (desc.item.kind === "optional") {
        lines.push(`${pad}        if (element) {`);
        lines.push(`${pad}            array.push_back(*element);`);
        lines.push(`${pad}        } else {`);
        lines.push(`${pad}            array.push_back(nullptr);`);
        lines.push(`${pad}        }`);
      } else {
        lines.push(`${pad}        array.push_back(element);`);
      }
      lines.push(`${pad}    }`);
      lines.push(`${pad}    j[${key}] = std::move(array);`);
      lines.push(`${pad}}`);
      return;
    }
    case "map":
      lines.push(`${pad}j[${key}] = ${valueExpr};`);
      return;
    default:
      fail(`cannot serialize ${desc.kind}`);
  }
}

function emitStruct(name, lines) {
  const cppName = cppDefinitionName(name);
  const fields = propertiesOf(name);
  lines.push(`struct ${cppName} {`);
  for (const field of fields) {
    lines.push(`    ${cppType(field.desc)} ${field.wireName}${field.defaultValue === "{}" ? "{}" : ` = ${field.defaultValue}`};`);
  }
  lines.push(`    bool operator==(const ${cppName}&) const = default;`);
  lines.push(`};`);
  lines.push("");
}

function emitEnum(enumInfo, lines) {
  const { name, cppName, definition } = enumInfo;
  if (!Array.isArray(definition.enum) || definition.enum.length === 0) {
    fail(`enum ${name} is empty`);
  }
  const seen = new Set();
  lines.push(`enum class ${cppName} : std::uint8_t {`);
  for (const value of definition.enum) {
    if (typeof value !== "string") fail(`non-string enum value in ${name}`);
    const enumerator = enumValueName(value);
    if (seen.has(enumerator)) fail(`duplicate enumerator ${enumerator} in ${name}`);
    seen.add(enumerator);
    lines.push(`    ${enumerator},`);
  }
  lines.push(`};`);
  lines.push("");
  lines.push(`inline void from_json(const nlohmann::json& j, ${cppName}& value) {`);
  lines.push(`    const auto text = detail::get_string(j, ${cppString(name)});`);
  let first = true;
  for (const value of definition.enum) {
    lines.push(`    ${first ? "if" : "else if"} (text == ${cppString(value)}) value = ${cppName}::${enumValueName(value)};`);
    first = false;
  }
  lines.push(`    else throw detail::schema_error(std::string(${cppString(name)}) + ": unknown enum value \\"" + std::string(text) + "\\"");`);
  lines.push(`}`);
  lines.push("");
  lines.push(`inline void to_json(nlohmann::json& j, ${cppName} value) {`);
  lines.push(`    switch (value) {`);
  for (const value of definition.enum) {
    lines.push(`        case ${cppName}::${enumValueName(value)}: j = ${cppString(value)}; break;`);
  }
  lines.push(`    }`);
  lines.push(`}`);
  lines.push("");
}

function emitStructFunctions(name, lines) {
  const cppName = cppDefinitionName(name);
  const fields = propertiesOf(name);
  const allowed = fields.map((field) => cppString(field.wireName)).join(", ");
  lines.push(`inline void from_json(const nlohmann::json& j, ${cppName}& value) {`);
  lines.push(`    try {`);
  lines.push(`        detail::require_object(j, ${cppString(name)});`);
  lines.push(`        detail::reject_unknown(j, {${allowed}}, ${cppString(name)});`);
  for (const field of fields) {
    const pathExpr = `std::string(${cppString(name)}) + "." + ${cppString(field.wireName)}`;
    if (field.required) {
      lines.push(`        {`);
      lines.push(`            const auto& member = detail::require_member(j, ${cppString(field.wireName)}, ${pathExpr});`);
      emitAssign(field.desc, `value.${field.wireName}`, "member", pathExpr, 12, lines);
      lines.push(`        }`);
    } else {
      lines.push(`        if (const auto* member = detail::optional_member(j, ${cppString(field.wireName)})) {`);
      emitAssign(field.desc, `value.${field.wireName}`, "*member", pathExpr, 12, lines);
      lines.push(`        } else {`);
      lines.push(`            value.${field.wireName}.reset();`);
      lines.push(`        }`);
    }
  }
  lines.push(`    } catch (const detail::schema_error&) {`);
  lines.push(`        throw;`);
  lines.push(`    } catch (const std::exception& error) {`);
  lines.push(`        throw detail::schema_error(std::string(${cppString(name)}) + ": " + error.what());`);
  lines.push(`    }`);
  lines.push(`}`);
  lines.push("");
  lines.push(`inline void to_json(nlohmann::json& j, const ${cppName}& value) {`);
  lines.push(`    j = nlohmann::json::object();`);
  for (const field of fields) {
    emitSerialize(field.desc, `value.${field.wireName}`, field.wireName, 4, lines, field.required);
  }
  lines.push(`}`);
  lines.push("");
}

function rootAlternatives(rootName) {
  const rootSchema = roots[rootName];
  if (!rootSchema.oneOf) fail(`${rootName} root has no oneOf`);
  return rootSchema.oneOf.map((entry) => {
    const name = definitionNameFromRef(entry.$ref);
    if (classifyDefinition(name) !== "object") fail(`${rootName} branch ${name} is not an object`);
    return { definition: name, cppName: cppDefinitionName(name) };
  });
}

const lines = [];
lines.push("// Generated by tools/generate-models.mjs. Do not edit.");
lines.push("//");
lines.push(`// Official schemas: ${protocolSource.repository}`);
lines.push(`// Revision: ${protocolSource.revision}`);
lines.push("");
lines.push("#pragma once");
lines.push("");
lines.push("#include <cstddef>");
lines.push("#include <cstdint>");
lines.push("#include <limits>");
lines.push("#include <map>");
lines.push("#include <optional>");
lines.push("#include <string>");
lines.push("#include <string_view>");
lines.push("#include <utility>");
lines.push("#include <variant>");
lines.push("#include <vector>");
lines.push("");
lines.push("#include <nlohmann/json.hpp>");
lines.push("");
lines.push('#include "livesplit/bridge/detail/schema.hpp"');
lines.push("");
lines.push("namespace livesplit::bridge {");
lines.push("");

for (const enumInfo of enums) emitEnum(enumInfo, lines);

for (const name of objectOrder) emitStruct(name, lines);

const requestAlternatives = rootAlternatives("rpc_request");
const responseAlternatives = rootAlternatives("rpc_response");
const eventAlternatives = rootAlternatives("bridge_event");

function renderVariant(name, alternatives) {
  lines.push(`using ${name} = std::variant<${alternatives.map((entry) => entry.cppName).join(", ")}>;`);
  lines.push("");
}
renderVariant("rpc_request", requestAlternatives);
renderVariant("rpc_response", responseAlternatives);
renderVariant("bridge_event", eventAlternatives);

for (const name of objectOrder) emitStructFunctions(name, lines);

for (const [name, alternatives] of [
  ["rpc_request", requestAlternatives],
  ["rpc_response", responseAlternatives],
  ["bridge_event", eventAlternatives],
]) {
  const title = name === "rpc_request" ? "RpcRequest" : name === "rpc_response" ? "RpcResponse" : "BridgeEvent";
  lines.push(`inline ${name} parse_${name}(std::string_view text) {`);
  lines.push(`    const auto document = detail::parse_strict(text);`);
  lines.push(`    return detail::parse_variant<${name}, ${alternatives.map((entry) => entry.cppName).join(", ")}>(document, ${cppString(title)});`);
  lines.push(`}`);
  lines.push("");
  lines.push(`inline void from_json(const nlohmann::json& document, ${name}& value) {`);
  lines.push(`    value = detail::parse_variant<${name}, ${alternatives.map((entry) => entry.cppName).join(", ")}>(document, ${cppString(title)});`);
  lines.push(`}`);
  lines.push("");
}

lines.push("}  // namespace livesplit::bridge");
lines.push("");

const generated = lines.join("\n");

const check = process.argv.includes("--check");
if (check) {
  let existing = "";
  try {
    existing = fs.readFileSync(outputPath, "utf8");
  } catch {
    fail(`${path.relative(root, outputPath)} does not exist`);
  }
  if (existing.replace(/\r\n/g, "\n") !== generated) {
    fail(`${path.relative(root, outputPath)} is out of date; run node tools/generate-models.mjs`);
  }
  console.log("protocol models are up to date");
} else {
  fs.mkdirSync(path.dirname(outputPath), { recursive: true });
  fs.writeFileSync(outputPath, generated);
  console.log(`wrote ${path.relative(root, outputPath)}`);
}
