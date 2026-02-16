#include "export_reclass_xml.h"
#include <QFile>
#include <QXmlStreamWriter>
#include <QHash>
#include <QVector>
#include <algorithm>

namespace rcx {

// Reverse type map: NodeKind -> ReClassEx V2016 XML Type integer
static int xmlTypeForKind(NodeKind kind) {
    switch (kind) {
    case NodeKind::Struct:    return 1;   // ClassInstance
    case NodeKind::Hex32:     return 4;
    case NodeKind::Hex64:     return 5;
    case NodeKind::Hex16:     return 6;
    case NodeKind::Hex8:      return 7;
    case NodeKind::Pointer64: return 8;   // ClassPointer
    case NodeKind::Pointer32: return 8;
    case NodeKind::Int64:     return 9;
    case NodeKind::Int32:     return 10;
    case NodeKind::Int16:     return 11;
    case NodeKind::Int8:      return 12;
    case NodeKind::Float:     return 13;
    case NodeKind::Double:    return 14;
    case NodeKind::UInt32:    return 15;
    case NodeKind::UInt16:    return 16;
    case NodeKind::UInt8:     return 17;
    case NodeKind::UInt64:    return 32;
    case NodeKind::UTF8:      return 18;
    case NodeKind::UTF16:     return 19;
    case NodeKind::Bool:      return 17;  // No native bool in ReClass, map to UInt8
    case NodeKind::Vec2:      return 22;
    case NodeKind::Vec3:      return 23;
    case NodeKind::Vec4:      return 24;
    case NodeKind::Mat4x4:    return 25;
    case NodeKind::Array:     return 27;  // ClassInstanceArray
    }
    return 7; // fallback to Hex8
}

static int nodeSizeForExport(const Node& node) {
    switch (node.kind) {
    case NodeKind::UTF8:  return node.strLen;
    case NodeKind::UTF16: return node.strLen * 2;
    case NodeKind::Array: {
        int elemSz = sizeForKind(node.elementKind);
        return node.arrayLen * (elemSz > 0 ? elemSz : 0);
    }
    default: return sizeForKind(node.kind);
    }
}

// Resolve a struct type name from a node ID
static QString resolveStructName(const NodeTree& tree, uint64_t refId) {
    int idx = tree.indexOfId(refId);
    if (idx < 0) return {};
    const Node& ref = tree.nodes[idx];
    if (!ref.structTypeName.isEmpty()) return ref.structTypeName;
    return ref.name;
}

bool exportReclassXml(const NodeTree& tree, const QString& filePath, QString* errorMsg) {
    if (tree.nodes.isEmpty()) {
        if (errorMsg) *errorMsg = QStringLiteral("No nodes to export");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMsg) *errorMsg = QStringLiteral("Cannot open file for writing: ") + filePath;
        return false;
    }

    // Build child map
    QHash<uint64_t, QVector<int>> childMap;
    for (int i = 0; i < tree.nodes.size(); i++)
        childMap[tree.nodes[i].parentId].append(i);

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(4);
    xml.writeStartDocument();

    xml.writeStartElement(QStringLiteral("ReClass"));
    xml.writeComment(QStringLiteral("ReClassEx"));

