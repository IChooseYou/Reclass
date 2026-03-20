# Lesson 07: Enums & Bitfields
**Difficulty:** Intermediate | **Prerequisites:** Lesson 06 (Arrays & Strings) | **Time:** ~15 minutes

## Learning Objectives

- Create enum types via the GUI (File > New Enum, Ctrl+E) or programmatically with `classKeyword="enum"` and `enumMembers`
- Understand how enum members are displayed as `name = value` pairs
- Define bitfield containers with `classKeyword="bitfield"`, `elementKind`, and `bitfieldMembers`
- Use `extractBits()` to read individual bit ranges from a container value
- Know that bitfields are created via import or API, not through the GUI directly

## Concept Overview

Enums and bitfields are two specialized forms of the `Struct` node kind. They reuse the `kind=Struct` infrastructure but change behavior through `classKeyword` and dedicated member arrays.

### Enums

An enum defines a set of named integer constants. In the data model:

| Field | Value |
|-------|-------|
| `kind` | `Struct` |
| `classKeyword` | `"enum"` |
| `enumMembers` | Array of `{name, value}` pairs |

Enum members are rendered as indented lines inside the enum body:

```
enum Team {
    None  = 0
    Red   = 1
    Blue  = 2
    Green = 3
}
```

### Bitfields

A bitfield packs multiple named flags or small integers into a single container word (typically 8, 16, or 32 bits):

| Field | Value |
|-------|-------|
| `kind` | `Struct` |
| `classKeyword` | `"bitfield"` |
| `elementKind` | Container size: `Hex8`, `Hex16`, `Hex32`, or `Hex64` |
| `bitfieldMembers` | Array of `{name, bitOffset, bitWidth}` entries |

Each `BitfieldMember` describes one named range of bits within the container:

```cpp
struct BitfieldMember {
    QString name;
    uint8_t bitOffset;  // position from LSB (0 = lowest bit)
    uint8_t bitWidth;   // number of bits (1 = single flag, 2+ = small integer)
};
```

The container's total byte size is determined by `elementKind`: a `Hex32` container is 4 bytes, a `Hex8` container is 1 byte.

### Creation Paths

**Enums** can be created directly from the GUI: **File > New Enum** (Ctrl+E), or right-click an empty workspace area and select **New Enum**. This creates a starter enum with 5 members (Member0–Member4). Once created, right-click to add/remove members, or click to inline-edit names and values.

**Bitfields** are created only via import (C header parsing, PDB import) or the MCP/programmatic API. There is no GUI workflow to create bitfields from scratch. However, once they exist in a project, their members can be viewed and their values are displayed correctly.

## Step-by-Step Walkthrough

### Step 1: Create an Enum Type

Define a `Team` enum with four members:

```json
{
  "kind": "Struct",
  "structTypeName": "Team",
  "name": "Team",
  "classKeyword": "enum",
  "parentId": "0",
  "offset": 0,
  "collapsed": false,
  "enumMembers": [
    { "name": "None",  "value": "0" },
    { "name": "Red",   "value": "1" },
    { "name": "Blue",  "value": "2" },
    { "name": "Green", "value": "3" }
  ]
}
```

![Enum type with named members](../screenshots/lesson_07_step1_enum.png)

The `resolvedClassKeyword()` method returns `"enum"` for this node. Each member is displayed on its own indented line inside the enum body, formatted by `fmt::fmtEnumMember()`:

```
enum Team {
    None  = 0
    Red   = 1
    Blue  = 2
    Green = 3
}
```

Note that `enumMembers` is a `QVector<QPair<QString, int64_t>>` -- each entry is a name-value pair where the value is a signed 64-bit integer, supporting the full range of C/C++ enum values.

**GUI:** File > New Enum (Ctrl+E), or right-click empty workspace area > New Enum. Creates a starter enum with 5 members. Right-click to add/remove members, click to edit names and values. You can also import enums from C headers via Import from Source (Lesson 09).

**MCP:** Add a node with `classKeyword: "enum"` and the `enumMembers` array as shown above.

### Step 2: Create a Bitfield

Define a `flags` bitfield inside a root struct. The bitfield uses a 32-bit container (`Hex32`) and packs four named fields:

```json
{
  "kind": "Struct",
  "name": "flags",
  "classKeyword": "bitfield",
  "parentId": "<root struct ID>",
  "offset": 4,
  "elementKind": "Hex32",
  "collapsed": false,
  "bitfieldMembers": [
    { "name": "isActive",   "bitOffset": 0, "bitWidth": 1 },
    { "name": "isVisible",  "bitOffset": 1, "bitWidth": 1 },
    { "name": "hasShield",  "bitOffset": 2, "bitWidth": 1 },
    { "name": "teamIndex",  "bitOffset": 3, "bitWidth": 2 }
  ]
}
```

