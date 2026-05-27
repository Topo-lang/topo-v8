// V8Codegen -- real TranspileModel -> TypeScript codegen.
//
// Mirrors PythonEmitter's scope and structure. Differences:
//   - brace-delimited blocks instead of indent-delimited
//   - C-style line comments (// [recovered] / // [inferred])
//   - `export class` + constructor for struct-like types
//   - `export function name(args): Ret { ... }` for free functions
//   - logical operators `&&` / `||` / `!` instead of Python `and`/`or`/`not`
//   - `null` for nullptr/None, `true`/`false` for booleans
//   - container mapping: vector<T> -> T[], map<K,V> -> Map<K,V>, optional<T> -> T | null
//
// stdlib bridging types: when TypeNode.isStdlib() is true,
// dispatch directly on stdlibId for the 6 first-batch types. i64 maps to
// `bigint` (not `number`) — the only place V8Codegen diverges from the legacy
// identifier-name primitive mapping. slice<T> renders as `readonly T[]` for
// simplicity; numeric typed-array specialization (Int32Array etc.) is deferred
// to a later batch where the perf vs. ergonomic trade-off is in scope.

#include "V8Codegen.h"
// Relocated from topo-lang-typescript/topo-transpile/TypeScriptEmitter.cpp.
// Class renamed to V8Codegen; behavior unchanged.
#include "topo/Stdlib/Types.h"
#include <cctype>
#include <functional>
#include <map>
#include <sstream>

namespace topo::transpile {

static std::string ind(int level) {
    return std::string(level * 4, ' ');
}

// TypeScript generic parameter list (`<T, U extends Bound>`). The MVP
// surfaces a single trait-style bound (TS `extends`) when the extractor
// populated constraintType, plus optional default `<T = X>` (legal on both
// classes and functions in TS, so the helper renders uniformly at every
// emit-site). Non-bound, no-default type params render as bare names so
// unbounded output stays byte-identical to pre-bounds emission. Defaults
// only render when the extractor populated TemplateParamDecl::defaultType.
// TypeScript has no equivalent to Rust associated-type bindings
// (`Iterator<Item = u8>`); the closest concept is mapped types on the type
// parameter side and is semantically distinct. DROP the bindings and
// append an inline `/* TOPO-TRANSPILE: ... */` block comment after the
// bound so the human-facing diff surfaces the loss. Block comments are
// legal inside `<T extends Iterator /* ... */>` in TypeScript.
static bool tsTypeNodeHasAssocBindings(const TypeNode& t) {
    return !t.assocBindings.empty();
}
static std::string tsAssocBindingDropNote(const std::string& paramName) {
    return " /* TOPO-TRANSPILE: associated-type bindings on " + paramName +
           " dropped (no TypeScript equivalent) */";
}

// Rust lifetime bounds (`T: 'a`) ride the wire as TypeNodes whose
// nameParts[0] starts with `'`. TS has no analogue — entries are silently
// dropped from any bound list (no comment, lifetime annotations are noise
// for non-Rust hosts).
static bool tsIsWireLifetimeBound(const TypeNode& t) {
    return !t.nameParts.empty() && !t.nameParts[0].empty() &&
           t.nameParts[0][0] == '\'';
}

static std::string tsGenericsImpl(const std::vector<TemplateParamDecl>& tpsIn,
                                  const std::function<std::string(const TypeNode&)>& renderType) {
    // Filter out kind=Lifetime entries up front.
    std::vector<TemplateParamDecl> tps;
    tps.reserve(tpsIn.size());
    for (const auto& p : tpsIn) {
        if (p.kind == TemplateParamDecl::LifetimeParam) continue;
        TemplateParamDecl q = p;
        if (q.kind == TemplateParamDecl::TypeParam &&
            !q.constraintType.nameParts.empty() &&
            tsIsWireLifetimeBound(q.constraintType)) {
            std::vector<TypeNode> kept;
            for (const auto& eb : q.extraBounds)
                if (!tsIsWireLifetimeBound(eb)) kept.push_back(eb);
            if (kept.empty()) {
                q.constraintType = TypeNode{};
                q.extraBounds.clear();
            } else {
                q.constraintType = kept.front();
                q.extraBounds.assign(kept.begin() + 1, kept.end());
            }
        } else if (q.kind == TemplateParamDecl::TypeParam) {
            std::vector<TypeNode> kept;
            for (const auto& eb : q.extraBounds)
                if (!tsIsWireLifetimeBound(eb)) kept.push_back(eb);
            q.extraBounds = std::move(kept);
        }
        tps.push_back(std::move(q));
    }
    if (tps.empty()) return "";
    std::string s = "<";
    for (size_t i = 0; i < tps.size(); ++i) {
        if (i > 0) s += ", ";
        s += tps[i].name;
        if (tps[i].kind == TemplateParamDecl::TypeParam &&
            !tps[i].constraintType.nameParts.empty()) {
            s += " extends " + renderType(tps[i].constraintType);
            // Intersection multi-bound: `<T extends A & B>`. extraBounds is
            // empty for single-bound payloads so the output stays
            // byte-identical to the legacy single-bound emission.
            bool anyAssoc = tsTypeNodeHasAssocBindings(tps[i].constraintType);
            for (const auto& eb : tps[i].extraBounds) {
                s += " & " + renderType(eb);
                if (tsTypeNodeHasAssocBindings(eb)) anyAssoc = true;
            }
            if (anyAssoc) s += tsAssocBindingDropNote(tps[i].name);
        }
        if (tps[i].kind == TemplateParamDecl::TypeParam &&
            tps[i].defaultType.has_value() &&
            !tps[i].defaultType->nameParts.empty()) {
            s += " = " + renderType(*tps[i].defaultType);
        }
    }
    s += ">";
    return s;
}

static std::string fidelityComment(Fidelity f, int level) {
    if (f == Fidelity::Recovered) return ind(level) + "// [recovered]\n";
    if (f == Fidelity::Inferred) return ind(level) + "// [inferred]\n";
    return "";
}

static std::string binaryOpStr(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Eq: return "===";
    case BinaryOp::NotEq: return "!==";
    case BinaryOp::Less: return "<";
    case BinaryOp::Greater: return ">";
    case BinaryOp::LessEq: return "<=";
    case BinaryOp::GreaterEq: return ">=";
    case BinaryOp::And: return "&&";
    case BinaryOp::Or: return "||";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    }
    return "??";
}

