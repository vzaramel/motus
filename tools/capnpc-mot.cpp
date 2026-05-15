/*
 * capnpc-mot — Cap'n Proto compiler plugin for Motus
 *
 * Reads a CodeGeneratorRequest from stdin (standard capnp plugin protocol)
 * and outputs a JSON schema descriptor to stdout that the Motus compiler
 * can consume.
 *
 * Build: c++ -std=c++17 -o capnpc-mot capnpc-mot.cpp -lcapnp -lkj
 * Usage: capnp compile -o ./capnpc-mot schema.capnp
 */

#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>
#include <kj/io.h>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace capnp::schema;

/* JSON escape helper */
static void jsonStr(FILE *out, const std::string &s) {
    fputc('"', out);
    for (char c : s) {
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:   fputc(c, out); break;
        }
    }
    fputc('"', out);
}

/* Extract short name from display name (after last ':' or prefix) */
static std::string shortName(Node::Reader node) {
    std::string full(node.getDisplayName().cStr());
    auto colon = full.rfind(':');
    if (colon != std::string::npos) return full.substr(colon + 1);
    auto prefix = node.getDisplayNamePrefixLength();
    return prefix < full.size() ? full.substr(prefix) : full;
}

/* Convert Type to string representation */
static std::string typeStr(Type::Reader type,
                           const std::map<uint64_t, std::string> &nodeNames) {
    switch (type.which()) {
        case Type::VOID:    return "Void";
        case Type::BOOL:    return "Bool";
        case Type::INT8:    return "Int8";
        case Type::INT16:   return "Int16";
        case Type::INT32:   return "Int32";
        case Type::INT64:   return "Int64";
        case Type::UINT8:   return "UInt8";
        case Type::UINT16:  return "UInt16";
        case Type::UINT32:  return "UInt32";
        case Type::UINT64:  return "UInt64";
        case Type::FLOAT32: return "Float32";
        case Type::FLOAT64: return "Float64";
        case Type::TEXT:    return "Text";
        case Type::DATA:    return "Data";
        case Type::LIST:
            return "List(" + typeStr(type.getList().getElementType(), nodeNames) + ")";
        case Type::ENUM: {
            auto it = nodeNames.find(type.getEnum().getTypeId());
            return it != nodeNames.end() ? it->second : "Enum";
        }
        case Type::STRUCT: {
            auto it = nodeNames.find(type.getStruct().getTypeId());
            return it != nodeNames.end() ? it->second : "Struct";
        }
        default: return "Unknown";
    }
}

/* Convert Value to string, return empty for zero/void defaults */
static std::string valueStr(Value::Reader val) {
    switch (val.which()) {
        case Value::BOOL:    return val.getBool() ? "true" : "false";
        case Value::INT8:    return std::to_string(val.getInt8());
        case Value::INT16:   return std::to_string(val.getInt16());
        case Value::INT32:   return std::to_string(val.getInt32());
        case Value::INT64:   return std::to_string(val.getInt64());
        case Value::UINT8:   return std::to_string(val.getUint8());
        case Value::UINT16:  return std::to_string(val.getUint16());
        case Value::UINT32:  return std::to_string(val.getUint32());
        case Value::UINT64:  return std::to_string(val.getUint64());
        case Value::FLOAT32: return std::to_string(val.getFloat32());
        case Value::FLOAT64: return std::to_string(val.getFloat64());
        case Value::TEXT:    return std::string(val.getText().cStr());
        default: return "";
    }
}

static bool isNonTrivialDefault(Value::Reader val) {
    switch (val.which()) {
        case Value::BOOL:    return val.getBool();
        case Value::INT8:    return val.getInt8() != 0;
        case Value::INT16:   return val.getInt16() != 0;
        case Value::INT32:   return val.getInt32() != 0;
        case Value::INT64:   return val.getInt64() != 0;
        case Value::UINT8:   return val.getUint8() != 0;
        case Value::UINT16:  return val.getUint16() != 0;
        case Value::UINT32:  return val.getUint32() != 0;
        case Value::UINT64:  return val.getUint64() != 0;
        case Value::FLOAT32: return val.getFloat32() != 0.0f;
        case Value::FLOAT64: return val.getFloat64() != 0.0;
        case Value::TEXT:    return val.getText().size() > 0;
        default: return false;
    }
}

/* Emit an annotations object: {"name": "value", "bare": true, ...} */
static void emitAnnotations(
    FILE *out,
    ::capnp::List<Annotation, ::capnp::Kind::STRUCT>::Reader annos,
    const std::map<uint64_t, std::string> &nodeNames) {
    fprintf(out, "{");
    bool first = true;
    for (auto anno : annos) {
        auto it = nodeNames.find(anno.getId());
        if (it == nodeNames.end()) continue;
        if (!first) fprintf(out, ",");
        first = false;
        jsonStr(out, it->second);
        fprintf(out, ":");
        auto val = anno.getValue();
        if (val.which() == Value::VOID) {
            fprintf(out, "true");
        } else if (val.which() == Value::TEXT) {
            jsonStr(out, std::string(val.getText().cStr()));
        } else if (val.which() == Value::BOOL) {
            fprintf(out, val.getBool() ? "true" : "false");
        } else {
            auto s = valueStr(val);
            if (s.empty()) fprintf(out, "true");
            else fprintf(out, "%s", s.c_str());
        }
    }
    fprintf(out, "}");
}

