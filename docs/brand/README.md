# Credits Community logo

An open C of six linked nodes around a central one: a community of nodes reaching consensus. It is
the community fork's own mark, distinct from the official Credits logo and trademark.

| File | Use |
| --- | --- |
| `credits-community-logo.svg`, `.png` | Horizontal logo on light backgrounds |
| `credits-community-logo-dark.svg`, `.png` | Horizontal logo on dark backgrounds |
| `credits-community-icon.svg`, `-512.png`, `-128.png`, `-32.png` | Icon alone (avatars, favicons) |

Colours: teal `#14B8A6` to indigo `#6366F1`, text `#0F172A` (light) or `#F8FAFC` (dark).
The lettering is converted to paths from DejaVu Sans (Bitstream Vera licence), so the SVGs need no
font. `make_logo.py` regenerates every file (`python3 make_logo.py`, then `rsvg-convert` for PNGs).
