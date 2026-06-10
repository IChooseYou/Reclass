#pragma once
// Syntax highlighting for the generated-code ("Code" tab) views.
//
// The view used to attach a bare QsciLexerCPP for every output language and
// never populate keyword-set 2, so primitive TYPE names (uint8_t, i64, u32,
// c_uint8, …) rendered as plain identifiers and non-C++ languages got C++
// rules. This centralizes per-language lexer selection + theming:
//   - C++ / Rust / #define  → RcxCodeLexer (QsciLexerCPP + type set 2 + a few
//                              Rust/C# keywords appended to set 1)
//   - C#                     → QsciLexerCSharp (shares the CPP style enums)
//   - Python (ctypes)        → RcxPyLexer (QsciLexerPython + ctypes type set 2)
//
// applyCodeLexer() picks the lexer for a CodeFormat and themes every style
// from the active Theme. It only swaps the lexer when the class actually
// changes, so it's cheap to call on every render.
#include <string>
#include <QFont>
#include <QColor>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexerpython.h>
#include "themes/theme.h"
#include "generator.h"   // CodeFormat

namespace rcx {

// QsciLexerCPP with RE primitive types in keyword set 2 (→ KeywordSet2 style,
// coloured as a type) and Rust/C# keywords appended to set 1 (→ Keyword style)
// so `pub fn let mut`, `sealed override`, etc. highlight in those outputs too.
class RcxCodeLexer : public QsciLexerCPP {
public:
    explicit RcxCodeLexer(QObject* parent = nullptr) : QsciLexerCPP(parent) {}

    const char* keywords(int set) const override {
        if (set == 1) {
            // Base C++ keywords + extra Rust/C# ones (append, don't replace,
            // so no C++ keyword is lost). Built once.
            static const std::string kw = [] {
                QsciLexerCPP base;
                const char* c = base.keywords(1);
                std::string s = c ? c : "";
                s += " pub fn let mut impl mod use dyn match move unsafe where "
                     "crate trait async await ref "
                     "abstract internal sealed partial readonly override params "
                     "base lock out is var";
                return s;
            }();
            return kw.c_str();
        }
        if (set == 2) {
            // Primitive/RE type names across C/C++/Rust/C#.
            return
                "int8_t int16_t int32_t int64_t int128_t "
                "uint8_t uint16_t uint32_t uint64_t uint128_t "
                "size_t ssize_t intptr_t uintptr_t ptrdiff_t wchar_t "
                "float double bool char char8_t char16_t char32_t void "
                "short int long signed unsigned "
                "u8 u16 u32 u64 u128 usize i8 i16 i32 i64 i128 isize f32 f64 "
                "byte sbyte ushort uint ulong nint nuint string object decimal";
        }
        return QsciLexerCPP::keywords(set);
    }
};

// QsciLexerPython with ctypes type names in keyword set 2 (→ HighlightedIdentifier).
class RcxPyLexer : public QsciLexerPython {
public:
    explicit RcxPyLexer(QObject* parent = nullptr) : QsciLexerPython(parent) {}