    // Get root structs
    QVector<int> roots = childMap.value(0);
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        return tree.nodes[a].offset < tree.nodes[b].offset;
    });

    int classCount = 0;

    for (int ri : roots) {
        const Node& root = tree.nodes[ri];
        if (root.kind != NodeKind::Struct) continue;

        xml.writeStartElement(QStringLiteral("Class"));
        xml.writeAttribute(QStringLiteral("Name"), root.name.isEmpty() ? root.structTypeName : root.name);
        xml.writeAttribute(QStringLiteral("Type"), QStringLiteral("28"));
        xml.writeAttribute(QStringLiteral("Comment"), QString());
        xml.writeAttribute(QStringLiteral("Offset"), QStringLiteral("0"));
        xml.writeAttribute(QStringLiteral("strOffset"), QStringLiteral("0"));
        xml.writeAttribute(QStringLiteral("Code"), QString());

        // Get children sorted by offset
        QVector<int> children = childMap.value(root.id);
        std::sort(children.begin(), children.end(), [&](int a, int b) {
            return tree.nodes[a].offset < tree.nodes[b].offset;
        });

        int i = 0;
        while (i < children.size()) {
            const Node& child = tree.nodes[children[i]];

            // Collapse consecutive hex nodes into a single Custom node (Type=21)
            if (isHexNode(child.kind)) {
                int runStart = child.offset;
                int runEnd = child.offset + child.byteSize();
                int j = i + 1;
                while (j < children.size()) {
                    const Node& next = tree.nodes[children[j]];
                    if (!isHexNode(next.kind)) break;
                    if (next.offset < runEnd) break; // overlap
                    runEnd = next.offset + next.byteSize();
                    j++;
                }
                int totalSize = runEnd - runStart;
                xml.writeStartElement(QStringLiteral("Node"));
                // Use first hex node's name if it's a single node, otherwise generate
                QString hexName = (j - i == 1 && !child.name.isEmpty()) ? child.name : QString();
                xml.writeAttribute(QStringLiteral("Name"), hexName);
                xml.writeAttribute(QStringLiteral("Type"), QStringLiteral("21")); // Custom
                xml.writeAttribute(QStringLiteral("Size"), QString::number(totalSize));
                xml.writeAttribute(QStringLiteral("bHidden"), QStringLiteral("false"));
                xml.writeAttribute(QStringLiteral("Comment"), QString());
                xml.writeEndElement(); // Node
                i = j;
                continue;
            }

            xml.writeStartElement(QStringLiteral("Node"));
            xml.writeAttribute(QStringLiteral("Name"), child.name);
            xml.writeAttribute(QStringLiteral("Type"), QString::number(xmlTypeForKind(child.kind)));
            xml.writeAttribute(QStringLiteral("Size"), QString::number(nodeSizeForExport(child)));
            xml.writeAttribute(QStringLiteral("bHidden"), QStringLiteral("false"));
            xml.writeAttribute(QStringLiteral("Comment"), QString());

            // Pointer with target
            if ((child.kind == NodeKind::Pointer64 || child.kind == NodeKind::Pointer32) && child.refId != 0) {
                QString target = resolveStructName(tree, child.refId);
                if (!target.isEmpty())
                    xml.writeAttribute(QStringLiteral("Pointer"), target);
            }

            // Embedded struct instance
            if (child.kind == NodeKind::Struct) {
                QString instName = child.structTypeName.isEmpty() ? child.name : child.structTypeName;
                xml.writeAttribute(QStringLiteral("Instance"), instName);
            }

            // Array: Total attribute and child <Array> element
            if (child.kind == NodeKind::Array) {
                xml.writeAttribute(QStringLiteral("Total"), QString::number(child.arrayLen));

                // Resolve element type name
                QString elemName;
                if (child.elementKind == NodeKind::Struct && !child.structTypeName.isEmpty()) {
                    elemName = child.structTypeName;
                } else if (child.refId != 0) {
                    elemName = resolveStructName(tree, child.refId);
                }
                if (elemName.isEmpty())
                    elemName = kindToString(child.elementKind);

                xml.writeStartElement(QStringLiteral("Array"));
                xml.writeAttribute(QStringLiteral("Name"), elemName);
                xml.writeAttribute(QStringLiteral("Total"), QString::number(child.arrayLen));
                xml.writeEndElement(); // Array
            }

            xml.writeEndElement(); // Node
            i++;
        }

        xml.writeEndElement(); // Class
        classCount++;
    }

    xml.writeEndElement(); // ReClass
    xml.writeEndDocument();
    file.close();

    if (classCount == 0) {
        if (errorMsg) *errorMsg = QStringLiteral("No struct classes found to export");
        return false;
    }

    return true;
}

} // namespace rcx
