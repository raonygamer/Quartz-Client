# Rainbow wave

In **Shaders**, select `rainbow-wave.frag` in the fragment file field, then click
**Import to catalog**. Choose **Rainbow wave** from the catalog. Its stable ID is
`custom.rainbow_wave`, which can also be selected by scripts.

Adjust it under **Material parameters**:

| Control | Meaning |
| --- | --- |
| Angle | Direction in degrees: 0 moves right, 90 moves down, 180 moves left. |
| Speed | Rainbow cycles per second. Negative reverses motion; 0 freezes it. |
| Phase | Color offset, with 0 and 1 representing the same position in the cycle. |
| Density | Rainbow cycles across a keyboard width; higher values make narrower bands. |
| Saturation | 0 is white; 1 gives full rainbow colors. |
| Brightness | Wave intensity from 0 to 1, before Quartz's global brightness. |
| Wave bend | Curves the bands; 0 produces straight bands. |
| Bend frequency | Number of bends per keyboard width, perpendicular to travel. |
| Media color blend | Blends toward the current media color when available. Defaults to 0. |

Defaults give a straight horizontal rainbow with one cycle across the keyboard,
moving at 0.12 cycles/second. Angle uses the keyboard's key spacing on both axes.
Caps Lock and Scroll Lock indicators continue to use your indicator settings.
Media blending also respects Quartz's global media blend setting.
