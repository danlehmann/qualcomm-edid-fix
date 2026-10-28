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
DisplayID. On those monitors the list is full before the driver reaches DisplayID, so
the high refresh modes are silently dropped and Windows offers 60 Hz.

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
DisplayID mode would not fit a 4-lane HBR3 link at the declared colour depth, it also
declares 8 bits per colour, which is what the driver outputs anyway.

The trimmed EDID is written to `C:\Windows\edidoverride.xml` and loaded through the
driver's own EDID override escape for that one display output. Nothing is installed and
no driver files are modified; the override lives in driver memory until reboot.

## Usage

Build with `build.cmd` (needs clang; Visual Studio 2022's "C++ Clang tools" component works).
It produces `qcedid-gui.exe` and the command line tool `qcedid.exe`.

### qcedid-gui

Asks for administrator rights on start. Pick the monitor (or open a raw EDID file) to see
every mode its EDID gives the driver, in the order the driver adds them, with the slot
each one takes:

- **Green**: a DisplayID mode that makes it into the list.
- **Red**, below the red line: past the 32-mode limit, dropped by the driver.
- **Yellow**: exceeds the link bandwidth at the chosen colour depth, skipped without a slot.
- **Grey**: removed, or a duplicate of an earlier mode (no slot).

Untick modes to remove them, or press **Auto trim**. **Colour depth** sets the bits per
colour the override declares. **Apply** saves the original EDID to `%ProgramData%\qcedid`,
writes the override and loads it; then unplug and replug the monitor and pick the refresh
rate in Settings > Display > Advanced display. **Clear override** removes it. **Save
EDID...** writes the trimmed EDID to a file. `qcedid-gui.exe edid.bin` opens a file directly.

### qcedid

```
qcedid list
```

Shows connected external monitors, how many modes their EDID produces, their DisplayID
timings, and whether they are affected.

```
qcedid apply [target]
```

From an **elevated** prompt. Saves the original EDID to `%ProgramData%\qcedid`, writes
the override and loads it. Then **unplug and replug the monitor** and pick the refresh
rate in Settings > Display > Advanced display. Options:

- `--bpc N`: declare N bits per colour instead of the automatic choice.
- `--display-id N`: the driver's display id, if the default (Windows target id + 1) is wrong.
- `--force`: run on an untested driver version, or on an EDID without DisplayID timings.

```
qcedid clear [target | --all]
```

Removes the override; replug the monitor afterwards. Rebooting also removes it.

```
qcedid analyze edid.bin [--bpc N] [--out trimmed.bin]
```

Offline: shows the mode table before and after trimming for a raw EDID file.

`color-info.ps1` prints the refresh rate, bit depth and colour encoding of each active
display, to confirm what the driver is actually sending.

## Limitations

- The slot model is inferred from the driver's code and checked against one monitor.
  Pixel clocks marked `~` are estimates, and the driver may skip or merge modes the model
  counts. Auto trim leaves 4 slots free for that reason.
- Tested only with driver 31.0.152.1 (Surface Pro 11) and a Philips 329M1RV
  (4K @ 120 Hz over USB-C, 8-bit RGB). Other driver versions are refused unless `--force`
  is given, since the escape layout may change. Reports from other devices are welcome.
- The override applies to a display output, not to a monitor: a different monitor
  plugged into the same port gets this EDID until you run `clear`, replug, or reboot.
- Overrides are lost on reboot or driver reload; run `apply` again and replug.
- The mapping from Windows target id to driver display id (target + 1) was observed on
  one machine. If `apply` succeeds but the mode does not appear after replugging, try
  `--display-id` with values 4, 5 or 6.
- The link must actually have the bandwidth: a USB-C hub or dock that keeps USB 3 data
  usually leaves only 2 DisplayPort lanes, which cannot carry 4K @ 120 Hz.

## Technical notes

See [NOTES.md](NOTES.md) for the reverse engineered driver interfaces.
