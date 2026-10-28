# Technical notes

Reverse engineered from `qcdxkm8380.sys` 31.0.152.1 (ARM64). Addresses are virtual
addresses with the default image base 0x140000000.

## Mode list limit

- The EDID is parsed into a per-display mode list. Add-mode (0x140159290) refuses new
  entries once the count reaches 0x20; entries are 0x98 bytes and the list is 0x1304
  bytes (count + 32 entries).
- Each candidate is validated (0x140159790) before it is counted, so modes the link
  cannot carry do not use up slots. The check is link bandwidth at the declared colour
  depth: CTA VIC 118 (4K120, 1188 MHz) is accepted at 6 bpc but not at 8 or 10 bpc on
  4-lane HBR3. There is no pixel clock cap below 1188 MHz.
- Parse order in 0x140157548: CTA-861 extension blocks (including HDMI VICs 1..4),
  then the four base block descriptors in order (detailed timings via 0x140158f40,
  0xFA standard timing descriptors via 0x140159c80, 0xF7 established timings III),
  then the base block standard timings (0x140159c80), then the 17 established timing
  bits. Standard and established timings are added by index into the driver's timing
  table (0x1401595c0), and add-mode skips an exact duplicate of a listed timing.
- DisplayID is handled by 0x1401a9630, called after the base/CTA parse (0x140157548)
  from 0x14019ab8c and 0x14019acb8. It supports DisplayID 1.2, 1.3 and 2.0; type I
  (0x03) and type VII (0x22) timing blocks are decoded by 0x1401a7f90 and added through
  the same add-mode function, so they are dropped once the list is full.
- The caller only replaces the mode list with the DisplayID-extended one when it is
  longer than the original.
- YCbCr 4:2:0 capability maps do not make VIC 118 available over DisplayPort.

## EDID sources

- The Windows `EDID_OVERRIDE` registry key reaches monitor.sys but not the driver's
  mode list; the driver reads the EDID itself.
- `ReadEDIDFromReg` / `PanelEdid` (registry, 128 bytes) is an internal panel path.
- External DP settings come from ACPI `PCFG` XML in the DSDT (`DPMaxLinkRate`,
  `DPMaxLaneCount`, `DPDSCOverride`, `DPFixedLanedConfiguration`, `DPEnableSSC`, ...).

## EDID override escape

The driver loads `C:\Windows\edidoverride.xml` when asked through `DxgkDdiEscape`:

- File: lines separated by `\n`. A line containing `BLOCK` (and not `/>`) contributes
  the 256 characters following `BLOCKn>`, parsed as hex. The text buffer holds 1027
  characters, so at most 4 blocks.
- Escape handler 0x14046d340: private data exactly 0x4854 bytes, escape code
  0x20000100 at +0x0c, result at +0x14, command size at +0x88, command at +0x8c. The
  escape needs a device handle (`D3DKMTCreateDevice`); it does not need elevation.
- Command dispatcher 0x14010f028, group 0x106, sub-command 2 at 0x140111468:
  `{0x106, 2, display id 1..10, enable}`, status at +0x1c. Status 0 = ok, 3 = no such
  display, 5 = file missing / parse failure / unsupported display type.
- The override is stored in the display state (+0x620 buffer, +0x618 size 1024,
  +0x658 enabled) in memory only, and used the next time the display is detected.
- After the override is active, Windows also reports the overridden EDID (registry
  `EDID` value and `WmiMonitorDescriptorMethods`), so the original must be saved first.

## Display ids

On a Surface Pro 11, ids 1, 4, 5, 6, 8, 9 and 10 have state; 2, 3 and 7 return status 3.
The internal panel is Windows target 0 / display id 1, and a USB-C DisplayPort monitor
was target 3 / display id 4.
