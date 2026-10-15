# Carriage redesign

This document records how `Carriage_fixed.FCStd` was analyzed and rebuilt as a
native FreeCAD model that does not import `Carriage.stl`.

The generated model is:

```text
Carriage_standard.FCStd
```

The reproducible FreeCAD recipe is:

```text
scripts/freecad/redesign_carriage.py
```

## Goal

The original file is an imported `Carriage.stl` converted to a solid and then
modified with cylinders:

```text
Carriage_Solid
  + Plug_PlusY
  + Plug_MinusY
  - Hole6mm_PlusY
  - Hole6mm_MinusY
```

The functional change is to reduce the rail bore for a 6 mm rail with 0.3 mm
clearance. The resulting design diameter is therefore:

```text
6.0 mm rail + 0.3 mm clearance = 6.3 mm bore
radius = 3.15 mm
```

The request said `0.3m`; this redesign treats that as `0.3 mm`, because meters
are not physically meaningful for this part.

## Source inspection

The FreeCAD document was inspected directly, not through visual guessing.

Commands used:

```bash
unzip -l Carriage_fixed.FCStd
unzip -p Carriage_fixed.FCStd Document.xml
XDG_CACHE_HOME=/tmp/freecad-cache HOME=/tmp/freecad-home \
  freecadcmd -c "..."
```

The document contains these relevant objects:

```text
Carriage_Mesh                 Mesh::Feature
Carriage_Solid                Part::Feature
Plug_PlusY                    Part::Cylinder
Hole6mm_PlusY                 Part::Cylinder
Plug_MinusY                   Part::Cylinder
Hole6mm_MinusY                Part::Cylinder
Carriage_PlugsFused           Part::MultiFuse
Carriage_Fixed_Cut0           Part::Cut
Carriage_Fixed_Cut1           Part::Cut, label Carriage_Fixed
```

The final imported/boolean model has:

```text
bounding box X: -28.0 .. 28.0 mm
bounding box Y: -25.425 .. 25.4242 mm
bounding box Z: -10.9969 .. 5.2 mm
faces: 2962
solids: 1
volume: 14801.676 mm^3
```

## Extracted dimensions

Critical dimensions from `Carriage_fixed.FCStd`:

| Feature | Value |
| --- | ---: |
| Overall X length | 56.0 mm |
| Overall Y width | 50.8492 mm |
| Overall Z range | -10.9969 .. 5.2 mm |
| Rail axis direction | X axis |
| Rail sleeve length | 49.0 mm |
| Rail sleeve X start/end | -24.5 .. 24.5 mm |
| Plus rail center | X -24.5 placement, Y 19.925/19.93, Z -5.5 |
| Minus rail center | X -24.5 placement, Y -19.925/-19.93, Z -5.5 |
| Plug radius | 4.6 mm |
| Plug diameter | 9.2 mm |
| Final bore radius | 3.15 mm |
| Final bore diameter | 6.3 mm |

The two final cylindrical bore faces detected in the finished model are:

```text
r = 3.15, center ~= (-24.5, -19.93, -5.5), axis = X
r = 3.15, center ~= (-24.5,  19.93, -5.5), axis = X
```

Additional top-side features were approximated from mesh level analysis:

| Feature | Approximate value |
| --- | ---: |
| Four mount centers | X +/-11.25, Y +/-11.24 mm |
| Mount through radius | 1.75 mm |
| Mount counterbore radius | 3.25 mm |
| Rear/center cable holes | radius 4.0 mm |
| Small front hole | radius 1.75 mm |
| Raised pad top | Z 5.2 mm |

These top features are not treated as exact reverse-engineering. They preserve
the useful layout for a native redesign without reproducing every triangle of
the STL.

## Native FreeCAD reconstruction

The native model is built from editable FreeCAD primitives:

```text
BasePlate                 Part::Box
RailSleeve_PlusY          Part::Cylinder
RailSleeve_MinusY         Part::Cylinder
TopPad_Left               Part::Box
TopPad_Right              Part::Box
TopPad_Rear               Part::Box
Body_Fused                Part::MultiFuse
RailBore_6p3mm_PlusY      Part::Cylinder
RailBore_6p3mm_MinusY     Part::Cylinder
vertical hole cutters     Part::Cylinder
Cutters_Fused             Part::MultiFuse
Carriage_Redesigned       Part::Cut
```

No STL is imported by the recipe.

The final generated solid validates as:

```text
bounding box X: -28.0 .. 28.0 mm
bounding box Y: -24.525 .. 24.525 mm
bounding box Z: -10.1 .. 5.2 mm
faces: 43
solids: 1
volume: 16881.0538 mm^3
rail bore radius: 3.15 mm
rail bore centers: Y +/-19.93, Z -5.5
```

The Y and lower Z envelope are intentionally driven by the functional rail
sleeves instead of preserving the imported STL's small mesh-only extrema.

## Rebuild steps

1. Open `Carriage_fixed.FCStd` as a zip archive and read `Document.xml`.
2. Identify whether the model is native or imported. In this case, the base is
   `Carriage_Solid`, created from the mesh.
3. Extract the final boolean tree:
   `Carriage_Solid + plugs - rail bores`.
4. Read the primitive cylinder properties:
   radius, height, placement and rotation.
5. Confirm the final bore by scanning cylindrical faces of
   `Carriage_Fixed_Cut1`.
6. Decide which dimensions are functional:
   rail axis, bore diameter, clearance, sleeve length and sleeve location.
7. Recreate the part as CSG primitives:
   base plate, rail sleeves, top pads and hole cutters.
8. Fuse all additive primitives.
9. Fuse all cutters.
10. Cut once to produce `Carriage_Redesigned`.
11. Save as `Carriage_standard.FCStd`.
12. Reopen the generated file and verify bounding box, solid count and rail bore
    radius.

## Automation plan

A future assisted redesign program can follow this minimum pipeline:

```text
input FCStd/STL
  -> inspect FCStd Document.xml
  -> detect imported mesh vs native feature tree
  -> extract bounding box and existing boolean primitives
  -> detect cylindrical faces near target rail axis
  -> classify functional dimensions
  -> ask user/LLM to approve ambiguous approximations
  -> emit FreeCAD Python CSG recipe
  -> run FreeCADCmd
  -> validate resulting solid and critical dimensions
```

The program should not attempt to convert every mesh triangle into parametric
features. That produces an editable-looking file with no useful design intent.
The useful target is a small set of named, functional primitives with explicit
dimensions and validation checks.

## Open decisions

- The exact nonfunctional contour of the imported carriage was not recreated.
- Top holes and pads are approximated from mesh levels and should be checked
  against hardware before print.
- If the carriage must mate with another exact STL surface, add that mating
  surface as a measured constraint instead of importing the whole mesh.