![Bitfield with 4 named bit ranges](../screenshots/lesson_07_step2_bitfield.png)

The display format uses `fmt::fmtBitfieldMember()` to show each member with its bit width and extracted value:

```
bitfield flags {            // container = 0x00000015
    isActive   : 1 = 1
    isVisible  : 1 = 0
    hasShield  : 1 = 1
    teamIndex  : 2 = 2
}
```

Key details:
- `elementKind = Hex32` means the container is 4 bytes (32 bits)
- `byteSize()` for a bitfield returns `sizeForKind(elementKind)`, so 4 bytes here
- Single-bit members (`bitWidth: 1`) are boolean flags (0 or 1)
- Multi-bit members (`bitWidth: 2`) are small integers (0-3 for 2 bits)

**GUI:** Bitfields cannot be created through the GUI. Use Import from Source or the MCP API.

**MCP:** Add a node with `classKeyword: "bitfield"`, `elementKind`, and the `bitfieldMembers` array.

### Step 3: Verify Bitfield Value Extraction

The `fmt::extractBits()` function reads the container value from memory and isolates individual bit ranges:

```cpp
uint64_t extractBits(const Provider& prov, uint64_t addr,
                     NodeKind containerKind,
                     uint8_t bitOffset, uint8_t bitWidth);
```

Given a container value of `0x15` (binary `10101`) at offset `0x04`:

| Member | bitOffset | bitWidth | Binary | Value |
|--------|-----------|----------|--------|-------|
| isActive | 0 | 1 | `1` | 1 (true) |
| isVisible | 1 | 1 | `0` | 0 (false) |
| hasShield | 2 | 1 | `1` | 1 (true) |
| teamIndex | 3 | 2 | `10` | 2 |

![Bitfield values extracted from container](../screenshots/lesson_07_step3_bitfield_values.png)

The extraction formula (conceptually):

```
value = (container >> bitOffset) & ((1 << bitWidth) - 1)
```

For `teamIndex` at bitOffset=3, bitWidth=2:
```
0x15 = 0b00010101
>> 3 = 0b00000010
& 0b11 = 2
```

## Multiple Ways to Do This (GUI / Keyboard / MCP)

| Action | GUI | Keyboard | MCP |
|--------|-----|----------|-----|
| Create enum | File > New Enum | Ctrl+E | `addNode` with `classKeyword: "enum"`, `enumMembers` |
| Create bitfield | Not available | Not available | `addNode` with `classKeyword: "bitfield"`, `bitfieldMembers` |
| Import enum from C header | File > Import from Source | -- | `importFromSource()` with header text |
| View member values | Expand the enum/bitfield node | `Space` to expand | Set `collapsed: false` |
| Edit enum member name | Click member name in display | -- | Modify `enumMembers` array |
| Edit enum member value | Click member value in display | -- | Modify `enumMembers` array |

## Key Takeaways

- **Enums and bitfields reuse `kind=Struct`.** The `classKeyword` field (`"enum"` or `"bitfield"`) changes how the node is rendered and sized.
- **Enum members are name-value pairs.** Stored in the `enumMembers` vector as `QPair<QString, int64_t>`. Values can be any 64-bit signed integer.
- **Bitfield members define bit ranges.** Each `BitfieldMember` has a `name`, `bitOffset` (from LSB), and `bitWidth`. These are packed within the container defined by `elementKind`.
- **`extractBits()` handles the bit math.** Given a provider, address, container kind, bit offset, and bit width, it returns the isolated value. This is used by both the display renderer and the value editing system.
- **Enums have a full GUI creation path.** File > New Enum (Ctrl+E) or right-click workspace > New Enum. Once created, members are fully editable via right-click and inline editing.
- **Bitfields have no GUI creation path.** They are created through Import from Source (parsing C/C++ headers), PDB import, or programmatic API/MCP calls. Once imported, they display and function fully within the editor.
- **Container size matters.** A bitfield's `elementKind` determines its byte footprint: `Hex8` = 1 byte (8 bits), `Hex16` = 2 bytes (16 bits), `Hex32` = 4 bytes (32 bits), `Hex64` = 8 bytes (64 bits).

## Next Lesson

[Lesson 08: Unions](08_unions.md) -- Learn how to model union types where multiple field interpretations overlap at the same memory offset.
