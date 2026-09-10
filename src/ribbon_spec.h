#pragma once
// Data model AND tables for the ReClassEx-style ribbon (tabs → panels → items).
//
// The tables live here (not in ribbon.cpp) so the wiring half can read them
// too: RibbonActions is built into targets that never link ribbon.cpp, and
// every command's label + tooltip has exactly ONE home — this file. The
// widget, the QAction, the menus and the tooltip all read the same strings.
//
// The `data` payload each id carries:
//   type.*   → data = int(NodeKind)
//   add.*    → data = int(byte count)
//   insert.* → data = int(byte count)

#include "core.h"

#include <QHash>
#include <utility>
#include <QString>
#include <QVariant>
#include <QVector>

namespace rcx {

// Colour family of a glyph icon — mapped to theme tokens by ribbonFamilyColour()
// (ribbon_icons.h). Four hues plus white: hex/plain white, int + uint the
// number colour (the I/U letter carries the sign), float the keyword colour,
// text the string colour, pointer the type colour. `markerPtr` (red) is
// reserved for destructive commands and never used as a type family.
enum class GlyphFamily { Hex, Signed, Unsigned, Float, Text, Pointer, Bits, Plain };

enum class RibbonItemSize { Large, Small };

struct RibbonIconSpec {
    enum class Kind {
        TypeGlyph,    // arg = pixel-font label ("H64", "I32", "F" …)
        AddBytes,     // arg = byte count → "+" glyph (labelled) / "+N" pixel text
        InsertBytes,  // arg = byte count → hook arrow (+ the count when unlabelled)
        DeleteCross,  // markerPtr ✕ (no longer used by the spec; kept for callers)
        FillSquares,  // arg = "000" | "FFF" | "???"
        ClassPtr,     // symbol-class Codicon + red "*" overlay
        Codicon       // arg = Codicon basename ("symbol-class") tinted by family
    };
    Kind        kind   = Kind::Codicon;
    QString     arg;
    GlyphFamily family = GlyphFamily::Plain;
    bool        mirrorH = false;   // Codicon flipped left↔right (redo = mirrored discard)
};

struct RibbonItemSpec {
    QString        id;                 // stable action id ("type.int32")
    QString        label;              // the ONE name of this command (menu = ribbon = tooltip)
    QString        tooltip;
    RibbonItemSize size = RibbonItemSize::Large;
    RibbonIconSpec icon;
    bool           columnBreakBefore = false;  // `|`  — start a new column
    bool           separatorBefore   = false;  // `||` — new column + hairline
    bool           iconOnly          = false;  // label used only for the tooltip
    bool           destructive       = false;  // icon markerPtr always, label red on hover/press
    // Opens a menu rather than acting: an 8-px ▾ in the state tone, cell +10 px.
    bool           menu              = false;
    // Strip text when the long label doesn't earn its width ("Add 4" → "4").
    // The QAction keeps `label`, so menus and tooltips stay unambiguous.
    QString        shortLabel;
    // Survives a panel-wide label drop (Type is glyph-only, but "Custom…"
    // is a word, not a glyph).
    bool           keepLabel         = false;
    // Starts a caption group spanning this column until the next groupCaption
    // (or until an `endsCaptionGroup` column). A panel with any group caption
    // draws those INSTEAD of its own caption.
    QString        groupCaption;
    // Closes the open caption group BEFORE this column without opening a new
    // one, so a trailing caption-less column ("Custom…") is not swallowed by
    // the last group's span.
    bool           endsCaptionGroup  = false;
    QVariant       data;
};

struct RibbonPanelSpec {
    QString id;
    QString caption;
    // Narrow-width behaviour (RibbonBar::relayout).
    // Stage 1 (LabelMode::Auto only) drops labels by labelDrop, HIGHER first;
    // panels with equal labelDrop drop together. Stage 2 hides whole panels by
    // hideOrder, LOWER first, never `neverHide`.
    int  labelDrop    = 0;
    int  hideOrder    = 0;
    bool neverHide    = false;
    // The icons ARE the labels (Type): glyph-only in EVERY label mode. Only
    // `keepLabel` items still show text.
    bool glyphLabels  = false;
    QVector<RibbonItemSpec> items;
};

struct RibbonTabSpec {
    QString id;
    QString title;
    QVector<RibbonPanelSpec> panels;
};

// ── Tables ──
// Home first: it is the tab a new user needs (open / save / new class /
// source), Modify is the working tab you switch to.

namespace detail {

inline RibbonItemSpec ribbonGlyphItem(const char* id, const char* label, const char* tip,
                                      const char* glyph, GlyphFamily family, NodeKind kind,
                                      bool brk = false, bool sep = false,
                                      const char* groupCaption = nullptr) {
    RibbonItemSpec it;
    it.id = QString::fromLatin1(id);
    it.label = QString::fromUtf8(label);
    it.tooltip = QString::fromUtf8(tip);
    it.size = RibbonItemSize::Small;
    it.icon = {RibbonIconSpec::Kind::TypeGlyph, QString::fromLatin1(glyph), family};
    it.columnBreakBefore = brk || sep;
    it.separatorBefore = sep;
    if (groupCaption) it.groupCaption = QString::fromUtf8(groupCaption);
    it.data = int(kind);
    return it;
}

inline RibbonItemSpec ribbonCodiconItem(const char* id, const char* label, const char* tip,
                                        const char* icon, GlyphFamily family = GlyphFamily::Plain,
                                        RibbonItemSize size = RibbonItemSize::Small,
                                        bool brk = false, bool sep = false,
                                        QVariant data = QVariant()) {
    RibbonItemSpec it;
    it.id = QString::fromLatin1(id);
    it.label = QString::fromUtf8(label);
    it.tooltip = QString::fromUtf8(tip);
    it.size = size;
    it.icon = {RibbonIconSpec::Kind::Codicon, QString::fromLatin1(icon), family};
    it.columnBreakBefore = brk || sep;
    it.separatorBefore = sep;
    it.data = data;
    return it;
}

// "Add 4" / "Insert 1024": the strip shows the count alone (the panel caption
// already says which verb), the QAction keeps the full sentence.
inline RibbonItemSpec ribbonBytesItem(const char* prefix, RibbonIconSpec::Kind kind, int n, bool brk) {
    RibbonItemSpec it;
    const bool add = (kind == RibbonIconSpec::Kind::AddBytes);
    it.id = QStringLiteral("%1.%2").arg(QLatin1String(prefix)).arg(n);
    it.label = QStringLiteral("%1 %2").arg(add ? QStringLiteral("Add") : QStringLiteral("Insert")).arg(n);
    it.shortLabel = QString::number(n);
    it.tooltip = add
        ? QStringLiteral("Append %1 bytes to the end of the class").arg(n)
        : QStringLiteral("Insert %1 bytes above the selected field").arg(n);
    it.size = RibbonItemSize::Small;
    it.icon = {kind, QString::number(n), GlyphFamily::Plain};
    // Icon-only in EVERY label mode. The unlabelled cell is 32 px wide and
    // renders "+64" itself in the 5x7 pixel font (ribbon_icons.h), so a
    // chrome-font "64" beside it said the same thing twice for 29 px a
    // column. The QAction keeps "Add 64" for menus, the overflow list and
    // the tooltip. Auto mode already dropped these words below 934 px --
    // this just makes the state the user usually sees the only state.
    it.iconOnly = true;
    it.columnBreakBefore = brk;
    it.data = n;
    return it;
}

inline QVector<RibbonTabSpec> buildDefaultRibbonSpec() {
    using IK = RibbonIconSpec::Kind;
    using GF = GlyphFamily;
    using SZ = RibbonItemSize;
    // Local aliases (lambdas, not function pointers — the default arguments
    // below are part of the call, and a function pointer drops them).
    auto glyph = [](const char* id, const char* label, const char* tip, const char* g,
                    GF fam, NodeKind k, bool brk = false, bool sep = false,
                    const char* cap = nullptr) {
        return ribbonGlyphItem(id, label, tip, g, fam, k, brk, sep, cap);
    };
    auto codicon = [](const char* id, const char* label, const char* tip, const char* icon,
                      GF fam = GF::Plain, SZ size = SZ::Small, bool brk = false,
                      bool sep = false, QVariant data = QVariant()) {
        return ribbonCodiconItem(id, label, tip, icon, fam, size, brk, sep, std::move(data));
    };
    auto bytes = [](const char* prefix, IK kind, int n, bool brk) {
        return ribbonBytesItem(prefix, kind, n, brk);
    };

    QVector<RibbonTabSpec> tabs;

    // ── Home: what you do to a PROJECT ──
    // Panels are named after the task, not the implementation: File · Class ·
    // Source · Panels. Size is the hierarchy (one or two Large entry points per
    // panel), hue appears once — the three "creates a type" buttons carry the
    // editor's class colour.
    {
        RibbonTabSpec tab;
        tab.id = QStringLiteral("home");
        tab.title = QStringLiteral("Home");

        RibbonPanelSpec file;
        file.id = QStringLiteral("file");
        file.caption = QStringLiteral("File");
        file.neverHide = true;
        file.labelDrop = 0;
        file.items = {
            codicon("home.file.open", "Open", "Open a project (Ctrl+O)", "folder-opened", GF::Plain, SZ::Large),
            codicon("home.file.save", "Save", "Save the project (Ctrl+S)", "save", GF::Plain, SZ::Large),
            // Not a file dialog: this SHOWS the generated code in the active
            // editor's Code view, scoped to one class or the whole project.
            // Writing it to disk still lives on File ▸ Export.
            codicon("home.file.code", "Code", "Show the generated code in the editor — this class or all classes",
                    "file-code", GF::Plain, SZ::Small, true),
            codicon("home.file.close", "Close", "Close Project (Ctrl+W)", "close"),
        };
        file.items[2].menu = true;   // Export ▾
        tab.panels.append(file);

        RibbonPanelSpec src;
        src.id = QStringLiteral("source");
        src.caption = QStringLiteral("Source");
        src.labelDrop = 10;
        src.hideOrder = 20;
        src.items = {
            codicon("home.source.attach", "Source", "Attach to a process or open a data source",
                    "plug", GF::Plain, SZ::Large),
            codicon("home.source.refresh", "Refresh", "Refresh the memory view (F5)",
                    "refresh", GF::Plain, SZ::Small, true),
            // "Goto Address", not "Set as Base Address": the long form pushed
            // the whole Source panel over for no gain, and nobody thinks of it
            // as setting a property. arrow-right is the icon the scanner
            // already uses for the same rebase (scannerpanel.cpp:985); the old
            // symbol-numeric "#" said "number", not "go".
            codicon("home.source.goto", "Goto Address",
                    "Go to an address — rebases the class to read from there (Ctrl+G)",
                    "arrow-right"),
        };
        src.items[0].menu = true;    // Source ▾
        tab.panels.append(src);

        RibbonPanelSpec cls;
        cls.id = QStringLiteral("class");
        cls.caption = QStringLiteral("Class");
        cls.neverHide = true;
        cls.labelDrop = 0;
        cls.items = {
            codicon("home.class.newclass", "New Class", "Create a new class (Ctrl+N)",
                    "symbol-class", GF::Pointer, SZ::Large),
            codicon("home.class.newstruct", "New Struct", "Create a new struct",
                    "symbol-structure", GF::Pointer, SZ::Small, true),
            codicon("home.class.newenum", "New Enum", "Create a new enum", "symbol-enum", GF::Pointer),
        };
        tab.panels.append(cls);

        RibbonPanelSpec panels;
        panels.id = QStringLiteral("panels");
        // "View", not "Panels": Split Below and Console open no panel, and
        // renaming the caption is cheaper and truer than evicting working
        // affordances to satisfy the old one.
        panels.caption = QStringLiteral("View");
        panels.labelDrop = 30;   // first labels to go …
        panels.hideOrder = 0;    // … and the first panel to hide
        // Two clean columns of three. RTTI joins Scanner and Symbols because
        // all three open a window onto the CURRENT target; Bookmarks, Console
        // and Split Below are the workspace column.
        panels.items = {
            codicon("home.panels.scanner", "Scanner", "Open the memory scanner (Ctrl+Shift+F)", "search"),
            codicon("home.panels.symbols", "Symbols", "Open the symbol browser (Ctrl+Shift+Y)", "symbol-key"),
            // RTTI opens a browser WINDOW (class hierarchy + vtable behind the
            // selected pointer). It sat in Modify ▸ Selection until 2026-09-06,
            // where it was the only item that opened something rather than
            // changing the selection — "wtf is the RTTI field for even?".
            // RibbonActions keeps owning the id so it keeps its enabled
            // predicate; MainWindow triggers the Tools action.
            codicon("home.panels.rtti", "RTTI",
                    "RTTI Browser: class hierarchy behind the selected pointer (Ctrl+Shift+R)",
                    "symbol-interface"),
            codicon("home.panels.bookmarks", "Bookmarks", "Open the bookmarks (Ctrl+Shift+K)",
                    "bookmark", GF::Plain, SZ::Small, true),
            codicon("home.panels.console", "Console", "Show the console", "console"),
            codicon("home.panels.split", "Split Below", "Split the editor below (Ctrl+\\)", "split-vertical"),
        };
        tab.panels.append(panels);

        tabs.append(tab);
    }

    // ── Modify: what you do to the SELECTION ──
    // Add · Insert · Selection · Type.
    {
        RibbonTabSpec tab;
        tab.id = QStringLiteral("modify");
        tab.title = QStringLiteral("Modify");

        RibbonPanelSpec add;
        add.id = QStringLiteral("add");
        add.caption = QStringLiteral("Add");
        add.labelDrop = 20;   // Add + Insert lose their labels together
        add.hideOrder = 20;
        add.items = {
            bytes("add", IK::AddBytes, 4, false),
            bytes("add", IK::AddBytes, 8, false),
            bytes("add", IK::AddBytes, 64, false),
            bytes("add", IK::AddBytes, 1024, true),
            bytes("add", IK::AddBytes, 2048, false),
        };
        tab.panels.append(add);

        RibbonPanelSpec ins;
        ins.id = QStringLiteral("insert");
        ins.caption = QStringLiteral("Insert");
        ins.labelDrop = 20;
        ins.hideOrder = 10;
        ins.items = {
            bytes("insert", IK::InsertBytes, 4, false),
            bytes("insert", IK::InsertBytes, 8, false),
            bytes("insert", IK::InsertBytes, 64, false),
            bytes("insert", IK::InsertBytes, 1024, true),
            bytes("insert", IK::InsertBytes, 2048, false),
        };
        tab.panels.append(ins);

        // Everything you can do TO a selection, in one panel, ordered by what
        // the button does: entry point, edit in place, fill, reshape.
        // "Structure" used to be a separate panel next to this one, which
        // split one question ("I selected some bytes — now what?") across two
        // captions. RTTI moved OUT to Home ▸ Panels: it opens a browser window
        // rather than changing the selection, so it belongs with Scanner and
        // Symbols, not among edit commands.
        RibbonPanelSpec type;
        type.id = QStringLiteral("type");
        type.caption = QStringLiteral("Type");
        type.labelDrop = 30;
        type.neverHide = true;
        type.glyphLabels = true;   // the glyphs ARE the labels, in every mode
        // SIZE-MAJOR, not kind-major. Columns are WIDTHS, rows are readings:
        // 64 / 32 / 16 / 8, each column Hex over Int over UInt. Kind-major
        // made row 1 read "H64 I64 U64 H8" — the row's meaning broke at the
        // fourth column — and it disagreed with the keyboard, where 1-4 pick a
        // WIDTH and S/U/F reinterpret at the existing size. This layout is that
        // grammar made visible. It is also cheaper: "64" fits its column free,
        // where "UInt:" inflated one by 3 px and "Byte:" by 13.
        type.items = {
            glyph("type.hex64", "Hex 64", "Change to hex64 — 8 raw bytes (key 4)", "H64", GF::Hex, NodeKind::Hex64, false, false, "64"),
            glyph("type.int64", "Int 64", "Change to int64_t — signed 8-byte integer (S on an 8-byte field)", "I64", GF::Signed, NodeKind::Int64),
            glyph("type.uint64", "UInt 64", "Change to uint64_t — unsigned 8-byte integer (U on an 8-byte field)", "U64", GF::Unsigned, NodeKind::UInt64),

            glyph("type.hex32", "Hex 32", "Change to hex32 — 4 raw bytes (key 3)", "H32", GF::Hex, NodeKind::Hex32, true, false, "32"),
            glyph("type.int32", "Int 32", "Change to int32_t — signed 4-byte integer (S on a 4-byte field)", "I32", GF::Signed, NodeKind::Int32),
            glyph("type.uint32", "UInt 32", "Change to uint32_t — unsigned 4-byte integer (U on a 4-byte field)", "U32", GF::Unsigned, NodeKind::UInt32),

            glyph("type.hex16", "Hex 16", "Change to hex16 — 2 raw bytes (key 2)", "H16", GF::Hex, NodeKind::Hex16, true, false, "16"),
            glyph("type.int16", "Int 16", "Change to int16_t — signed 2-byte integer (S on a 2-byte field)", "I16", GF::Signed, NodeKind::Int16),
            glyph("type.uint16", "UInt 16", "Change to uint16_t — unsigned 2-byte integer (U on a 2-byte field)", "U16", GF::Unsigned, NodeKind::UInt16),

            glyph("type.hex8", "Hex 8", "Change to hex8 — 1 raw byte (key 1)", "H8", GF::Hex, NodeKind::Hex8, true, false, "8"),
            glyph("type.int8", "Int 8", "Change to int8_t — signed byte (S on a 1-byte field)", "I8", GF::Signed, NodeKind::Int8),
            glyph("type.uint8", "UInt 8", "Change to uint8_t — unsigned byte (U on a 1-byte field)", "U8", GF::Unsigned, NodeKind::UInt8),

            // One `‖` opens the float group; the columns inside it are plain
            // `|` breaks. Four hairlines through one 4x3 grid contradicted the
            // grid, and the rule after D/F/B made "Float" read as the caption
            // for V2/V3/V4 rather than for the whole group.
            glyph("type.double", "Double", "Change to double — 8-byte float (F on an 8-byte field)", "D", GF::Float, NodeKind::Double, true, true, "Float"),
            glyph("type.float", "Float", "Change to float — 4-byte float (F on a 4-byte field)", "F", GF::Float, NodeKind::Float),

            glyph("type.vec2", "Vec 2", "Change to vec2 — 2 floats, 8 bytes", "V2", GF::Float, NodeKind::Vec2, true),
            glyph("type.vec3", "Vec 3", "Change to vec3 — 3 floats, 12 bytes", "V3", GF::Float, NodeKind::Vec3),
            glyph("type.vec4", "Vec 4", "Change to vec4 — 4 floats, 16 bytes", "V4", GF::Float, NodeKind::Vec4),

            glyph("type.mat4x4", "Mat 4x4", "Change to mat4x4 — 16 floats, 64 bytes", "M4", GF::Float, NodeKind::Mat4x4, true),

            glyph("type.pointer", "Pointer", "Change to ptr64 — 8-byte pointer (P key)", "PTR", GF::Pointer, NodeKind::Pointer64, true, true, "Ptr"),
            glyph("type.funcptr", "Func Ptr", "Change to fnptr64 — 8-byte function pointer", "FN*", GF::Pointer, NodeKind::FuncPtr64),

            glyph("type.utf8", "Str", "Change to str — ASCII / UTF-8 text", "STR", GF::Text, NodeKind::UTF8, true, true, "Str"),
            glyph("type.utf16", "WStr", "Change to wstr — UTF-16 text", "WSTR", GF::Text, NodeKind::UTF16),

            // Bool is a 1-byte flag, not a float. Under "Float" the caption
            // stated something false about the button beneath it.
            glyph("type.bool", "Bool", "Change to bool — 1 byte", "B", GF::Bits, NodeKind::Bool, true, true, "Other"),

        };
        {
            // A codicon, not a pixel glyph: "ASM" is three wide letters whose
            // ink overflows the 16-unit square cell by 2 px, which
            // test_pixel_glyphs pins as a contract. It also sits beside
            // Custom… in the same visual language, which is the right place
            // for a specialist type.
            RibbonItemSpec asmItem = codicon("type.asm", "Asm",
                                             "Change to asm — disassemble these bytes in place",
                                             "file-binary", GF::Bits, SZ::Small,
                                             false, false, int(NodeKind::Asm));
            asmItem.keepLabel = true;
            type.items.append(asmItem);

            RibbonItemSpec custom = codicon("type.custom", "Custom…",
                                            "Any type by name: opens the inline type editor",
                                            "symbol-misc", GF::Plain, SZ::Small);
            custom.keepLabel = true;   // a word, not a glyph — it survives the drop
            // Shares the "Other" column with Bool now, so it no longer has to
            // close "Str" with a separator of its own.
            type.items.append(custom);
        }
        tab.panels.append(type);


        RibbonPanelSpec sel;
        sel.id = QStringLiteral("selected");
        sel.caption = QStringLiteral("Selection");
        sel.labelDrop = 10;   // the reshape words go before Add / Insert counts
        sel.hideOrder = 0;    // first panel to hide
        {
            // The panel's entry point, and the most consequential thing a
            // selection can become: a class sized exactly to it. Large, so
            // size matches how much it matters.
            RibbonItemSpec brk = codicon("type.class", "Carve",
                                         "Carve the selected bytes / fields out into a new embedded class (Ctrl+Shift+B)",
                                         "selection", GF::Pointer, SZ::Large);
            brk.data = int(NodeKind::Struct);
            brk.keepLabel = true;      // words in EVERY label mode
            sel.items.append(brk);

            // Delete is the one red thing on the strip: a Codicon `close`
            // tinted markerPtr (the hand-drawn ✕ bitmap read as a different
            // weight next to its Codicon neighbours).
            RibbonItemSpec del = codicon("sel.delete", "Delete",
                                         "Delete the selected fields (Delete)", "close");
            del.destructive = true;
            del.columnBreakBefore = true;   // starts the small column beside Carve
            sel.items.append(del);
            sel.items.append(codicon("sel.duplicate", "Duplicate",
                                     "Duplicate the selected fields below themselves (Ctrl+D)", "clippy"));
            sel.items.append(codicon("sel.comment", "Comment",
                                     "Edit the comment of the selected field(s) (;)", "note"));

            auto fill = [](const char* id, const char* label, const char* tip,
                           const char* text, bool brk) {
                RibbonItemSpec it;
                it.id = QString::fromLatin1(id);
                it.label = QString::fromUtf8(label);
                it.tooltip = QString::fromUtf8(tip);
                it.size = RibbonItemSize::Small;
                // Plain: the squares + the label take the item's state tone,
                // so a fill button reads as chrome, not as a type.
                it.icon = {RibbonIconSpec::Kind::FillSquares, QString::fromLatin1(text), GlyphFamily::Plain};
                it.iconOnly = true;
                it.columnBreakBefore = brk;
                return it;
            };
            sel.items.append(fill("sel.zero", "Zero", "Fill the selected bytes with 00", "000", true));
            sel.items.append(fill("sel.ff", "FF", "Fill the selected bytes with FF", "FFF", false));
            sel.items.append(fill("sel.random", "Random", "Fill the selected bytes with random values", "???", false));

            RibbonItemSpec swap = codicon("sel.swap", "Big endian",
                                          "Show the selected scalars byte-swapped (big-endian)",
                                          "sync", GF::Plain, SZ::Small, true);
            sel.items.append(swap);

            // Reshape: the other two things a selection can become. They share
            // the column with Big endian because all three change what the
            // selected bytes ARE rather than removing or overwriting them.
            RibbonItemSpec pc;
            pc.id = QStringLiteral("type.ptrclass");
            pc.label = QStringLiteral("Ptr → Class");
            pc.tooltip = QStringLiteral("Change to a pointer to a new class (each selected 4/8-byte field)");
            pc.size = RibbonItemSize::Small;
            pc.icon = {IK::ClassPtr, QString(), GF::Pointer};
            pc.data = int(NodeKind::Pointer64);
            pc.keepLabel = true;
            sel.items.append(pc);

            RibbonItemSpec arr = codicon("type.array", "Array",
                                         "Turn the selected field (or contiguous same-type fields) into an array",
                                         "symbol-array");
            arr.data = int(NodeKind::Array);
            arr.keepLabel = true;
            sel.items.append(arr);
        }
        tab.panels.append(sel);

        tabs.append(tab);
    }

    return tabs;
}

}  // namespace detail

// Home + Modify tables (Home first — it is also the first-run default tab).
inline const QVector<RibbonTabSpec>& defaultRibbonSpec() {
    static const QVector<RibbonTabSpec> spec = detail::buildDefaultRibbonSpec();
    return spec;
}

// The spec entry behind an id, or nullptr. The single source of a command's
// label + tooltip: RibbonActions reads them from here rather than repeating
// the strings, so ribbon = menu = tooltip by construction.
inline const RibbonItemSpec* ribbonSpecForId(const QString& id) {
    static const QHash<QString, const RibbonItemSpec*> index = [] {
        QHash<QString, const RibbonItemSpec*> m;
        for (const RibbonTabSpec& tab : defaultRibbonSpec())
            for (const RibbonPanelSpec& panel : tab.panels)
                for (const RibbonItemSpec& it : panel.items)
                    if (!m.contains(it.id)) m.insert(it.id, &it);
        return m;
    }();
    return index.value(id, nullptr);
}

// The text painted on the strip: the short form when the spec has one.
inline const QString& ribbonStripLabel(const RibbonItemSpec& it) {
    return it.shortLabel.isEmpty() ? it.label : it.shortLabel;
}

}  // namespace rcx
