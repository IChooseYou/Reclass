#include "import_reclass_xml.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QHash>
#include <QVector>
#include <QDebug>

namespace rcx {

// ── Version-specific type maps ──
// Maps XML Type attribute (integer) → NodeKind.
// Entries with no rcx equivalent use Hex8 as fallback.

enum class XmlVersion { V2013, V2016 };

// 2016 / ReClassEx / MemeClsEx type map (35 entries, index = XML Type value)
static const struct { int xmlType; NodeKind kind; } kTypeMap2016[] = {
    // 0: null (unused)
    { 1,  NodeKind::Struct },     // ClassInstance
    // 2,3: null
    { 4,  NodeKind::Hex32 },
    { 5,  NodeKind::Hex64 },
    { 6,  NodeKind::Hex16 },
    { 7,  NodeKind::Hex8 },
    { 8,  NodeKind::Pointer64 },  // ClassPointer
    { 9,  NodeKind::Int64 },
    { 10, NodeKind::Int32 },
    { 11, NodeKind::Int16 },
    { 12, NodeKind::Int8 },
    { 13, NodeKind::Float },
    { 14, NodeKind::Double },
    { 15, NodeKind::UInt32 },
    { 16, NodeKind::UInt16 },
    { 17, NodeKind::UInt8 },
    { 18, NodeKind::UTF8 },       // UTF8Text
    { 19, NodeKind::UTF16 },      // UTF16Text
    { 20, NodeKind::Pointer64 },  // FunctionPtr
    { 21, NodeKind::Hex8 },       // Custom (expanded by Size)
    { 22, NodeKind::Vec2 },
    { 23, NodeKind::Vec3 },
    { 24, NodeKind::Vec4 },
    { 25, NodeKind::Mat4x4 },
    { 26, NodeKind::Pointer64 },  // VTable
    { 27, NodeKind::Array },      // ClassInstanceArray
    // 28: null (used for Class elements, not nodes)
    { 29, NodeKind::Pointer64 },  // UTF8TextPtr
    { 30, NodeKind::Pointer64 },  // UTF16TextPtr
    // 31: BitField → UInt8 fallback
    { 31, NodeKind::UInt8 },
    { 32, NodeKind::UInt64 },
    { 33, NodeKind::Pointer64 },  // Function
};

// 2013 / ReClass 2011 type map (31 entries)
static const struct { int xmlType; NodeKind kind; } kTypeMap2013[] = {
    { 1,  NodeKind::Struct },     // ClassInstance
    { 4,  NodeKind::Hex32 },
    { 5,  NodeKind::Hex16 },
    { 6,  NodeKind::Hex8 },
    { 7,  NodeKind::Pointer64 },  // ClassPointer
    { 8,  NodeKind::Int32 },
    { 9,  NodeKind::Int16 },
    { 10, NodeKind::Int8 },
    { 11, NodeKind::Float },
    { 12, NodeKind::UInt32 },
    { 13, NodeKind::UInt16 },
    { 14, NodeKind::UInt8 },
    { 15, NodeKind::UTF8 },       // UTF8Text
    { 16, NodeKind::Pointer64 },  // FunctionPtr
    { 17, NodeKind::Hex8 },       // Custom
    { 18, NodeKind::Vec2 },
    { 19, NodeKind::Vec3 },
    { 20, NodeKind::Vec4 },
    { 21, NodeKind::Mat4x4 },
    { 22, NodeKind::Pointer64 },  // VTable
    { 23, NodeKind::Array },      // ClassInstanceArray
    { 27, NodeKind::Int64 },
    { 28, NodeKind::Double },
    { 29, NodeKind::UTF16 },      // UTF16Text
    { 30, NodeKind::Array },      // ClassPointerArray
};

static NodeKind lookupKind(int xmlType, XmlVersion ver, int ptrSize = 8) {
    NodeKind k = NodeKind::Hex8;
    if (ver == XmlVersion::V2016) {
        for (const auto& e : kTypeMap2016)
            if (e.xmlType == xmlType) { k = e.kind; break; }
    } else {
        for (const auto& e : kTypeMap2013)
            if (e.xmlType == xmlType) { k = e.kind; break; }
    }
    // Remap pointer types for 32-bit targets
    if (ptrSize < 8 && k == NodeKind::Pointer64)
        k = NodeKind::Pointer32;
    return k;
}

// Is this XML type a pointer-like type that uses the "Pointer" attribute?
static bool isPointerType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016)
        return xmlType == 8 || xmlType == 20 || xmlType == 26 || xmlType == 29 || xmlType == 30 || xmlType == 33;
    else
        return xmlType == 7 || xmlType == 16 || xmlType == 22;
}

