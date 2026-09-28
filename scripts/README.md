# RMS shader switch

Add `rms-shader-switch.js` as an external script in **Scripts**. It uses Quartz's
`@quartz/client` SDK and exposes these properties in the script card:

- **RMS threshold**: defaults to `0.02`. Below it, use the quiet shader; at or
  above it, return to the equalizer.
- **Equalizer shader**: defaults to Rainbow equalizer.
- **Quiet shader (previous)**: initially remembers the shader used before the
  equalizer. You can choose a different one here.
- **Transition seconds**: defaults to `0.35`.

The script owns the shader mutex while enabled and releases it when disabled,
reloaded or removed. Its remembered shader and properties survive restarts.
If no previous shader is available, choose a quiet shader in its properties.
The script never overwrites that choice with its own alternating transitions.

# Start Quartz with Windows

First build a portable folder using `build-windows.ps1`. Then run:

```powershell
.\scripts\install-windows-startup.ps1 -Executable .\build\Quartz-Windows-x64\quartz.exe
```

This creates the current user's `Quartz.lnk` startup entry. Quartz starts hidden;
**Ctrl+Alt+Shift+Q** shows it. To stop automatic startup, remove `Quartz.lnk` from
your Windows Startup folder. Keep the portable folder at the selected path.
