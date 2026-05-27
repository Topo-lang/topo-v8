#ifndef TOPO_CHECK_TYPESCRIPTCALLEDGEEXTRACTOR_H
#define TOPO_CHECK_TYPESCRIPTCALLEDGEEXTRACTOR_H

#include "topo/Check/CallEdgeExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based TypeScript call edge extractor used by StageIsolationCheck
/// and VisibilityCheck.
///
/// Uses brace-depth scope tracking (mirrors TypeScriptSymbolExtractor).
/// Recognized caller forms that enter a function scope:
///   - `function NAME(...) { ... }`
///   - `class NAME { ... }`             (method bodies attribute to Class.method)
///   - `NAME = (args) => { ... }`       (arrow assigned to const/let/var)
///   - `NAME(args) { ... }`             (method shorthand inside class/object)
///   - `NAME: function(...) { ... }`    (object member)
///
/// Callee naming convention (mirror of Python):
///   - Bare call  `bar(...)`              → callee = "bar"
///   - Member call `obj.bar(...)`         → callee = "bar" (simple) + "obj::bar" (qualified)
///   - Chained    `a.b.c(...)`            → callee = "c" (simple) + "a::b::c" (qualified)
///   - `new Foo(...)`                     → callee = "Foo"
class TypeScriptCallEdgeExtractor : public CallEdgeExtractor {
public:
    std::vector<CallEdge> extractCallEdges(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_TYPESCRIPTCALLEDGEEXTRACTOR_H