// Is this XML type a ClassInstance (embedded struct)?
static bool isClassInstanceType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016) return xmlType == 1;
    else return xmlType == 1;
}

// Is this XML type a ClassInstanceArray?
static bool isClassInstanceArrayType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016) return xmlType == 27;
    else return xmlType == 23 || xmlType == 30;
}

// Is this XML type a text node?
static bool isTextType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016) return xmlType == 18 || xmlType == 19;
    else return xmlType == 15 || xmlType == 29;
}

// Is this XML type a UTF16 text node?
static bool isUtf16TextType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016) return xmlType == 19;
    else return xmlType == 29;
}

// Is this XML type a Custom node (expanded to hex)?
static bool isCustomType(int xmlType, XmlVersion ver) {
    if (ver == XmlVersion::V2016) return xmlType == 21;
    else return xmlType == 17;
}

// Deferred pointer resolution entry
struct PendingRef {
    uint64_t nodeId;
    QString  className;
};

NodeTree importReclassXml(const QString& filePath, QString* errorMsg, int pointerSize) {
    qDebug() << "[ImportXML] Opening file:" << filePath;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "[ImportXML] ERROR: Cannot open file";
        if (errorMsg) *errorMsg = QStringLiteral("Cannot open file: ") + filePath;
        return {};
    }

    qDebug() << "[ImportXML] File size:" << file.size() << "bytes";

    QXmlStreamReader xml(&file);
    XmlVersion version = XmlVersion::V2016; // default to 2016 (most common)

    NodeTree tree;
    tree.baseAddress = 0x00400000;
    tree.pointerSize = pointerSize;

    // Class name → struct node ID (for pointer resolution)
    QHash<QString, uint64_t> classIds;
    // Deferred pointer refs to resolve after all classes are parsed
    QVector<PendingRef> pendingRefs;

    // Detect version from first comment
    bool versionDetected = false;

    while (!xml.atEnd()) {
        xml.readNext();

        // Detect version from XML comments
        if (!versionDetected && xml.isComment()) {
            QString comment = xml.text().toString().trimmed();
            if (comment.contains(QStringLiteral("ReClassEx"), Qt::CaseInsensitive) ||
                comment.contains(QStringLiteral("MemeClsEx"), Qt::CaseInsensitive) ||
                comment.contains(QStringLiteral("2016"), Qt::CaseInsensitive) ||
                comment.contains(QStringLiteral("2015"), Qt::CaseInsensitive)) {
                version = XmlVersion::V2016;
            } else if (comment.contains(QStringLiteral("2013"), Qt::CaseInsensitive) ||
                       comment.contains(QStringLiteral("2011"), Qt::CaseInsensitive)) {
                version = XmlVersion::V2013;
            }
            // else keep default V2016
            versionDetected = true;
            qDebug() << "[ImportXML] Detected version:" << (version == XmlVersion::V2016 ? "V2016" : "V2013");
        }

        if (!xml.isStartElement()) continue;

        if (xml.name() == QStringLiteral("Class")) {
            // Parse a class element into a root Struct node
            QString className = xml.attributes().value(QStringLiteral("Name")).toString();
            QString strOffset = xml.attributes().value(QStringLiteral("strOffset")).toString();

            // Create root struct node (collapsed by default for large files)
            Node structNode;
            structNode.kind = NodeKind::Struct;
            structNode.name = className;
            structNode.structTypeName = className;
            structNode.parentId = 0; // root level
            structNode.offset = 0;
            structNode.collapsed = true;

            int structIdx = tree.addNode(structNode);
            uint64_t structId = tree.nodes[structIdx].id;
            classIds[className] = structId;
            qDebug() << "[ImportXML] Class:" << className << "id:" << structId;

            // Parse child Node elements
            int childOffset = 0;
            while (!xml.atEnd()) {
                xml.readNext();

                if (xml.isEndElement() && xml.name() == QStringLiteral("Class"))
                    break;

                if (!xml.isStartElement() || xml.name() != QStringLiteral("Node"))
                    continue;

                int xmlType = xml.attributes().value(QStringLiteral("Type")).toInt();
                QString nodeName = xml.attributes().value(QStringLiteral("Name")).toString();
                int nodeSize = xml.attributes().value(QStringLiteral("Size")).toInt();
                QString ptrClass = xml.attributes().value(QStringLiteral("Pointer")).toString();
                QString instClass = xml.attributes().value(QStringLiteral("Instance")).toString();

                qDebug() << "[ImportXML]   Node:" << nodeName << "type:" << xmlType
                         << "size:" << nodeSize << "ptr:" << ptrClass << "inst:" << instClass;

                // Handle Custom type: expand to appropriate hex nodes
                if (isCustomType(xmlType, version) && nodeSize > 0) {
                    // Pick best-fit hex kind
                    NodeKind hexKind;
                    int hexSize;
                    if (nodeSize >= 8 && nodeSize % 8 == 0) {
                        hexKind = NodeKind::Hex64; hexSize = 8;
                    } else if (nodeSize >= 4 && nodeSize % 4 == 0) {
                        hexKind = NodeKind::Hex32; hexSize = 4;
                    } else if (nodeSize >= 2 && nodeSize % 2 == 0) {
                        hexKind = NodeKind::Hex16; hexSize = 2;
                    } else {
                        hexKind = NodeKind::Hex8; hexSize = 1;
                    }
                    int count = nodeSize / hexSize;
                    for (int i = 0; i < count; i++) {
                        Node n;
                        n.kind = hexKind;
                        n.name = (count == 1) ? nodeName : QString();
                        n.parentId = structId;
                        n.offset = childOffset;
                        tree.addNode(n);
                        childOffset += hexSize;
                    }
                    continue;
                }

                NodeKind kind = lookupKind(xmlType, version, pointerSize);

                // Handle ClassInstanceArray: read child <Array> element
                if (isClassInstanceArrayType(xmlType, version)) {
                    qDebug() << "[ImportXML]     -> ClassInstanceArray";
                    int total = xml.attributes().value(QStringLiteral("Total")).toInt();
                    if (total <= 0)
                        total = xml.attributes().value(QStringLiteral("Count")).toInt();
                    if (total <= 0) total = 1;

                    // Read child <Array> element for class name
                    QString arrayClassName;
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isEndElement() && xml.name() == QStringLiteral("Node"))
                            break;
                        if (xml.isStartElement() && xml.name() == QStringLiteral("Array")) {
                            arrayClassName = xml.attributes().value(QStringLiteral("Name")).toString();
                            int arrayTotal = xml.attributes().value(QStringLiteral("Total")).toInt();
                            if (arrayTotal <= 0)
                                arrayTotal = xml.attributes().value(QStringLiteral("Count")).toInt();
                            if (arrayTotal > 0) total = arrayTotal;
                        }
                    }

                    // Create an Array node wrapping Struct elements
                    Node arrNode;
                    arrNode.kind = NodeKind::Array;
                    arrNode.name = nodeName;
                    arrNode.parentId = structId;
                    arrNode.offset = childOffset;
                    arrNode.arrayLen = total;
                    arrNode.elementKind = NodeKind::Struct;
                    if (!arrayClassName.isEmpty())
                        arrNode.structTypeName = arrayClassName;
                    int arrIdx = tree.addNode(arrNode);
                    uint64_t arrId = tree.nodes[arrIdx].id;

                    // Defer ref resolution if array references a class
                    if (!arrayClassName.isEmpty()) {
                        pendingRefs.append({arrId, arrayClassName});
                    }

                    childOffset += nodeSize > 0 ? nodeSize : 0;
                    continue;
                }

                Node n;
                n.kind = kind;
                n.name = nodeName;
                n.parentId = structId;
                n.offset = childOffset;

                // Handle text nodes
                if (isTextType(xmlType, version)) {
                    if (isUtf16TextType(xmlType, version))
                        n.strLen = qMax(1, nodeSize / 2);
                    else
                        n.strLen = qMax(1, nodeSize);
                }

                // Handle pointer types
                if (isPointerType(xmlType, version) && !ptrClass.isEmpty()) {
                    qDebug() << "[ImportXML]     -> Pointer to class:" << ptrClass;
                    n.collapsed = true; // Start collapsed to avoid recursive expansion freeze
                    int nodeIdx = tree.addNode(n);
                    uint64_t nodeId = tree.nodes[nodeIdx].id;
                    pendingRefs.append({nodeId, ptrClass});
                    childOffset += nodeSize > 0 ? nodeSize : sizeForKind(kind);
                    continue;
                }

                // Handle embedded class instance
                if (isClassInstanceType(xmlType, version)) {
                    QString resolvedClass = instClass.isEmpty() ? ptrClass : instClass;
                    qDebug() << "[ImportXML]     -> ClassInstance:" << resolvedClass;
                    n.collapsed = true; // Start collapsed to avoid recursive expansion freeze
                    n.structTypeName = resolvedClass;
                    if (!n.structTypeName.isEmpty()) {
                        int nodeIdx = tree.addNode(n);
                        uint64_t nodeId = tree.nodes[nodeIdx].id;
                        pendingRefs.append({nodeId, n.structTypeName});
                    } else {
                        tree.addNode(n);
                    }
                    childOffset += nodeSize > 0 ? nodeSize : 0;
                    continue;
                }

                tree.addNode(n);
                childOffset += nodeSize > 0 ? nodeSize : sizeForKind(kind);
            }
        }
    }

    if (xml.hasError() && xml.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
        qDebug() << "[ImportXML] XML parse error at line" << xml.lineNumber() << ":" << xml.errorString();
        if (errorMsg)
            *errorMsg = QStringLiteral("XML parse error at line %1: %2")
                .arg(xml.lineNumber())
                .arg(xml.errorString());
        return {};
    }

    qDebug() << "[ImportXML] Parsing complete. Total nodes:" << tree.nodes.size()
             << "classes:" << classIds.size() << "pending refs:" << pendingRefs.size();

    if (tree.nodes.isEmpty()) {
        qDebug() << "[ImportXML] ERROR: No classes found";
        if (errorMsg) *errorMsg = QStringLiteral("No classes found in file");
        return {};
    }

    // Resolve deferred pointer/struct references
    int resolved = 0, unresolved = 0;
    for (const auto& ref : pendingRefs) {
        int nodeIdx = tree.indexOfId(ref.nodeId);
        if (nodeIdx < 0) continue;

        auto it = classIds.find(ref.className);
        if (it != classIds.end()) {
            tree.nodes[nodeIdx].refId = it.value();
            resolved++;
        } else {
            qDebug() << "[ImportXML] Unresolved ref:" << ref.className << "for node" << ref.nodeId;
            unresolved++;
        }
    }

    qDebug() << "[ImportXML] Refs resolved:" << resolved << "unresolved:" << unresolved;
    qDebug() << "[ImportXML] Import complete. Returning tree with" << tree.nodes.size() << "nodes";

    return tree;
}

} // namespace rcx
