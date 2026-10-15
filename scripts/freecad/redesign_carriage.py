import os

import FreeCAD


MM = 1.0


def placement(x, y, z, yaw=0, pitch=0, roll=0):
    return FreeCAD.Placement(
        FreeCAD.Vector(x, y, z),
        FreeCAD.Rotation(yaw, pitch, roll),
    )


def add_box(doc, name, length, width, height, x, y, z):
    obj = doc.addObject("Part::Box", name)
    obj.Length = length * MM
    obj.Width = width * MM
    obj.Height = height * MM
    obj.Placement = placement(x, y, z)
    return obj


def add_cylinder(doc, name, radius, height, x, y, z, yaw=0, pitch=0, roll=0):
    obj = doc.addObject("Part::Cylinder", name)
    obj.Radius = radius * MM
    obj.Height = height * MM
    obj.Placement = placement(x, y, z, yaw, pitch, roll)
    return obj


doc = FreeCAD.newDocument("Carriage_standard")

# Extracted from Carriage_fixed.FCStd:
# - envelope: X 56.0, Y 50.8492, Z -10.9969..5.2
# - rail sleeve centers: Y +/-19.925, Z -5.5
# - rail sleeve plug radius: 4.6
# - final rail bore radius: 3.15 (6.0 mm rail + 0.3 mm clearance)
# - rail cylinder length: 49.0, starting at X -24.5 and ending at X +24.5
base = add_box(doc, "BasePlate", 56.0, 39.86, 6.5, -28.0, -19.93, -3.5)

rail_plus = add_cylinder(doc, "RailSleeve_PlusY", 4.6, 49.0, -24.5, 19.925, -5.5, 0, 90, 0)
rail_minus = add_cylinder(doc, "RailSleeve_MinusY", 4.6, 49.0, -24.5, -19.925, -5.5, 0, 90, 0)

left_pad = add_box(doc, "TopPad_Left", 7.0, 20.0, 2.2, -28.0, -10.0, 3.0)
right_pad = add_box(doc, "TopPad_Right", 7.0, 20.0, 2.2, 21.0, -10.0, 3.0)
rear_pad = add_box(doc, "TopPad_Rear", 20.0, 7.5, 2.2, -10.0, -23.425, 3.0)

body = doc.addObject("Part::MultiFuse", "Body_Fused")
body.Shapes = [base, rail_plus, rail_minus, left_pad, right_pad, rear_pad]
body.Refine = True

rail_hole_plus = add_cylinder(doc, "RailBore_6p3mm_PlusY", 3.15, 58.0, -29.0, 19.93, -5.5, 0, 90, 0)
rail_hole_minus = add_cylinder(doc, "RailBore_6p3mm_MinusY", 3.15, 58.0, -29.0, -19.93, -5.5, 0, 90, 0)

# Functional top holes measured from the mesh top levels. These keep the
# standard model useful without attempting to reproduce every STL triangle.
vertical_cuts = [
    add_cylinder(doc, "CableHole_Rear_R4", 4.0, 12.0, 0.0, -9.5, -4.0),
    add_cylinder(doc, "CableHole_Center_R4", 4.0, 12.0, 0.0, 1.0, -4.0),
    add_cylinder(doc, "SmallHole_Front_R1p75", 1.75, 12.0, 0.0, 10.0, -4.0),
]

for x in (-11.25, 11.25):
    for y in (-11.24, 11.24):
        vertical_cuts.append(add_cylinder(doc, f"MountThrough_{x:g}_{y:g}", 1.75, 12.0, x, y, -4.0))
        vertical_cuts.append(add_cylinder(doc, f"MountCounterbore_{x:g}_{y:g}", 3.25, 3.0, x, y, 0.0))

cutters = doc.addObject("Part::MultiFuse", "Cutters_Fused")
cutters.Shapes = [rail_hole_plus, rail_hole_minus] + vertical_cuts
cutters.Refine = True

final = doc.addObject("Part::Cut", "Carriage_Redesigned")
final.Base = body
final.Tool = cutters
final.Refine = True

for obj in doc.Objects:
    obj.Visibility = obj is final

doc.recompute()
doc.saveAs(os.path.abspath("Carriage_standard.FCStd"))
