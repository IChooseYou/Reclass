#pragma once
#include "core.h"
#include <QString>
#include <QHash>
#include <QSet>

namespace rcx {

// ── Code output format ──

enum class CodeFormat : int {
    CppHeader = 0,    // C/C++ struct definitions
    RustStruct,       // Rust #[repr(C)] struct definitions
    DefineOffsets,    // #define ClassName_FieldName 0xNN
    CSharpStruct,     // C# [StructLayout] with [FieldOffset]
    PythonCtypes,     // Python ctypes.Structure
    _Count
};

enum class CodeScope : int {
    Current = 0,      // Just the selected struct
    WithChildren,     // Selected struct + all referenced types
    FullSdk,          // All root-level structs
    _Count
};

const char* codeFormatName(CodeFormat fmt);
const char* codeFormatFileFilter(CodeFormat fmt);
const char* codeScopeName(CodeScope scope);

// ── Format-aware dispatch (calls the appropriate backend) ──

QString renderCode(CodeFormat fmt, const NodeTree& tree, uint64_t rootStructId,
                   const QHash<NodeKind, QString>* typeAliases = nullptr,
                   bool emitAsserts = false);

// Render rootStructId + all struct types reachable from it
QString renderCodeTree(CodeFormat fmt, const NodeTree& tree, uint64_t rootStructId,
                       const QHash<NodeKind, QString>* typeAliases = nullptr,
                       bool emitAsserts = false);

QString renderCodeAll(CodeFormat fmt, const NodeTree& tree,
                      const QHash<NodeKind, QString>* typeAliases = nullptr,
                      bool emitAsserts = false);

// ── Individual backends ──

QString renderCpp(const NodeTree& tree, uint64_t rootStructId,
                  const QHash<NodeKind, QString>* typeAliases = nullptr,
                  bool emitAsserts = false);
QString renderCppTree(const NodeTree& tree, uint64_t rootStructId,
                      const QHash<NodeKind, QString>* typeAliases = nullptr,
                      bool emitAsserts = false);
QString renderCppAll(const NodeTree& tree,
                     const QHash<NodeKind, QString>* typeAliases = nullptr,
                     bool emitAsserts = false);

QString renderRust(const NodeTree& tree, uint64_t rootStructId,
                   const QHash<NodeKind, QString>* typeAliases = nullptr,
                   bool emitAsserts = false);
QString renderRustTree(const NodeTree& tree, uint64_t rootStructId,
                       const QHash<NodeKind, QString>* typeAliases = nullptr,
                       bool emitAsserts = false);
QString renderRustAll(const NodeTree& tree,
                      const QHash<NodeKind, QString>* typeAliases = nullptr,
                      bool emitAsserts = false);

QString renderDefines(const NodeTree& tree, uint64_t rootStructId);
QString renderDefinesTree(const NodeTree& tree, uint64_t rootStructId);
QString renderDefinesAll(const NodeTree& tree);

QString renderCSharp(const NodeTree& tree, uint64_t rootStructId,
                     const QHash<NodeKind, QString>* typeAliases = nullptr,
                     bool emitAsserts = false);
QString renderCSharpTree(const NodeTree& tree, uint64_t rootStructId,
                         const QHash<NodeKind, QString>* typeAliases = nullptr,
                         bool emitAsserts = false);
QString renderCSharpAll(const NodeTree& tree,
                        const QHash<NodeKind, QString>* typeAliases = nullptr,
                        bool emitAsserts = false);

QString renderPython(const NodeTree& tree, uint64_t rootStructId);
QString renderPythonTree(const NodeTree& tree, uint64_t rootStructId);
QString renderPythonAll(const NodeTree& tree);

// Null generator placeholder (returns empty string).
QString renderNull(const NodeTree& tree, uint64_t rootStructId);

} // namespace rcx
