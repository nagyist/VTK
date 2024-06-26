#!/usr/bin/env python
"""
Create a HTG
without mask
during HTG build we build scalar, used Global Index implicit with SetGlobalIndexStart
SetGlobalIndexStart, one call by HT
"""
from vtkmodules.vtkCommonCore import (
    vtkDoubleArray,
    vtkLookupTable,
)
from vtkmodules.vtkCommonDataModel import (
    vtkHyperTreeGridNonOrientedCursor,
    vtkUniformHyperTreeGrid,
)
from vtkmodules.vtkFiltersGeneral import vtkShrinkFilter
from vtkmodules.vtkFiltersHyperTree import vtkHyperTreeGridGeometry
from vtkmodules.vtkRenderingCore import (
    vtkActor,
    vtkCamera,
    vtkDataSetMapper,
    vtkRenderWindow,
    vtkRenderWindowInteractor,
    vtkRenderer,
)
import vtkmodules.vtkInteractionStyle
import vtkmodules.vtkRenderingFreeType
import vtkmodules.vtkRenderingOpenGL2

htg = vtkUniformHyperTreeGrid()
htg.Initialize()

scalarArray = vtkDoubleArray()
scalarArray.SetName('scalar')
scalarArray.SetNumberOfValues(0)
htg.GetCellData().AddArray(scalarArray)
htg.GetCellData().SetActiveScalars('scalar')

htg.SetDimensions([4, 3, 1])
htg.SetBranchFactor(2)
htg.SetOrigin([-1., -1., -2])
htg.SetGridScale([1., 1., 1.])

# Let's split the various trees
cursor = vtkHyperTreeGridNonOrientedCursor()
offsetIndex = 0

# ROOT CELL 0
htg.InitializeNonOrientedCursor(cursor, 0, True)
cursor.SetGlobalIndexStart(offsetIndex)

idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 1)

cursor.SubdivideLeaf()

# ROOT CELL 0/[0-3]
cursor.ToChild(0)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 7)
cursor.ToParent()

cursor.ToChild(1)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 8)
cursor.ToParent()

cursor.ToChild(2)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 9)
cursor.ToParent()

cursor.ToChild(3)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 10)
cursor.ToParent()

# ROOT CELL 0

offsetIndex += cursor.GetTree().GetNumberOfVertices()

# ROOT CELL 1
htg.InitializeNonOrientedCursor(cursor, 1, True)
cursor.SetGlobalIndexStart(offsetIndex)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 2)

offsetIndex += cursor.GetTree().GetNumberOfVertices()

# ROOT CELL 2
htg.InitializeNonOrientedCursor(cursor, 2, True)
cursor.SetGlobalIndexStart(offsetIndex)

idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 3)

cursor.SubdivideLeaf()

# ROOT CELL 2/[0-3]
cursor.ToChild(0)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 11)
cursor.ToParent()

cursor.ToChild(1)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 12)
cursor.ToParent()

cursor.ToChild(2)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 13)
cursor.ToParent()

cursor.ToChild(3)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 14)
cursor.ToParent()

# ROOT CELL 2

offsetIndex += cursor.GetTree().GetNumberOfVertices()

# ROOT CELL 3
htg.InitializeNonOrientedCursor(cursor, 3, True)
cursor.SetGlobalIndexStart(offsetIndex)

idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 4)

offsetIndex += cursor.GetTree().GetNumberOfVertices()

# ROOT CELL 4
htg.InitializeNonOrientedCursor(cursor, 4, True)
cursor.SetGlobalIndexStart(offsetIndex)

idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 5)

cursor.SubdivideLeaf()

# ROOT CELL 4/[0-3]
cursor.ToChild(0)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 15)
cursor.ToParent()

cursor.ToChild(1)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 16)
cursor.ToParent()

cursor.ToChild(2)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 17)
cursor.ToParent()

cursor.ToChild(3)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 18)

cursor.SubdivideLeaf()

# ROOT CELL 4/3/[0-3]
cursor.ToChild(0)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 19)

cursor.SubdivideLeaf()

# ROOT CELL 4/3/0/[0-3]
cursor.ToChild(0)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 23)
cursor.ToParent()

cursor.ToChild(1)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 24)
cursor.ToParent()

cursor.ToChild(2)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 25)
cursor.ToParent()

cursor.ToChild(3)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 26)
cursor.ToParent()

# ROOT CELL 4/3/0
cursor.ToParent()

# ROOT CELL 4/3/[1-3]
cursor.ToChild(1)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 20)
cursor.ToParent()

cursor.ToChild(2)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 21)
cursor.ToParent()

cursor.ToChild(3)
idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 22)
cursor.ToParent()

# ROOT CELL 4/3

cursor.ToParent()

# ROOT CELL 4

offsetIndex += cursor.GetTree().GetNumberOfVertices()

# ROOT CELL 5
htg.InitializeNonOrientedCursor(cursor, 5, True)
cursor.SetGlobalIndexStart(offsetIndex)

idx = cursor.GetGlobalNodeIndex()
scalarArray.InsertTuple1(idx, 6)

print('#',scalarArray.GetNumberOfTuples())
print('DataRange: ',scalarArray.GetRange())

# Geometries
geometry = vtkHyperTreeGridGeometry()
geometry.SetInputData(htg)
print('With Geometry Filter (HTG to NS)')

# Shrink Filter
if True:
  print('With Shrink Filter (NS)')
  # In 3D, the shrink shouldn't be done on the geometry because it only represents the skin
  shrink = vtkShrinkFilter()
  shrink.SetInputConnection(geometry.GetOutputPort())
  shrink.SetShrinkFactor(.8)
else:
  print('No Shrink Filter (NS)')
  shrink = geometry

# LookupTable
lut = vtkLookupTable()
lut.SetHueRange(0.66, 0)
lut.UsingLogScale()
lut.Build()

# Mappers
mapper = vtkDataSetMapper()
mapper.SetInputConnection(shrink.GetOutputPort())

mapper.SetLookupTable(lut)
mapper.SetColorModeToMapScalars()
mapper.SetScalarModeToUseCellFieldData()
mapper.SelectColorArray('scalar')
dataRange = [1,26] # Forced for compare with 2DMask
mapper.SetScalarRange(dataRange[0], dataRange[1])

# Actors
actor = vtkActor()
actor.SetMapper(mapper)

# Camera
bd = htg.GetBounds()
camera = vtkCamera()
camera.SetClippingRange(1., 100.)
focal = []
for i in range(3):
  focal.append(bd[ 2 * i ] + (bd[ 2 * i + 1 ] - bd[ 2 * i]) / 2.)
camera.SetFocalPoint(focal)
camera.SetPosition(focal[0], focal[1], focal[2] + 4.)

# Renderer
renderer = vtkRenderer()
renderer.SetActiveCamera(camera)
renderer.AddActor(actor)

# Render window
renWin = vtkRenderWindow()
renWin.AddRenderer(renderer)
renWin.SetSize(600, 400)

# Render window interactor
iren = vtkRenderWindowInteractor()
iren.SetRenderWindow(renWin)

# render the image
renWin.Render()
# iren.Start()

# prevent the tk window from showing up then start the event loop
# --- end of script --