/// Map compound-assign operators to TypeScript syntax. Same as C-family.
static std::string compoundOpStr(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+=";
    case BinaryOp::Sub: return "-=";
    case BinaryOp::Mul: return "*=";
    case BinaryOp::Div: return "/=";
    case BinaryOp::Mod: return "%=";
    case BinaryOp::BitAnd: return "&=";
    case BinaryOp::BitOr: return "|=";
    case BinaryOp::BitXor: return "^=";
    case BinaryOp::Shl: return "<<=";
    case BinaryOp::Shr: return ">>=";
    default: return binaryOpStr(op) + "=";
    }
}

/// Map known C++/Rust/Java container type names to a tag that emitType can
/// render with TypeScript-specific template syntax. "array" means render as
/// `T[]`; other tags use `Tag<...>`.
static std::string mapContainerName(const std::string& name) {
    if (name == "vector" || name == "Vec" || name == "List" || name == "ArrayList" || name == "LinkedList" ||
        name == "list")
        return "array";
    if (name == "optional" || name == "Option" || name == "Optional") return "optional";
    if (name == "unordered_map" || name == "map" || name == "HashMap" || name == "TreeMap" || name == "Map" ||
        name == "dict")
        return "Map";
    if (name == "unordered_set" || name == "set" || name == "HashSet" || name == "TreeSet" || name == "Set")
        return "Set";
    if (name == "tuple" || name == "Tuple") return "tuple";
    return "";
}

/// Map known concrete type names directly to TypeScript types.
///
/// This is the fallback path taken when a TypeNode's `stdlibId` was not
/// populated (the canonical path goes through the per-`TypeId` switch in
/// `emitType` and is precision-correct). For 64-bit integers we still
/// emit `bigint` here — matching the stdlib path and the file-level
/// contract that "i64 maps to bigint (not number)" — because mapping
/// them to `number` would silently lose the upper 11 bits of precision
/// at the language boundary (IEEE-754 binary64 only carries 53 bits of
/// mantissa). 64-bit floats stay on `number`: IEEE-754 binary64 is the
/// host representation of `number`, so a JS `number` IS an `f64`.
static std::string mapPrimitiveType(const std::string& name) {
    // 64-bit integers -- TS `bigint` preserves the full range.
    if (name == "i64" || name == "u64" ||
        name == "int64_t" || name == "uint64_t")
        return "bigint";
    // Other integers / 32-and-64-bit floats -- TS `number` (IEEE-754 binary64)
    // is precision-safe for everything up to 53 bits, which covers
    // i32 / u32 / i16 / u16 / i8 / u8 and the legacy `int` / `long` / `short`
    // names. f64 and f32 are stored exactly as the host `number`.
    if (name == "int" || name == "int32_t" || name == "int16_t" || name == "int8_t" ||
        name == "uint32_t" || name == "uint16_t" || name == "uint8_t" || name == "i32" ||
        name == "i16" || name == "i8" || name == "u32" || name == "u16" ||
        name == "u8" || name == "size_t" || name == "usize" || name == "isize" || name == "long" ||
        name == "short" || name == "double" || name == "float" || name == "f64" || name == "f32")
        return "number";
    // Boolean
    if (name == "bool" || name == "boolean") return "boolean";
    // String
    if (name == "string" || name == "String" || name == "std::string" || name == "str") return "string";
    // Void
    if (name == "void" || name == "Void") return "void";
    return "";
}

