#pragma once
#include "core.h"
#include <QVector>

namespace rcx {

// ── Common type template: a predefined struct with real fields ──
// When the user selects one from the typechooser, the struct is created
// with these exact fields (not blank hex64 padding).

struct CommonField {
    int      offset;
    NodeKind kind;
    const char* name;
    // For pointer fields: target type name (empty = void*)
    const char* ptrTarget = nullptr;
};

struct CommonType {
    const char* name;           // e.g. "_M128A"
    const char* category;       // e.g. "Windows NT", "C++ STL"
    const char* classKeyword;   // "struct", "union", "class"
    int         totalSize;      // bytes
    const CommonField* fields;
    int         fieldCount;
};

// ── Windows NT types ──

static const CommonField kFields_M128A[] = {
    {0x00, NodeKind::UInt64, "Low"},
    {0x08, NodeKind::UInt64, "High"},
};

static const CommonField kFields_UNICODE_STRING[] = {
    {0x00, NodeKind::UInt16,    "Length"},
    {0x02, NodeKind::UInt16,    "MaximumLength"},
    {0x04, NodeKind::Hex32,     "_padding"},
    {0x08, NodeKind::Pointer64, "Buffer", "UTF16"},
};

static const CommonField kFields_LIST_ENTRY[] = {
    {0x00, NodeKind::Pointer64, "Flink"},
    {0x08, NodeKind::Pointer64, "Blink"},
};

static const CommonField kFields_LARGE_INTEGER[] = {
    {0x00, NodeKind::UInt32, "LowPart"},
    {0x04, NodeKind::Int32,  "HighPart"},
};

static const CommonField kFields_OBJECT_ATTRIBUTES[] = {
    {0x00, NodeKind::UInt32,    "Length"},
    {0x04, NodeKind::Hex32,     "_pad"},
    {0x08, NodeKind::Pointer64, "RootDirectory"},
    {0x10, NodeKind::Pointer64, "ObjectName", "UNICODE_STRING"},
    {0x18, NodeKind::UInt32,    "Attributes"},
    {0x1c, NodeKind::Hex32,     "_pad2"},
    {0x20, NodeKind::Pointer64, "SecurityDescriptor"},
    {0x28, NodeKind::Pointer64, "SecurityQualityOfService"},
};

static const CommonField kFields_CLIENT_ID[] = {
    {0x00, NodeKind::Pointer64, "UniqueProcess"},
    {0x08, NodeKind::Pointer64, "UniqueThread"},
};

static const CommonField kFields_IO_STATUS_BLOCK[] = {
    {0x00, NodeKind::Int64,     "Status"},
    {0x08, NodeKind::UInt64,    "Information"},
};

static const CommonField kFields_GUID[] = {
    {0x00, NodeKind::UInt32, "Data1"},
    {0x04, NodeKind::UInt16, "Data2"},
    {0x06, NodeKind::UInt16, "Data3"},
    {0x08, NodeKind::Hex64,  "Data4"},
};

// ── C++ STL types (MSVC x64 layout) ──

static const CommonField kFields_std_string[] = {
    {0x00, NodeKind::Pointer64, "_Ptr"},
    {0x08, NodeKind::Hex64,     "_Buf_hi"},  // second half of SSO buffer
    {0x10, NodeKind::UInt64,    "_Mysize"},
    {0x18, NodeKind::UInt64,    "_Myres"},
};

// std::wstring has identical MSVC layout to std::string — shares kFields_std_string

static const CommonField kFields_std_vector[] = {
    {0x00, NodeKind::Pointer64, "_Myfirst"},
    {0x08, NodeKind::Pointer64, "_Mylast"},
    {0x10, NodeKind::Pointer64, "_Myend"},
};

static const CommonField kFields_std_shared_ptr[] = {
    {0x00, NodeKind::Pointer64, "_Ptr"},
    {0x08, NodeKind::Pointer64, "_Rep"},
};

static const CommonField kFields_std_unique_ptr[] = {
    {0x00, NodeKind::Pointer64, "_Ptr"},
};

static const CommonField kFields_std_function[] = {
    {0x00, NodeKind::Hex64,     "_storage0"},
    {0x08, NodeKind::Hex64,     "_storage1"},
    {0x10, NodeKind::Hex64,     "_storage2"},
    {0x18, NodeKind::Hex64,     "_storage3"},
    {0x20, NodeKind::Pointer64, "_impl"},
    {0x28, NodeKind::Pointer64, "_invoke"},
};

static const CommonField kFields_std_map_node[] = {
    {0x00, NodeKind::Pointer64, "_Left"},
    {0x08, NodeKind::Pointer64, "_Parent"},
    {0x10, NodeKind::Pointer64, "_Right"},
    {0x18, NodeKind::UInt8,     "_Color"},    // 0=red, 1=black
    {0x19, NodeKind::UInt8,     "_IsNil"},
    {0x1a, NodeKind::Hex16,     "_pad"},
    {0x1c, NodeKind::Hex32,     "_pad2"},
    {0x20, NodeKind::Hex64,     "_Key"},      // user fills in real key type
    {0x28, NodeKind::Hex64,     "_Value"},    // user fills in real value type
};

static const CommonField kFields_std_unordered_map[] = {
    {0x00, NodeKind::Pointer64, "_List_head"},
    {0x08, NodeKind::UInt64,    "_List_size"},
    {0x10, NodeKind::Pointer64, "_Vec_buckets"},
    {0x18, NodeKind::UInt64,    "_Vec_size"},
    {0x20, NodeKind::UInt64,    "_Mask"},
    {0x28, NodeKind::UInt64,    "_Maxidx"},
    {0x30, NodeKind::Float,     "_Max_load_factor"},
    {0x34, NodeKind::Hex32,     "_pad"},
};

// ── Unreal Engine types ──

static const CommonField kFields_FString[] = {
    {0x00, NodeKind::Pointer64, "Data"},
    {0x08, NodeKind::Int32,     "Num"},
    {0x0c, NodeKind::Int32,     "Max"},
};

static const CommonField kFields_FName[] = {
    {0x00, NodeKind::Int32,     "ComparisonIndex"},
    {0x04, NodeKind::Int32,     "Number"},
};

// TArray has identical layout to FString — shares kFields_FString

static const CommonField kFields_FVector[] = {
    {0x00, NodeKind::Float,     "X"},
    {0x04, NodeKind::Float,     "Y"},
    {0x08, NodeKind::Float,     "Z"},
};

static const CommonField kFields_FRotator[] = {
    {0x00, NodeKind::Float,     "Pitch"},
    {0x04, NodeKind::Float,     "Yaw"},
    {0x08, NodeKind::Float,     "Roll"},
};

static const CommonField kFields_FTransform[] = {
    {0x00, NodeKind::Vec4,      "Rotation"},    // quaternion XYZW
    {0x10, NodeKind::Vec4,      "Translation"}, // XYZ + pad
    {0x20, NodeKind::Vec4,      "Scale3D"},     // XYZ + pad
};

static const CommonField kFields_FQuat[] = {
    {0x00, NodeKind::Float,     "X"},
    {0x04, NodeKind::Float,     "Y"},
    {0x08, NodeKind::Float,     "Z"},
    {0x0c, NodeKind::Float,     "W"},
};

static const CommonField kFields_FLinearColor[] = {
    {0x00, NodeKind::Float,     "R"},
    {0x04, NodeKind::Float,     "G"},
    {0x08, NodeKind::Float,     "B"},
    {0x0c, NodeKind::Float,     "A"},
};

// ── Generic patterns ──
// These are layouts that appear in nearly every C++ codebase

static const CommonField kFields_VTable[] = {
    {0x00, NodeKind::FuncPtr64, "fn0"},
    {0x08, NodeKind::FuncPtr64, "fn1"},
    {0x10, NodeKind::FuncPtr64, "fn2"},
    {0x18, NodeKind::FuncPtr64, "fn3"},
    {0x20, NodeKind::FuncPtr64, "fn4"},
    {0x28, NodeKind::FuncPtr64, "fn5"},
    {0x30, NodeKind::FuncPtr64, "fn6"},
    {0x38, NodeKind::FuncPtr64, "fn7"},
};

static const CommonField kFields_RefCounted[] = {
    {0x00, NodeKind::Pointer64, "__vptr"},
    {0x08, NodeKind::Int32,     "_refCount"},
    {0x0c, NodeKind::Int32,     "_weakCount"},
};

static const CommonField kFields_LinkedNode[] = {
    {0x00, NodeKind::Pointer64, "next"},
    {0x08, NodeKind::Pointer64, "prev"},
    {0x10, NodeKind::Pointer64, "data"},
};

static const CommonField kFields_TreeNode[] = {
    {0x00, NodeKind::Pointer64, "left"},
    {0x08, NodeKind::Pointer64, "right"},
    {0x10, NodeKind::Pointer64, "parent"},
    {0x18, NodeKind::Pointer64, "data"},
};

static const CommonField kFields_SlabEntry[] = {
    {0x00, NodeKind::Pointer64, "data"},
    {0x08, NodeKind::UInt32,    "size"},
    {0x0c, NodeKind::UInt32,    "capacity"},
    {0x10, NodeKind::UInt32,    "flags"},
    {0x14, NodeKind::UInt32,    "refCount"},
};

static const CommonField kFields_Delegate[] = {
    {0x00, NodeKind::Pointer64, "object"},
    {0x08, NodeKind::Pointer64, "function"},
};

static const CommonField kFields_Variant[] = {
    {0x00, NodeKind::Hex64,     "data0"},
    {0x08, NodeKind::Hex64,     "data1"},
    {0x10, NodeKind::UInt32,    "typeId"},
    {0x14, NodeKind::UInt32,    "flags"},
};

static const CommonField kFields_RGBA8[] = {
    {0x00, NodeKind::UInt8,     "r"},
    {0x01, NodeKind::UInt8,     "g"},
    {0x02, NodeKind::UInt8,     "b"},
    {0x03, NodeKind::UInt8,     "a"},
};

static const CommonField kFields_AABB[] = {
    {0x00, NodeKind::Vec3,      "min"},
    {0x0c, NodeKind::Vec3,      "max"},
};

static const CommonField kFields_Matrix4x4[] = {
    {0x00, NodeKind::Mat4x4,    "m"},
};

static const CommonField kFields_Sphere[] = {
    {0x00, NodeKind::Vec3,      "center"},
    {0x0c, NodeKind::Float,     "radius"},
};

static const CommonField kFields_Ray[] = {
    {0x00, NodeKind::Vec3,      "origin"},
    {0x0c, NodeKind::Vec3,      "direction"},
};

static const CommonField kFields_Plane[] = {
    {0x00, NodeKind::Vec3,      "normal"},
    {0x0c, NodeKind::Float,     "distance"},
};

static const CommonField kFields_TimeStamp[] = {
    {0x00, NodeKind::Int64,     "ticks"},     // 100ns intervals since epoch
};

static const CommonField kFields_Slice[] = {
    {0x00, NodeKind::Pointer64, "ptr"},
    {0x08, NodeKind::UInt64,    "len"},
};

static const CommonField kFields_FatPointer[] = {
    {0x00, NodeKind::Pointer64, "ptr"},
    {0x08, NodeKind::Pointer64, "vtable"},
};

// ── Windows additional ──

static const CommonField kFields_RTL_BALANCED_NODE[] = {
    {0x00, NodeKind::Pointer64, "Left"},
    {0x08, NodeKind::Pointer64, "Right"},
    {0x10, NodeKind::UInt64,    "ParentValue"},  // low bits = color
};

static const CommonField kFields_SINGLE_LIST_ENTRY[] = {
    {0x00, NodeKind::Pointer64, "Next"},
};

static const CommonField kFields_STRING[] = {
    {0x00, NodeKind::UInt16,    "Length"},
    {0x02, NodeKind::UInt16,    "MaximumLength"},
    {0x04, NodeKind::Hex32,     "_pad"},
    {0x08, NodeKind::Pointer64, "Buffer"},
};

static const CommonField kFields_DISPATCHER_HEADER[] = {
    {0x00, NodeKind::Int32,     "Lock"},
    {0x04, NodeKind::Int32,     "SignalState"},
    {0x08, NodeKind::Pointer64, "WaitListHead_Flink"},
    {0x10, NodeKind::Pointer64, "WaitListHead_Blink"},
};

// ── Registry ──
// CT(name, cat, kw, size, fields) expands to a CommonType with auto-counted fields.
#define CT(n, cat, kw, sz, arr) {n, cat, kw, sz, arr, (int)std::size(arr)}

static const CommonType kCommonTypes[] = {
    // Windows NT
    CT("_M128A",             "Windows NT", "struct", 16, kFields_M128A),
    CT("UNICODE_STRING",     "Windows NT", "struct", 16, kFields_UNICODE_STRING),
    CT("LIST_ENTRY",         "Windows NT", "struct", 16, kFields_LIST_ENTRY),
    CT("LARGE_INTEGER",      "Windows NT", "union",   8, kFields_LARGE_INTEGER),
    CT("OBJECT_ATTRIBUTES",  "Windows NT", "struct", 48, kFields_OBJECT_ATTRIBUTES),
    CT("CLIENT_ID",          "Windows NT", "struct", 16, kFields_CLIENT_ID),
    CT("IO_STATUS_BLOCK",    "Windows NT", "struct", 16, kFields_IO_STATUS_BLOCK),
    CT("GUID",               "Windows NT", "struct", 16, kFields_GUID),
    CT("RTL_BALANCED_NODE",  "Windows NT", "struct", 24, kFields_RTL_BALANCED_NODE),
    CT("SINGLE_LIST_ENTRY",  "Windows NT", "struct",  8, kFields_SINGLE_LIST_ENTRY),
    CT("STRING",             "Windows NT", "struct", 16, kFields_STRING),
    CT("DISPATCHER_HEADER",  "Windows NT", "struct", 24, kFields_DISPATCHER_HEADER),

    // C++ STL (MSVC x64)
    CT("std::string",        "C++ STL",    "class",  32, kFields_std_string),
    CT("std::wstring",       "C++ STL",    "class",  32, kFields_std_string),
    CT("std::vector",        "C++ STL",    "class",  24, kFields_std_vector),
    CT("std::shared_ptr",    "C++ STL",    "class",  16, kFields_std_shared_ptr),
    CT("std::unique_ptr",    "C++ STL",    "class",   8, kFields_std_unique_ptr),
    CT("std::function",      "C++ STL",    "class",  48, kFields_std_function),
    CT("std::map_node",      "C++ STL",    "struct", 48, kFields_std_map_node),
    CT("std::unordered_map", "C++ STL",    "class",  56, kFields_std_unordered_map),

    // Unreal Engine
    CT("FString",            "Unreal",     "struct", 16, kFields_FString),
    CT("FName",              "Unreal",     "struct",  8, kFields_FName),
    CT("TArray",             "Unreal",     "struct", 16, kFields_FString),
    CT("FVector",            "Unreal",     "struct", 12, kFields_FVector),
    CT("FRotator",           "Unreal",     "struct", 12, kFields_FRotator),
    CT("FTransform",         "Unreal",     "struct", 48, kFields_FTransform),
    CT("FQuat",              "Unreal",     "struct", 16, kFields_FQuat),
    CT("FLinearColor",       "Unreal",     "struct", 16, kFields_FLinearColor),

    // Generic patterns
    CT("VTable8",            "Generic",    "struct", 64, kFields_VTable),
    CT("RefCounted",         "Generic",    "class",  16, kFields_RefCounted),
    CT("LinkedNode",         "Generic",    "struct", 24, kFields_LinkedNode),
    CT("TreeNode",           "Generic",    "struct", 32, kFields_TreeNode),
    CT("SlabEntry",          "Generic",    "struct", 24, kFields_SlabEntry),
    CT("Delegate",           "Generic",    "struct", 16, kFields_Delegate),
    CT("Variant",            "Generic",    "struct", 24, kFields_Variant),
    CT("Slice",              "Generic",    "struct", 16, kFields_Slice),
    CT("FatPointer",         "Generic",    "struct", 16, kFields_FatPointer),
    CT("TimeStamp",          "Generic",    "struct",  8, kFields_TimeStamp),

    // Math / Spatial
    CT("RGBA8",              "Math",       "struct",  4, kFields_RGBA8),
    CT("AABB",               "Math",       "struct", 24, kFields_AABB),
    CT("Matrix4x4",          "Math",       "struct", 64, kFields_Matrix4x4),
    CT("Sphere",             "Math",       "struct", 16, kFields_Sphere),
    CT("Ray",                "Math",       "struct", 24, kFields_Ray),
    CT("Plane",              "Math",       "struct", 16, kFields_Plane),
};

#undef CT

static constexpr int kCommonTypeCount = (int)std::size(kCommonTypes);

// Look up a common type by name. Returns nullptr if not found.
inline const CommonType* findCommonType(const QString& name) {
    for (int i = 0; i < kCommonTypeCount; i++) {
        if (name == QLatin1String(kCommonTypes[i].name))
            return &kCommonTypes[i];
    }
    return nullptr;
}

} // namespace rcx
