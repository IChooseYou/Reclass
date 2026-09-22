# Large Binary Files

File > Open and the source picker's File command use `FileProvider`. `.bin`,
`.dmp`, and `.dump` files open as binary sources, not JSON definitions. No mode
switch or preload is required. A binary source gets a separate definition tab;
saving that definition never overwrites the binary.

## Implementation

- On-demand reads through Qt's cross-platform `QFile` APIs with 64-bit offsets.
- One bounded 64 KiB read cache per source; larger reads go directly into the
  caller's buffer. The scanner already processes regions in at most 2 MiB chunks.
- Mutex-protected file position, cache, and edits allow a source to be shared by
  instance views and background scanning.
- The original file handle is read-only. Session edits use sparse 4 KiB
  copy-on-write pages, preserving the previous in-memory file-edit behavior.
  Memory for edits and Undo grows with edited data, not total file size.
- Full 64-bit range validation and scanner regions. The legacy plugin ABI's
  `int size()` remains clamped to `INT_MAX`; file operations use `byteSize()` and
  the overridden range checks. No provider-plugin ABI change is required.
- No whole-file memory mapping, whole-file read, or one-row-per-byte UI model.

`QFileDevice::seek` and file sizes use `qint64`; shared-instance access is
explicitly serialized. See [Qt file I/O documentation](https://doc.qt.io/qt-6/qfiledevice.html).

## Verification

- `test_provider`: sparse 64 GiB fixture, reads crossing 2/4 GiB, EOF and overflow
  bounds, cache bounds, concurrent reads, and edits leaving the original intact.
- `test_controller`: navigation and shared-instance editing above 8 GiB, Undo,
  and failed source opens preserving the current source/address.
- `test_scanner_panel`: constrained file scan above 4 GiB.
- Actual Windows fixture: 68,615,969,870-byte dump. The screenshot integration
  run measured 26 ms for File > Open and 2 ms for five sampled range reads;
  observed peak app working set was about 62.4 MiB. The file's size and last-write
  time were unchanged. These are local measurements, not guaranteed cold-disk
  performance. Opening does not scan the entire file.

Run the real-window integration check on a local fixture:

```powershell
$env:RCX_LARGE_FILE = 'F:\MEMORY\MEMORY.DMP'
.\build\RC.exe --screenshot build\large-file.png largefile
```

The result is printed as `LARGE_FILE_RESULT PASS/FAIL`. The screenshot contains
fixture bytes and should not be published without checking its contents.

The implementation uses portable Qt APIs and the sparse tests are platform-aware.
Windows was verified locally; macOS/Linux runtime claims require their CI runs.
This supports raw binary file offsets, not Windows dump virtual-address mapping
or automatic dump-format parsing. Full scans still take time proportional to
bytes searched, and result lists consume memory proportional to retained hits.