static std::pair<std::string, std::string> splitQualifiedName(const std::string& qname) {
    auto pos = qname.rfind("::");
    if (pos == std::string::npos)
        return {"", qname};
    return {qname.substr(0, pos), qname.substr(pos + 2)};
}

static std::string capitalize(const std::string& s) {
    if (s.empty()) return s;
    std::string result = s;
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

V8Codegen::V8Codegen(TypeBinder binder) : binder_(std::move(binder)) {}

EmitResult V8Codegen::emit(const TranspileModule& module) {
    EmitResult result;

    // Group types and functions by namespace. Empty namespace goes at module
    // scope; non-empty namespaces are emitted as `export namespace <Ns> { ... }`.
    struct NsGroup {
        std::vector<const TranspileType*> types;
        std::vector<const TranspileFunction*> functions;
    };
    std::map<std::string, NsGroup> groups;

    for (const auto& t : module.types) {
        auto [ns, _] = splitQualifiedName(t.qualifiedName);
        groups[ns].types.push_back(&t);
    }
    for (const auto& f : module.functions) {
        auto [ns, _] = splitQualifiedName(f.qualifiedName);
        groups[ns].functions.push_back(&f);
    }

    for (const auto& [ns, group] : groups) {
        bool inNamespace = !ns.empty();
        if (inNamespace) {
            auto lastSep = ns.rfind("::");
            std::string nsName = capitalize(lastSep == std::string::npos ? ns : ns.substr(lastSep + 2));
            result.code += "export namespace " + nsName + " {\n";
        }

        int baseIndent = inNamespace ? 1 : 0;

        for (const auto* t : group.types)
            result.code += emitStruct(*t, baseIndent) + "\n";
        for (const auto* f : group.functions)
            result.code += emitFunction(*f, baseIndent) + "\n";

        if (inNamespace)
            result.code += "}\n";
    }

    return result;
}

std::string V8Codegen::emitOwnership(const TypeNode& type) {
    // TypeScript lacks ownership semantics -- render the underlying type,
    // attaching an annotation comment so the loss is visible to readers.
    // Copy-and-mutate, not positional reconstruction: a positional
    // TypeNode{...} silently drops any field not listed (stdlibId,
    // recordFields), so `owned slice<T>` / `owned record<...>` would lose
    // their stdlib identity through the ownership path.
    TypeNode bare = type;
    bare.ownership = OwnershipKind::None;
    bare.modifier = TypeNode::None;
    std::string inner = emitType(bare);

    switch (type.ownership) {
    case OwnershipKind::Owned: return inner + " /* [owned] */";
    case OwnershipKind::Shared: return inner + " /* [shared] */";
    case OwnershipKind::Weak: return "(" + inner + " | null) /* [weak] */";
    case OwnershipKind::None: break;
    }
    return inner;
}

std::string V8Codegen::emitType(const TypeNode& type) {
    if (type.ownership != OwnershipKind::None) return emitOwnership(type);

    // Helper: render template args joined with ", "
    auto renderArgs = [&](const std::vector<TypeNode>& args) {
        std::string out;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) out += ", ";
            out += emitType(args[i]);
        }
        return out;
    };

    // stdlib bridging types take priority over the legacy
    // primitive/container name lookups so that i64 -> bigint stays correct
    // even when a TypeNode also has nameParts={"i64"} (parser sets both).
    if (type.isStdlib()) {
        switch (type.stdlibId) {
        case stdlib::TypeId::Bool: return "boolean";
        case stdlib::TypeId::I64: return "bigint";
        case stdlib::TypeId::TimeNs: return "bigint"; // ns since epoch; i64 range exceeds JS number safe int
        case stdlib::TypeId::Uuid: return "Uint8Array"; // 16-byte RFC 4122 buffer (byte-faithful, not canonical string)
        case stdlib::TypeId::Decimal128: return "Uint8Array"; // 16-byte IEEE 754-2008 buffer (no native JS decimal)
        case stdlib::TypeId::F64: return "number";
        case stdlib::TypeId::String: return "string";
        case stdlib::TypeId::Optional: {
            if (type.templateArgs.empty()) return "unknown | null";
            return emitType(type.templateArgs[0]) + " | null";
        }
        case stdlib::TypeId::Slice: {
            if (type.templateArgs.empty()) return "readonly unknown[]";
            return "readonly " + emitType(type.templateArgs[0]) + "[]";
        }
        // `bytes` is slice<u8>-isomorphic: emit exactly what slice<u8>
        // emits. slice<T> -> `readonly T[]`, u8 -> `number`, so the
        // observable mapping is `readonly number[]`. Kept literal (not a
        // recursive slice<u8> synthesis) so the contract is visible here.
        case stdlib::TypeId::Bytes: return "readonly number[]";
        // array<T, N> is a fixed-length inline buffer. Element type T is
        // templateArgs[0] (recurse like slice). N is the integer literal in
        // templateArgs[1].nonTypeValue. TS expresses a fixed length most
        // precisely as a readonly tuple [T, T, ... x N], which (unlike
        // slice's `readonly T[]`) preserves the distinguishing length. When
        // N is unknown/absent fall back to slice's `readonly T[]` form so
        // the two stay consistent for the degenerate case.
        case stdlib::TypeId::Array: {
            std::string elem = type.templateArgs.empty()
                                   ? "unknown"
                                   : emitType(type.templateArgs[0]);
            std::optional<int> n;
            if (type.templateArgs.size() >= 2)
                n = type.templateArgs[1].nonTypeValue;
            if (!n && type.nonTypeValue) n = type.nonTypeValue;
            if (!n || *n < 0) return "readonly " + elem + "[]";
            std::string out = "readonly [";
            for (int i = 0; i < *n; ++i) {
                if (i > 0) out += ", ";
                out += elem;
            }
            out += "]";
            return out;
        }
        // JS `number` is IEEE-754 binary64 so integers up to
        // 2^53-1 are lossless. u8/i32/u32/f32 fit inside `number`; u64 must use
        // `bigint` to preserve full 64-bit range (same rationale as i64).
        case stdlib::TypeId::U8:  return "number";
        case stdlib::TypeId::I32: return "number";
        case stdlib::TypeId::U32: return "number";
        case stdlib::TypeId::U64: return "bigint";
        case stdlib::TypeId::F32: return "number";
        case stdlib::TypeId::I8:  return "number";
        case stdlib::TypeId::I16: return "number";
        case stdlib::TypeId::U16: return "number";
        case stdlib::TypeId::Record: {
            // record<f1: T1, ...> -> readonly tuple [T1, T2, ...]. Field
            // order is the load-bearing cross-language byte contract; field
            // names live in the .topo declaration, not the host type (same
            // positional idiom PythonEmitter uses, and consistent with how
            // array<T,N> renders as a readonly tuple here). Each field type
            // recurses so nested stdlib types compose.
            const auto& fields = type.recordFields;
            if (fields.empty()) return "readonly []"; // defensive; Sema rejects record<> upstream
            std::string out = "readonly [";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i > 0) out += ", ";
                out += emitType(fields[i].type());
            }
            out += "]";
            return out;
        }
        case stdlib::TypeId::Union: {
            // union<tag: TagT, v1: T1, ...> -> readonly tuple
            // [TagT, T1, ...]. TypeScript has no anonymous tagged-union
            // literal that pins a byte layout; the order-preserving readonly
            // tuple is the faithful surface — the same idiom record uses.
            // The .topo declaration owns the field names and the
            // variant-overlap semantics (only the tag-selected variant
            // occupies the shared storage); the tuple necessarily widens to
            // carry tag plus every possible variant slot.
            const auto& fields = type.recordFields;
            if (fields.empty()) return "readonly []"; // defensive; Sema rejects upstream
            std::string out = "readonly [";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i > 0) out += ", ";
                out += emitType(fields[i].type());
            }
            out += "]";
            return out;
        }
        case stdlib::TypeId::None: break; // fall through to legacy paths
        }
    }

    // Helper: apply container rendering for a known container tag
    auto renderContainer = [&](const std::string& tag, const std::vector<TypeNode>& args) -> std::string {
        if (tag == "array") {
            if (args.empty()) return "unknown[]";
            return emitType(args[0]) + "[]";
        }
        if (tag == "optional") {
            if (args.empty()) return "unknown | null";
            return emitType(args[0]) + " | null";
        }
        if (tag == "tuple") {
            return "[" + renderArgs(args) + "]";
        }
        // Map / Set -- use TS generic syntax
        if (args.empty()) return tag;
        return tag + "<" + renderArgs(args) + ">";
    };

    // `union<A, B, ...>` carried positionally in templateArgs (the form a
    // Python `TypeVar('T', int, str)` constraint tuple lowers to) renders
    // as a native TypeScript union `A | B | ...` — `T` is exactly one of
    // the listed types. This is the untagged member-choice sense, distinct
    // from the stdlib *tagged* `union<tag: …, v1: …>` whose discriminant +
    // named variant fields ride `recordFields` (handled above when the
    // `stdlibId` is set). A wire-loaded node has `stdlibId == None`, so it
    // reaches here; matching on `nameParts` keeps the two senses apart.
    if (type.nameParts.size() == 1 && type.nameParts[0] == "union" &&
        !type.templateArgs.empty()) {
        std::string out;
        for (size_t i = 0; i < type.templateArgs.size(); ++i) {
            if (i > 0) out += " | ";
            out += emitType(type.templateArgs[i]);
        }
        return out;
    }

    // Try TypeBinder resolution for single-part abstract names (integer, text, ...)
    if (type.nameParts.size() == 1) {
        auto resolved = binder_.resolve(type.nameParts[0]);
        if (resolved) return *resolved;
    }

    // Single-part name: primitive mapping
    if (type.nameParts.size() == 1) {
        auto prim = mapPrimitiveType(type.nameParts[0]);
        if (!prim.empty()) {
            if (!type.templateArgs.empty())
                return prim + "<" + renderArgs(type.templateArgs) + ">";
            return prim;
        }
    }

    // Single-part container mapping (e.g. vector<T> -> T[])
    if (type.nameParts.size() == 1) {
        auto tag = mapContainerName(type.nameParts[0]);
        if (!tag.empty()) return renderContainer(tag, type.templateArgs);
    }

    // Qualified names: check last part for container/primitive
    if (type.nameParts.size() > 1) {
        const auto& lastName = type.nameParts.back();
        auto tag = mapContainerName(lastName);
        if (!tag.empty()) return renderContainer(tag, type.templateArgs);

        auto prim = mapPrimitiveType(lastName);
        if (!prim.empty()) return prim;
    }

    // Fallback: emit as dot-separated name (TS namespace path)
    std::string result;
    for (size_t i = 0; i < type.nameParts.size(); ++i) {
        if (i > 0) result += ".";
        result += type.nameParts[i];
    }

    if (!type.templateArgs.empty())
        result += "<" + renderArgs(type.templateArgs) + ">";

    // TypeScript ignores Ref/Ptr modifiers and const
    return result;
}

