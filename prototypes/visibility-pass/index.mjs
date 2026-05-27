// VisibilityPass AST-level prototype.
// Inputs: .ts source text, visibility map { name -> "public" | "internal" | "private" }.
// Output: rewritten .ts source text. Pure transform; no I/O side effects.

import ts from "typescript";

const INTERNAL_DOC = "*\n * @internal\n ";

function declaredName(node) {
    if (ts.isFunctionDeclaration(node) || ts.isClassDeclaration(node) ||
        ts.isInterfaceDeclaration(node) || ts.isTypeAliasDeclaration(node) ||
        ts.isEnumDeclaration(node) || ts.isModuleDeclaration(node)) {
        return node.name?.getText();
    }
    if (ts.isVariableStatement(node)) {
        // Single-binding `export const NAME = ...` only — multi-binding
        // declarations are left alone in this prototype.
        const decls = node.declarationList.declarations;
        if (decls.length === 1 && ts.isIdentifier(decls[0].name)) {
            return decls[0].name.text;
        }
    }
    return undefined;
}

function withModifiers(node, mods, factory) {
    if (ts.isFunctionDeclaration(node)) {
        return factory.updateFunctionDeclaration(node, mods, node.asteriskToken,
            node.name, node.typeParameters, node.parameters, node.type, node.body);
    }
    if (ts.isClassDeclaration(node)) {
        return factory.updateClassDeclaration(node, mods, node.name,
            node.typeParameters, node.heritageClauses, node.members);
    }
    if (ts.isInterfaceDeclaration(node)) {
        return factory.updateInterfaceDeclaration(node, mods, node.name,
            node.typeParameters, node.heritageClauses, node.members);
    }
    if (ts.isTypeAliasDeclaration(node)) {
        return factory.updateTypeAliasDeclaration(node, mods, node.name,
            node.typeParameters, node.type);
    }
    if (ts.isEnumDeclaration(node)) {
        return factory.updateEnumDeclaration(node, mods, node.name, node.members);
    }
    if (ts.isVariableStatement(node)) {
        return factory.updateVariableStatement(node, mods, node.declarationList);
    }
    return node;
}

function withoutExport(modifiers, factory) {
    if (!modifiers) return undefined;
    const filtered = modifiers.filter(m =>
        m.kind !== ts.SyntaxKind.ExportKeyword &&
        m.kind !== ts.SyntaxKind.DefaultKeyword);
    return filtered.length ? filtered : undefined;
}

function attachInternalDoc(node, factory) {
    // Prepend a /** @internal */ JSDoc as a synthetic leading comment that
    // ts.createPrinter will emit in front of the node.
    ts.setSyntheticLeadingComments(node, [
        {
            kind: ts.SyntaxKind.MultiLineCommentTrivia,
            text: INTERNAL_DOC,
            hasTrailingNewLine: true,
            pos: -1, end: -1,
        },
    ]);
    return node;
}

export function rewrite(sourceText, visibilityMap, fileName = "input.ts") {
    const source = ts.createSourceFile(fileName, sourceText,
        ts.ScriptTarget.ES2022, /*setParentNodes*/ true);

    const transformer = (context) => (root) => {
        const factory = context.factory;
        const visit = (node) => {
            // Only act on top-level declarations of the source file.
            if (node.parent && node.parent.kind === ts.SyntaxKind.SourceFile) {
                const name = declaredName(node);
                const action = name ? visibilityMap[name] : undefined;
                if (action) {
                    const isExported = (node.modifiers ?? []).some(
                        m => m.kind === ts.SyntaxKind.ExportKeyword);
                    if (action === "private" && isExported) {
                        const newMods = withoutExport(node.modifiers, factory);
                        return withModifiers(node, newMods, factory);
                    }
                    if (action === "internal") {
                        return attachInternalDoc(node, factory);
                    }
                    // "public": leave as-is.
                }
            }
            return node;
        };
        return ts.visitEachChild(root, visit, context);
    };

    const result = ts.transform(source, [transformer]);
    const printer = ts.createPrinter({ newLine: ts.NewLineKind.LineFeed });
    const out = printer.printFile(result.transformed[0]);
    result.dispose();
    return out;
}