/* Check if a node belongs to a requested file (walk scope chain) */
static bool isFromRequestedFile(
    Node::Reader node,
    const std::map<uint64_t, bool> &requestedIds,
    const std::map<uint64_t, Node::Reader> &nodeById) {
    if (requestedIds.count(node.getId())) return true;
    uint64_t id = node.getScopeId();
    for (int i = 0; i < 16; i++) {
        if (requestedIds.count(id)) return true;
        auto it = nodeById.find(id);
        if (it == nodeById.end()) break;
        id = it->second.getScopeId();
        if (id == 0) break;
    }
    return false;
}

int main() {
    kj::FdInputStream fdInput(0);
    capnp::InputStreamMessageReader message(fdInput);
    auto request = message.getRoot<CodeGeneratorRequest>();
    auto nodes = request.getNodes();

    /* Build lookup maps */
    std::map<uint64_t, std::string> nodeNames;
    std::map<uint64_t, Node::Reader> nodeById;
    for (auto node : nodes) {
        nodeNames[node.getId()] = shortName(node);
        nodeById[node.getId()] = node;
    }

    std::map<uint64_t, bool> requestedIds;
    for (auto rf : request.getRequestedFiles()) {
        requestedIds[rf.getId()] = true;
    }

    /* Collect structs and enums from requested files only */
    std::vector<Node::Reader> structNodes, enumNodes;
    for (auto node : nodes) {
        if (!isFromRequestedFile(node, requestedIds, nodeById)) continue;
        if (node.which() == Node::STRUCT) structNodes.push_back(node);
        else if (node.which() == Node::ENUM) enumNodes.push_back(node);
    }

    /* Emit JSON */
    FILE *out = stdout;
    fprintf(out, "{");

    /* File ID */
    if (request.getRequestedFiles().size() > 0) {
        fprintf(out, "\"fileId\":\"0x%016llx\",",
                (unsigned long long)request.getRequestedFiles()[0].getId());
    }

    /* Structs */
    fprintf(out, "\"structs\":[");
    for (size_t si = 0; si < structNodes.size(); si++) {
        auto node = structNodes[si];
        if (si > 0) fprintf(out, ",");
        fprintf(out, "{");
        fprintf(out, "\"name\":");
        jsonStr(out, nodeNames[node.getId()]);

        /* Struct annotations */
        fprintf(out, ",\"annotations\":");
        emitAnnotations(out, node.getAnnotations(), nodeNames);

        /* Fields */
        fprintf(out, ",\"fields\":[");
        auto structData = node.getStruct();
        auto fields = structData.getFields();
        bool firstField = true;
        for (auto field : fields) {
            if (field.which() != Field::SLOT) continue;
            if (!firstField) fprintf(out, ",");
            firstField = false;

            auto slot = field.getSlot();
            fprintf(out, "{");
            fprintf(out, "\"name\":");
            jsonStr(out, std::string(field.getName().cStr()));

            fprintf(out, ",\"ordinal\":%u", (unsigned)field.getOrdinal().isExplicit() ?
                    field.getOrdinal().getExplicit() : 0);

            fprintf(out, ",\"type\":");
            jsonStr(out, typeStr(slot.getType(), nodeNames));

            /* Default value */
            if (isNonTrivialDefault(slot.getDefaultValue())) {
                fprintf(out, ",\"default\":");
                auto dv = valueStr(slot.getDefaultValue());
                /* If it's a string-typed default, quote it */
                if (slot.getDefaultValue().which() == Value::TEXT) {
                    jsonStr(out, dv);
                } else {
                    fprintf(out, "%s", dv.c_str());
                }
            }

            /* Field annotations */
            fprintf(out, ",\"annotations\":");
            emitAnnotations(out, field.getAnnotations(), nodeNames);

            fprintf(out, "}");
        }
        fprintf(out, "]");
        fprintf(out, "}");
    }
    fprintf(out, "],");

    /* Enums */
    fprintf(out, "\"enums\":[");
    for (size_t ei = 0; ei < enumNodes.size(); ei++) {
        auto node = enumNodes[ei];
        if (ei > 0) fprintf(out, ",");
        fprintf(out, "{");
        fprintf(out, "\"name\":");
        jsonStr(out, nodeNames[node.getId()]);
        fprintf(out, ",\"enumerants\":[");
        auto enumData = node.getEnum();
        auto enumerants = enumData.getEnumerants();
        for (unsigned i = 0; i < enumerants.size(); i++) {
            if (i > 0) fprintf(out, ",");
            fprintf(out, "{\"name\":");
            jsonStr(out, std::string(enumerants[i].getName().cStr()));
            fprintf(out, ",\"ordinal\":%u}", i);
        }
        fprintf(out, "]}");
    }
    fprintf(out, "]");

    fprintf(out, "}\n");
    return 0;
}
