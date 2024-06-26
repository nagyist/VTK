set(LagrangeGeometricOperations_ARGS
  -D DATA{../Data/Input/Elements.vtu}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-Boundary.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-Contour.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-Cut.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-Clip.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-StabR.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-StabS.png,:}
  -V DATA{../Data/Baseline/LagrangeGeometricOperations-StabT.png,:}
)


vtk_module_test_data(
  Data/convexPointSet.vtu)

vtk_add_test_python(
  dataSetSurfaceFilter.py
  dataSetSurfaceFilter2.py,NO_DATA,NO_VALID,NO_OUTPUT
  geomFilter.py
  geometryFilterConvexPointSet.py,NO_VALID,NO_OUTPUT
  LagrangeGeometricOperations.py,JUST_VALID
  officeStreamPoints.py
  rectGrid.py
  structuredGridBlanking.py
  TestAttributeSmoothingFilter.py
  TestAttributeSmoothingFilter2.py
  TestGeometryFilter.py
  TestGeometryFilter2.py
  TestGhostPoints.py,NO_DATA,NO_VALID,NO_OUTPUT
  TestMarkBoundaryFilter.py,
  TestMarkBoundaryFilter2.py
)
