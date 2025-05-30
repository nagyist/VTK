#!/usr/bin/env python
from vtkmodules.vtkFiltersCore import (
    vtkContourFilter,
    vtkMaskPoints,
)
from vtkmodules.vtkIOImage import vtkVolume16Reader
from vtkmodules.vtkRenderingCore import (
    vtkActor,
    vtkActor2D,
    vtkPolyDataMapper,
    vtkRenderWindow,
    vtkRenderWindowInteractor,
    vtkRenderer,
    vtkSelectVisiblePoints,
)
from vtkmodules.vtkRenderingLabel import vtkLabeledDataMapper
import vtkmodules.vtkInteractionStyle
import vtkmodules.vtkRenderingFreeType
import vtkmodules.vtkRenderingOpenGL2
from vtkmodules.util.misc import vtkGetDataRoot
VTK_DATA_ROOT = vtkGetDataRoot()

# demonstrate labeling of contour with scalar value
# Create the RenderWindow, Renderer and both Actors
#
ren1 = vtkRenderer()
renWin = vtkRenderWindow()
renWin.SetMultiSamples(0)
renWin.AddRenderer(ren1)
iren = vtkRenderWindowInteractor()
iren.SetRenderWindow(renWin)

# Read a slice and contour it
v16 = vtkVolume16Reader()
v16.SetDataDimensions(64, 64)
v16.GetOutput().SetOrigin(0.0, 0.0, 0.0)
v16.SetDataByteOrderToLittleEndian()
v16.SetFilePrefix(VTK_DATA_ROOT + "/Data/headsq/quarter")
v16.SetImageRange(45, 45)
v16.SetDataSpacing(3.2, 3.2, 1.5)

iso = vtkContourFilter()
iso.SetInputConnection(v16.GetOutputPort())
iso.GenerateValues(6, 500, 1150)
iso.Update()

numPts = iso.GetOutput().GetNumberOfPoints()

isoMapper = vtkPolyDataMapper()
isoMapper.SetInputConnection(iso.GetOutputPort())
isoMapper.ScalarVisibilityOn()
isoMapper.SetScalarRange(iso.GetOutput().GetScalarRange())
isoActor = vtkActor()
isoActor.SetMapper(isoMapper)

# Subsample the points and label them
mask = vtkMaskPoints()
mask.SetInputConnection(iso.GetOutputPort())
mask.SetOnRatio(numPts // 50)
mask.SetMaximumNumberOfPoints(50)
mask.RandomModeOn()

# Create labels for points - only show visible points
visPts = vtkSelectVisiblePoints()
visPts.SetInputConnection(mask.GetOutputPort())
visPts.SetRenderer(ren1)

ldm = vtkLabeledDataMapper()
ldm.SetInputConnection(mask.GetOutputPort())
#    ldm.SetLabelFormat("%g")
ldm.SetLabelModeToLabelScalars()

tprop = ldm.GetLabelTextProperty()
tprop.SetFontFamilyToArial()
tprop.SetFontSize(10)
tprop.SetColor(1, 0, 0)

contourLabels = vtkActor2D()
contourLabels.SetMapper(ldm)

# Add the actors to the renderer, set the background and size
#
ren1.AddViewProp(isoActor)
ren1.AddViewProp(contourLabels)
ren1.SetBackground(1, 1, 1)

renWin.SetSize(500, 500)

renWin.Render()
ren1.GetActiveCamera().Zoom(1.5)

# render the image
#
#iren.Start()
