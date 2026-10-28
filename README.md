# qualcomm-edid-fix

Restores missing high refresh rate modes (for example 4K @ 120 Hz) on external monitors
connected to Snapdragon X Elite / X Plus laptops running Windows.

Unofficial and reverse engineered. It talks to the GPU driver through an undocumented
debug interface; use at your own risk.

## The problem

The Qualcomm display driver (`qcdxkm8380.sys`) keeps at most **32 modes** per display,
and it reads the EDID's DisplayID extension block **after** the base and CTA-861
blocks. Many monitors declare dozens of legacy modes (VGA-era established and standard
timings, 480p/576p/interlaced TV modes) and put their high refresh timings only in
DisplayID, because the classic EDID timing format cannot describe pixel clocks above
655 MHz. On those monitors the list is full before the driver reaches DisplayID, so the
high refresh modes are silently dropped and Windows offers 60 Hz.

Typical symptoms:

- The monitor works at its full refresh rate on other machines, including Linux on the
  same Snapdragon chip, but tops out at 60 Hz on Windows.
- A USB-C to HDMI adapter gets the high refresh rate (the HDMI EDID describes it differently).

## How the fix works

`qcedid` models the driver's mode list: the order it parses the EDID in (CTA-861 blocks,
base block descriptors, standard timings, established timings, DisplayID last), which
modes it skips as duplicates, and which it rejects because they exceed the link
bandwidth. It then builds a copy of the EDID with the lowest value modes removed, one at
a time and lowest resolution first (VGA-era established and standard timings, then
4:2:0-only modes, then interlaced and low resolution CTA modes; native modes are never
removed), until the DisplayID timings fit with a few slots to spare. If the fastest
DisplayID mode would not pass the driver's bandwidth check at the declared colour depth,
the copy declares 8 bits per colour; this only affects which modes the driver lists, and
HDR at 10-bit still works.

The trimmed EDID is written to `C:\Windows\edidoverride.xml` and loaded through the
driver's own EDID override escape. Nothing is installed and no driver files are
modified; the override lives in driver memory until reboot.

## Usage

Download the release, or build with `build.cmd` (needs clang; Visual Studio 2022's
"C++ Clang tools" component works). There is a UI, `qcedid-gui.exe`, and a command line
tool, `qcedid.exe`.

### qcedid-gui

Asks for administrator rights on start. With the monitor connected it says, for example:

> Your PHL 329M1RV supports 4K (3840 × 2160) at 120 Hz, but because of a bug in the
> Qualcomm display driver, only up to 4K (3840 × 2160) at 60 Hz is available.

Press **Fix**, then unplug the monitor and plug it back in, and pick the refresh rate in
Settings > Display > Advanced display. The window notices the replug and shows that the
fix is active. **Undo fix** removes it again; press it before connecting a different
monitor (see Limitations).

**Advanced** shows every mode the monitor's EDID gives the driver, in the order the
driver adds them, with the slot each one takes:

- **Green**: a DisplayID mode that makes it into the list.
- **Red**, below the red line: past the 32-mode limit, dropped by the driver.
- **Yellow**: exceeds the link bandwidth at the chosen colour depth, skipped without a slot.
- **Grey**: removed, or a duplicate of an earlier mode (no slot).

Untick modes to remove them, or press **Auto trim**. **Colour depth** sets the bits per
colour the override declares. **Apply** and **Clear override** load and remove the
override; the display id field limits them to one driver output (empty means all external
outputs). **Save EDID...** writes the trimmed EDID to a file, and **Open EDID file...**
(or `qcedid-gui.exe edid.bin`) analyses a raw EDID dump without a monitor connected.

### qcedid

```
qcedid list
```

Shows connected external monitors, how many mode list slots their EDID uses before
DisplayID, their DisplayID timings, and whether they are affected.

```
qcedid apply [target]
```

From an **elevated** prompt. Saves the original EDID to `%ProgramData%\qcedid`, writes
the override and loads it. Then **unplug and replug the monitor**. Options:

- `--bpc N`: declare N bits per colour instead of the automatic choice.
- `--margin N`: mode list slots to keep free after the DisplayID modes (default 4). Raise
  it if DisplayID modes are still missing: the driver adds modes the tool does not model,
  for example for monitors with a variable refresh range.
- `--display-id N`: load it on one driver output only (default: all external outputs).
- `--force`: run on an untested driver version, or on an EDID without DisplayID timings.

```
qcedid clear [--display-id N]
```

Removes the override; replug the monitor afterwards. Rebooting also removes it.

```
qcedid analyze edid.bin [--bpc N] [--margin N] [--out trimmed.bin]
```

Offline: shows the mode table before and after trimming for a raw EDID file.

`color-info.ps1` prints the refresh rate, bit depth and colour encoding of each active
display, to confirm what the driver is actually sending.

## Limitations

- Tested on a Surface Pro 11 with drivers 31.0.152.1 and 31.0.170.0 and a Philips
  329M1RV: 4K @ 120 Hz over USB-C, including HDR at 10-bit. Other driver versions ask
  for confirmation (UI) or `--force` (command line), since the driver interface may
  change. Reports from other devices and monitors are welcome; please include the
  output of `qcedid list`.
- Which driver output belongs to which USB-C port is not known, so the override is loaded
  on all external outputs. While it is active, any monitor you connect gets the fixed
  monitor's EDID: use **Undo fix** / `qcedid clear` before switching monitors.
- Overrides are lost on reboot or driver reload; apply again and replug.
- The slot model is inferred from the driver's code and checked against one monitor.
  Pixel clocks marked `~` are estimates, and the driver may skip or merge modes the model
  counts. Auto trim leaves 4 slots free for that reason.
- The tool only fixes modes that the 32-mode limit drops. The driver also rejects modes
  above a configured maximum refresh rate (on a Surface Pro 11 apparently 120 Hz) and
  modes that need more than the uncompressed link bandwidth; for example 5120x2160 at
  120 or 165 Hz on an Alienware AW3926QW stays unavailable.
- The link must actually have the bandwidth: a USB-C hub or dock that keeps USB 3 data
  usually leaves only 2 DisplayPort lanes, which cannot carry 4K @ 120 Hz.

## Technical notes

See [NOTES.md](NOTES.md) for the reverse engineered driver interfaces.
