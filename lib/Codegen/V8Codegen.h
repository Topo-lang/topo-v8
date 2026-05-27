#ifndef TOPO_V8_CODEGEN_V8CODEGEN_H
#define TOPO_V8_CODEGEN_V8CODEGEN_H

#include "topo/Transpile/Emitter.h"
#include "topo/Sema/TypeBinder.h"

namespace topo::transpile {

// V8Codegen — AST → source emission for the V8 language family.
// Currently emits TypeScript (interface + type annotations). When JavaScript
// host support lands, a mode enum will select between ts-with-annotations and
// js-bare; for now the single mode is implicit.
class V8Codegen : public Emitter {
public:
    explicit V8Codegen(TypeBinder binder = TypeBinder::createDefault(HostLanguage::TypeScript));
    EmitResult emit(const TranspileModule& module) override;

private:
    TypeBinder binder_;

    std::string emitType(const TypeNode& type);
    std::string emitExpr(const Expr& expr);
    std::string emitStmt(const Stmt& stmt, int indent);
    std::string emitFunction(const TranspileFunction& func, int baseIndent = 0);
    std::string emitStruct(const TranspileType& type, int baseIndent = 0);
    std::string emitOwnership(const TypeNode& type);
};

} // namespace topo::transpile

#endif // TOPO_V8_CODEGEN_V8CODEGEN_H