std::string V8Codegen::emitExpr(const Expr& expr) {
    switch (expr.kind()) {
    case Expr::Kind::BinaryOp: {
        const auto& e = static_cast<const BinaryOpExpr&>(expr);
        return "(" + emitExpr(*e.lhs) + " " + binaryOpStr(e.op) + " " + emitExpr(*e.rhs) + ")";
    }
    case Expr::Kind::UnaryOp: {
        const auto& e = static_cast<const UnaryOpExpr&>(expr);
        std::string op;
        switch (e.op) {
        case UnaryOp::Negate: op = "-"; break;
        case UnaryOp::Not: op = "!"; break;
        case UnaryOp::BitNot: op = "~"; break;
        case UnaryOp::PreIncrement: return "++" + emitExpr(*e.operand);
        case UnaryOp::PostIncrement: return emitExpr(*e.operand) + "++";
        case UnaryOp::PreDecrement: return "--" + emitExpr(*e.operand);
        case UnaryOp::PostDecrement: return emitExpr(*e.operand) + "--";
        }
        return op + emitExpr(*e.operand);
    }
    case Expr::Kind::Call: {
        const auto& e = static_cast<const CallExpr&>(expr);
        std::string result = e.callee + "(";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitExpr(*e.args[i]);
        }
        result += ")";
        return result;
    }
    case Expr::Kind::MemberAccess: {
        const auto& e = static_cast<const MemberAccessExpr&>(expr);
        return emitExpr(*e.object) + "." + e.member;
    }
    case Expr::Kind::Index: {
        const auto& e = static_cast<const IndexExpr&>(expr);
        return emitExpr(*e.object) + "[" + emitExpr(*e.index) + "]";
    }
    case Expr::Kind::Literal: {
        const auto& e = static_cast<const LiteralExpr&>(expr);
        if (e.litKind == LiteralKind::String) return "\"" + e.value + "\"";
        if (e.litKind == LiteralKind::Boolean) return (e.value == "true") ? "true" : "false";
        return e.value;
    }
    case Expr::Kind::VarRef: {
        const auto& e = static_cast<const VarRefExpr&>(expr);
        // Map null/nullptr/None to TypeScript null
        if (e.name == "null" || e.name == "nullptr" || e.name == "None") return "null";
        if (e.name == "true" || e.name == "True") return "true";
        if (e.name == "false" || e.name == "False") return "false";
        return e.name;
    }
    case Expr::Kind::Construct: {
        const auto& e = static_cast<const ConstructExpr&>(expr);
        std::string result = "new " + emitType(e.type) + "(";
        for (size_t i = 0; i < e.args.size(); ++i) {
            if (i > 0) result += ", ";
            result += emitExpr(*e.args[i]);
        }
        result += ")";
        return result;
    }
    case Expr::Kind::Lambda: {
        const auto& e = static_cast<const LambdaExpr&>(expr);
        // TS arrow function: (p: T, ...) => body-or-block
        std::string result = "(";
        for (size_t i = 0; i < e.params.size(); ++i) {
            if (i > 0) result += ", ";
            result += e.params[i].name;
            if (!e.params[i].type.nameParts.empty())
                result += ": " + emitType(e.params[i].type);
        }
        result += ")";
        if (!e.returnType.nameParts.empty())
            result += ": " + emitType(e.returnType);
        result += " => ";

        // Single return-expression lambdas collapse to arrow-with-expr
        if (e.body.size() == 1 && e.body[0]->kind() == Stmt::Kind::Return) {
            const auto& ret = static_cast<const ReturnStmt&>(*e.body[0]);
            if (ret.value) {
                result += emitExpr(*ret.value);
                return result;
            }
        }

        result += "{\n";
        for (const auto& st : e.body)
            result += emitStmt(*st, 1);
        result += "}";
        return result;
    }
    case Expr::Kind::Throw: {
        const auto& e = static_cast<const ThrowExpr&>(expr);
        return "(() => { throw " + emitExpr(*e.operand) + "; })()";
    }
    case Expr::Kind::Unsupported: {
        const auto& e = static_cast<const UnsupportedExpr&>(expr);
        return "/* TOPO-TRANSPILE: unsupported -- " + e.description + " */ null";
    }
    case Expr::Kind::Ternary: {
        const auto& e = static_cast<const TernaryExpr&>(expr);
        return "(" + emitExpr(*e.condition) + " ? " + emitExpr(*e.trueExpr) + " : " + emitExpr(*e.falseExpr) + ")";
    }
    case Expr::Kind::CompoundAssign: {
        const auto& e = static_cast<const CompoundAssignExpr&>(expr);
        return emitExpr(*e.target) + " " + compoundOpStr(e.op) + " " + emitExpr(*e.value);
    }
    }
    return "/* TOPO-TRANSPILE: unsupported -- unknown expression */ null";
}