    const char* keywords(int set) const override {
        if (set == 2) {
            return
                "c_int8 c_uint8 c_int16 c_uint16 c_int32 c_uint32 c_int64 c_uint64 "
                "c_byte c_ubyte c_short c_ushort c_int c_uint c_long c_ulong "
                "c_longlong c_ulonglong c_float c_double c_bool c_char c_wchar "
                "c_char_p c_wchar_p c_void_p c_size_t c_ssize_t "
                "Structure Union LittleEndianStructure BigEndianStructure "
                "ctypes Array POINTER";
        }
        return QsciLexerPython::keywords(set);
    }
};

// Theme a CPP-family lexer (QsciLexerCPP / QsciLexerCSharp / RcxCodeLexer).
inline void themeCppFamilyLexer(QsciLexerCPP* l, const Theme& t,
                                const QFont& f, const QColor& paper) {
    if (!l) return;
    l->setFont(f);
    l->setColor(t.syntaxKeyword, QsciLexerCPP::Keyword);
    l->setColor(t.syntaxType,    QsciLexerCPP::KeywordSet2);
    l->setColor(t.syntaxType,    QsciLexerCPP::GlobalClass);
    l->setColor(t.syntaxNumber,  QsciLexerCPP::Number);
    l->setColor(t.syntaxString,  QsciLexerCPP::DoubleQuotedString);
    l->setColor(t.syntaxString,  QsciLexerCPP::SingleQuotedString);
    l->setColor(t.syntaxString,  QsciLexerCPP::VerbatimString);
    l->setColor(t.syntaxComment, QsciLexerCPP::Comment);
    l->setColor(t.syntaxComment, QsciLexerCPP::CommentLine);
    l->setColor(t.syntaxComment, QsciLexerCPP::CommentDoc);
    l->setColor(t.syntaxComment, QsciLexerCPP::CommentLineDoc);
    l->setColor(t.text,          QsciLexerCPP::Default);
    l->setColor(t.text,          QsciLexerCPP::Identifier);
    l->setColor(t.syntaxPreproc, QsciLexerCPP::PreProcessor);
    l->setColor(t.text,          QsciLexerCPP::Operator);
    for (int i = 0; i <= 127; ++i) { l->setPaper(paper, i); l->setFont(f, i); }
}

// Select + theme the lexer for `fmt` on `sci`. Swaps the lexer only when the
// class changes; re-themes every call (cheap).
inline void applyCodeLexer(QsciScintilla* sci, CodeFormat fmt,
                           const Theme& t, const QFont& f) {
    const QColor paper = editorPaperColor(t);

    if (fmt == CodeFormat::PythonCtypes) {
        if (!dynamic_cast<RcxPyLexer*>(sci->lexer())) {
            QsciLexer* old = sci->lexer();   // setLexer detaches but never deletes
            sci->setLexer(new RcxPyLexer(sci));
            if (old) old->deleteLater();      // else a lexer leaks per class swap
        }
        auto* l = qobject_cast<QsciLexerPython*>(sci->lexer());
        if (!l) return;
        l->setFont(f);
        l->setColor(t.text,          QsciLexerPython::Default);
        l->setColor(t.text,          QsciLexerPython::Identifier);
        l->setColor(t.syntaxKeyword, QsciLexerPython::Keyword);
        l->setColor(t.syntaxType,    QsciLexerPython::HighlightedIdentifier);
        l->setColor(t.syntaxType,    QsciLexerPython::ClassName);
        l->setColor(t.syntaxType,    QsciLexerPython::FunctionMethodName);
        l->setColor(t.syntaxNumber,  QsciLexerPython::Number);
        l->setColor(t.syntaxComment, QsciLexerPython::Comment);
        l->setColor(t.syntaxComment, QsciLexerPython::CommentBlock);
        l->setColor(t.syntaxString,  QsciLexerPython::DoubleQuotedString);
        l->setColor(t.syntaxString,  QsciLexerPython::SingleQuotedString);
        l->setColor(t.syntaxString,  QsciLexerPython::TripleSingleQuotedString);
        l->setColor(t.syntaxString,  QsciLexerPython::TripleDoubleQuotedString);
        l->setColor(t.syntaxPreproc, QsciLexerPython::Decorator);
        l->setColor(t.text,          QsciLexerPython::Operator);
        for (int i = 0; i <= 127; ++i) { l->setPaper(paper, i); l->setFont(f, i); }
        return;
    }

    if (fmt == CodeFormat::CSharpStruct) {
        if (!qobject_cast<QsciLexerCSharp*>(sci->lexer())) {
            QsciLexer* old = sci->lexer();
            sci->setLexer(new QsciLexerCSharp(sci));
            if (old) old->deleteLater();
        }
        themeCppFamilyLexer(qobject_cast<QsciLexerCPP*>(sci->lexer()), t, f, paper);
        return;
    }

    // C++ / Rust / #define — type-aware CPP lexer.
    if (!dynamic_cast<RcxCodeLexer*>(sci->lexer())) {
        QsciLexer* old = sci->lexer();
        sci->setLexer(new RcxCodeLexer(sci));
        if (old) old->deleteLater();
    }
    themeCppFamilyLexer(qobject_cast<QsciLexerCPP*>(sci->lexer()), t, f, paper);
}

} // namespace rcx
