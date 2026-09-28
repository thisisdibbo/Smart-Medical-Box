# 3D-printed parts

Seven STL files make up the enclosure. Dimensions are the raw bounding boxes from the meshes, in
millimetres, so allow for your printer's tolerance on the fits.

| Part | File | Bounding box | Triangles | Qty |
|------|------|:------------:|:---------:|:---:|
| Main chassis | `main-chassis.stl` | 231 × 354 × 143 | 9,746 | 1 |
| Top lid / cover | `lid-top-cover.stl` | 231 × 247 × 46 | 3,998 | 1 |
| Pill silo tube | `pill-silo-tube.stl` | 59 × 59 × 61 | 624 | 3 |
| Silo base hopper | `silo-base-hopper.stl` | 59 × 73 × 30 | 2,104 | 3 |
| Dispenser disc | `dispenser-disc.stl` | 54 × 53 × 9 | 1,204 | 3 |
| Motor / gearbox mount | `motor-gearbox-mount.stl` | 50 × 30 × 36 | 3,236 | 3 |
| Catch tray | `catch-tray.stl` | 50 × 51 × 21 | 2,310 | 1 |

Renders of each part: [`../../images/render/`](../../images/render)
CAD views and the exploded assembly: [`../../images/cad/`](../../images/cad)

## Printing

| | |
|---|---|
| Material | PLA is fine; PETG if the box will sit anywhere warm |
| Layer height | 0.2 mm, or 0.15 mm for the dispenser disc |
| Walls | 3 perimeters — the silo tubes take side load from the tablets |
| Infill | 20 % for structure, 40 %+ for the dispenser disc and motor mount |
| Supports | Only the main chassis needs them, under the silo wells |

The **main chassis** is 231 × 354 mm, which is larger than a 220 × 220 bed. Either print it on a
300 mm-class printer, or split it in your slicer and join the halves — the built unit in the
photos uses a split chassis with the seam running between the silo bank and the electronics bay.

## Fit notes

- The **dispenser disc** pocket is sized for one standard tablet. Print a test disc first and
  check your actual tablets drop cleanly and only one at a time — this is the part worth
  iterating on.
- The **motor mount** has a bracket with two screw holes and a bore for the gearbox shaft. Check
  the bore against your motor before committing to three copies.
- The **silo tube** sits inside the chassis well and drops onto the base hopper; the angled cut
  at the top is the refill opening.
- The **catch tray** takes the piezo disc underneath — glue the piezo flat to the underside of
  the floor so a falling tablet couples into it properly. A loose piezo gives unreliable
  detection and is the most common reason the motor keeps running past a pill.