std::string V8Codegen::emitStmt(const Stmt& stmt, int level) {
    std::string prefix = fidelityComment(stmt.fidelity, level);

    switch (stmt.kind()) {
    case Stmt::Kind::VarDecl: {
        const auto& s = static_cast<const VarDeclStmt&>(stmt);
        std::string result = prefix + ind(level) + "let " + s.name;
        if (!s.type.nameParts.empty()) result += ": " + emitType(s.type);
        if (s.init) result += " = " + emitExpr(*s.init);
        result += ";\n";
        return result;
    }
    case Stmt::Kind::Assign: {
        const auto& s = static_cast<const AssignStmt&>(stmt);
        return prefix + ind(level) + emitExpr(*s.target) + " = " + emitExpr(*s.value) + ";\n";
    }
    case Stmt::Kind::Return: {
        const auto& s = static_cast<const ReturnStmt&>(stmt);
        if (s.value) return prefix + ind(level) + "return " + emitExpr(*s.value) + ";\n";
        return prefix + ind(level) + "return;\n";
    }
    case Stmt::Kind::If: {
        const auto& s = static_cast<const IfStmt&>(stmt);
        std::string result = prefix + ind(level) + "if (" + emitExpr(*s.condition) + ") {\n";
        for (const auto& st : s.thenBody)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}";
        if (!s.elseBody.empty()) {
            // Collapse `else { if (...) { ... } }` into `else if (...)` chains
            if (s.elseBody.size() == 1 && s.elseBody[0]->kind() == Stmt::Kind::If) {
                const auto& elseIf = static_cast<const IfStmt&>(*s.elseBody[0]);
                // Emit inline `else <if-stmt>` by recursively emitting the
                // nested if and stripping its leading indent.
                std::string nested = emitStmt(elseIf, level);
                // strip leading indent (ind(level))
                std::string leading = ind(level);
                if (nested.rfind(leading, 0) == 0)
                    nested.erase(0, leading.size());
                // drop trailing newline so we can append cleanly
                while (!nested.empty() && nested.back() == '\n') nested.pop_back();
                result += " else " + nested + "\n";
                return result;
            }
            result += " else {\n";
            for (const auto& st : s.elseBody)
                result += emitStmt(*st, level + 1);
            result += ind(level) + "}";
        }
        result += "\n";
        return result;
    }
    case Stmt::Kind::For: {
        const auto& s = static_cast<const ForStmt&>(stmt);
        // Emit a C-style for-loop directly; TS supports the same form as Java.
        std::string init;
        if (s.init) {
            std::string raw = emitStmt(*s.init, 0);
            size_t start = raw.find_first_not_of(" \t\n");
            if (start != std::string::npos) raw = raw.substr(start);
            while (!raw.empty() && (raw.back() == '\n' || raw.back() == ';' || raw.back() == ' '))
                raw.pop_back();
            init = raw;
        }
        std::string cond = s.condition ? emitExpr(*s.condition) : "";
        std::string incr = s.increment ? emitExpr(*s.increment) : "";

        std::string result = prefix + ind(level) + "for (" + init + "; " + cond + "; " + incr + ") {\n";
        for (const auto& st : s.body)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}\n";
        return result;
    }
    case Stmt::Kind::While: {
        const auto& s = static_cast<const WhileStmt&>(stmt);
        std::string result = prefix + ind(level) + "while (" + emitExpr(*s.condition) + ") {\n";
        for (const auto& st : s.body)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}\n";
        return result;
    }
    case Stmt::Kind::ExprStmt: {
        const auto& s = static_cast<const ExprStmt&>(stmt);
        return prefix + ind(level) + emitExpr(*s.expr) + ";\n";
    }
    case Stmt::Kind::TryCatch: {
        const auto& s = static_cast<const TryCatchStmt&>(stmt);
        std::string result = prefix + ind(level) + "try {\n";
        for (const auto& st : s.tryBody)
            result += emitStmt(*st, level + 1);
        result += ind(level) + "}";
        // TS supports only a single catch clause. Render the first clause as
        // the catch; additional clauses are folded into a comment as a
        // partial-fidelity marker.
        if (!s.catchClauses.empty()) {
            const auto& c = s.catchClauses.front();
            result += " catch";
            if (!c.varName.empty()) {
                result += " (" + c.varName;
                if (!c.exceptionType.nameParts.empty())
                    result += ": " + emitType(c.exceptionType);
                result += ")";
            }
            result += " {\n";
            for (const auto& st : c.body)
                result += emitStmt(*st, level + 1);
            result += ind(level) + "}";
            if (s.catchClauses.size() > 1) {
                result += " /* TOPO-TRANSPILE: " + std::to_string(s.catchClauses.size() - 1) +
                          " additional catch clause(s) merged into first */";
            }
        }
        if (!s.finallyBody.empty()) {
            result += " finally {\n";
            for (const auto& st : s.finallyBody)
                result += emitStmt(*st, level + 1);
            result += ind(level) + "}";
        }
        result += "\n";
        return result;
    }
    case Stmt::Kind::Break: return prefix + ind(level) + "break;\n";
    case Stmt::Kind::Continue: return prefix + ind(level) + "continue;\n";
    case Stmt::Kind::Switch: {
        const auto& s = static_cast<const SwitchStmt&>(stmt);
        std::string result = prefix + ind(level) + "switch (" + emitExpr(*s.subject) + ") {\n";
        for (const auto& c : s.cases) {
            if (c.value)
                result += ind(level + 1) + "case " + emitExpr(*c.value) + ":\n";
            else
                result += ind(level + 1) + "default:\n";
            for (const auto& st : c.body)
                result += emitStmt(*st, level + 2);
        }
        result += ind(level) + "}\n";
        return result;
    }
    }
    return prefix + ind(level) + "// TOPO-TRANSPILE: unsupported -- unknown statement\n";
}

std::string V8Codegen::emitFunction(const TranspileFunction& func, int baseIndent) {
    std::string result;
    result += fidelityComment(func.fidelity, baseIndent);

    for (const auto& u : func.unsupported)
        result += ind(baseIndent) + "// TOPO-TRANSPILE: unsupported -- " + u + "\n";

    auto [_, simpleName] = splitQualifiedName(func.qualifiedName);
    result += ind(baseIndent) + "export function " + simpleName +
              tsGenericsImpl(func.templateParams,
                             [this](const TypeNode& t) { return emitType(t); }) +
              "(";
    for (size_t i = 0; i < func.params.size(); ++i) {
        if (i > 0) result += ", ";
        result += func.params[i].name;
        if (!func.params[i].type.nameParts.empty())
            result += ": " + emitType(func.params[i].type);
    }
    result += ")";

    // Return type annotation -- render explicitly when available
    if (!func.returnType.nameParts.empty())
        result += ": " + emitType(func.returnType);

    result += " {\n";

    for (const auto& s : func.body)
        result += emitStmt(*s, baseIndent + 1);

    result += ind(baseIndent) + "}\n";
    return result;
}

std::string V8Codegen::emitStruct(const TranspileType& type, int baseIndent) {
    std::string result;
    result += fidelityComment(type.fidelity, baseIndent);
    auto [_, simpleName] = splitQualifiedName(type.qualifiedName);
    result += ind(baseIndent) + "export class " + simpleName +
              tsGenericsImpl(type.templateParams,
                             [this](const TypeNode& t) { return emitType(t); });

    // Inheritance — TypeScript shape mirrors Java exactly:
    //   `class C extends Base implements I1, I2 { ... }`
    // When baseClassKinds is supplied (parallel-length array of Class/Interface
    // tags) the placement is exact: the at-most-one Class base goes after
    // `extends`, every Interface base after `implements`. Without the
    // discriminator (empty kinds) fall back to the legacy heuristic — first
    // base = extends, rest = implements — so pre-discriminator payloads stay
    // byte-identical. Empty baseClasses ⇒ no clause, byte-identical to the
    // pre-inheritance emission.
    if (!type.baseClasses.empty()) {
        const bool haveKinds = type.baseClassKinds.size() == type.baseClasses.size();
        if (haveKinds) {
            std::string extendsTarget;
            std::vector<std::string> implementsList;
            for (size_t i = 0; i < type.baseClasses.size(); ++i) {
                if (type.baseClassKinds[i] == BaseClassKind::Class) {
                    extendsTarget = emitType(type.baseClasses[i]);
                } else {
                    implementsList.push_back(emitType(type.baseClasses[i]));
                }
            }
            if (!extendsTarget.empty()) result += " extends " + extendsTarget;
            if (!implementsList.empty()) {
                result += " implements ";
                for (size_t i = 0; i < implementsList.size(); ++i) {
                    if (i > 0) result += ", ";
                    result += implementsList[i];
                }
            }
        } else {
            result += " extends " + emitType(type.baseClasses[0]);
            if (type.baseClasses.size() > 1) {
                result += " implements ";
                for (size_t i = 1; i < type.baseClasses.size(); ++i) {
                    if (i > 1) result += ", ";
                    result += emitType(type.baseClasses[i]);
                }
            }
        }
    }
    result += " {\n";

    if (type.fields.empty()) {
        // Empty class -- emit an empty body. TS permits `class Foo {}` fine.
    } else {
        // Field declarations
        for (const auto& f : type.fields) {
            result += fidelityComment(f.fidelity, baseIndent + 1);
            result += ind(baseIndent + 1) + f.name + ": " + emitType(f.type) + ";\n";
        }

        // Constructor that accepts each field positionally, matching the
        // dataclass-like ergonomics of PythonEmitter's @dataclass output.
        result += ind(baseIndent + 1) + "constructor(";
        for (size_t i = 0; i < type.fields.size(); ++i) {
            if (i > 0) result += ", ";
            result += type.fields[i].name + ": " + emitType(type.fields[i].type);
        }
        result += ") {\n";
        for (const auto& f : type.fields)
            result += ind(baseIndent + 2) + "this." + f.name + " = " + f.name + ";\n";
        result += ind(baseIndent + 1) + "}\n";
    }

    result += ind(baseIndent) + "}\n";
    return result;
}

} // namespace topo::transpile
