// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#define VTK_EPSILON 1e-10

// Subclass of vtkCell
// #include "vtkEmptyCell.h"
#include "vtkGenericCell.h"
#include "vtkLine.h"
#include "vtkPixel.h"
// #include "vtkPolygon.h"
// #include "vtkPolyLine.h"
// #include "vtkPolyVertex.h"
#include "vtkQuad.h"
#include "vtkTriangle.h"
// #include "vtkTriangleStrip.h"
#include "vtkVertex.h"

// Subclass of vtkCell3D
// #include "vtkConvexPointSet.h"
#include "vtkHexagonalPrism.h"
#include "vtkHexahedron.h"
#include "vtkPentagonalPrism.h"
#include "vtkPyramid.h"
#include "vtkTetra.h"
#include "vtkVoxel.h"
#include "vtkWedge.h"

// Subclass of vtkNonLinearCell
#include "vtkQuadraticEdge.h"
#include "vtkQuadraticHexahedron.h"
#include "vtkQuadraticPyramid.h"
#include "vtkQuadraticQuad.h"
#include "vtkQuadraticTetra.h"
#include "vtkQuadraticTriangle.h"
#include "vtkQuadraticWedge.h"

// Bi/Tri linear quadratic cells
#include "vtkBiQuadraticQuad.h"
#include "vtkBiQuadraticQuadraticHexahedron.h"
#include "vtkBiQuadraticQuadraticWedge.h"
#include "vtkBiQuadraticTriangle.h"
#include "vtkCubicLine.h"
#include "vtkQuadraticLinearQuad.h"
#include "vtkQuadraticLinearWedge.h"
#include "vtkTriQuadraticHexahedron.h"
#include "vtkTriQuadraticPyramid.h"

#include <vector>

template <class TCell>
int TestOneInterpolationDerivs(double eps = VTK_EPSILON)
{
  auto cell = vtkSmartPointer<TCell>::New();
  int numPts = cell->GetNumberOfPoints();
  int dim = cell->GetCellDimension();
  std::vector<double> derivs(dim * numPts);
  double* coords = cell->GetParametricCoords();
  int r = 0;
  for (int i = 0; i < numPts; ++i)
  {
    double* point = coords + 3 * i;
    double sum = 0.;
    cell->InterpolateDerivs(point, derivs.data()); // static function
    for (int j = 0; j < dim * numPts; j++)
    {
      sum += derivs[j];
    }
    if (fabs(sum) > eps)
    {
      ++r;
    }
  }

  // Let's test zero condition on the center point:
  double center[3];
  cell->GetParametricCenter(center);
  cell->InterpolateDerivs(center, derivs.data()); // static function
  double sum = 0.;
  for (int j = 0; j < dim * numPts; j++)
  {
    sum += derivs[j];
  }
  if (fabs(sum) > eps)
  {
    ++r;
  }

  return r;
}

int TestInterpolationDerivs(int, char*[])
{
  int r = 0;

  // Subclasses of vtkCell3D
  // r += TestOneInterpolationDerivs<vtkEmptyCell>(); // not implemented
  // r += TestOneInterpolationDerivs<vtkGenericCell>(); // not implemented
  // r += TestOneInterpolationDerivs<vtkLine>();
  r += TestOneInterpolationDerivs<vtkPixel>();
  // r += TestOneInterpolationDerivs<vtkPolygon>(); // not implemented
  // r += TestOneInterpolationDerivs<vtkPolyLine>(); // not implemented
  // r += TestOneInterpolationDerivs<vtkPolyVertex>(); // not implemented
  r += TestOneInterpolationDerivs<vtkQuad>();
  r += TestOneInterpolationDerivs<vtkTriangle>();
  // r += TestOneInterpolationDerivs<vtkTriangleStrip>(); // not implemented
  // r += TestOneInterpolationDerivs<vtkVertex>();

  // Subclasses of vtkCell3D
  // r += TestOneInterpolationDerivs<vtkConvexPointSet>(); // not implemented
  r += TestOneInterpolationDerivs<vtkHexagonalPrism>();
  r += TestOneInterpolationDerivs<vtkHexahedron>();
  r += TestOneInterpolationDerivs<vtkPentagonalPrism>(1.e-05);
  r += TestOneInterpolationDerivs<vtkPyramid>();
  // r += TestOneInterpolationDerivs<vtkTetra>();
  r += TestOneInterpolationDerivs<vtkVoxel>();
  r += TestOneInterpolationDerivs<vtkWedge>();

  // Subclasses of vtkNonLinearCell
  r += TestOneInterpolationDerivs<vtkQuadraticEdge>();
  r += TestOneInterpolationDerivs<vtkQuadraticHexahedron>();
  r += TestOneInterpolationDerivs<vtkQuadraticPyramid>();
  r += TestOneInterpolationDerivs<vtkQuadraticQuad>();
  r += TestOneInterpolationDerivs<vtkQuadraticTetra>();
  r += TestOneInterpolationDerivs<vtkQuadraticTriangle>();
  r += TestOneInterpolationDerivs<vtkQuadraticWedge>();

  // Bi/Tri linear quadratic cells
  r += TestOneInterpolationDerivs<vtkBiQuadraticQuad>();
  r += TestOneInterpolationDerivs<vtkBiQuadraticQuadraticHexahedron>();
  r += TestOneInterpolationDerivs<vtkBiQuadraticQuadraticWedge>();
  r += TestOneInterpolationDerivs<vtkBiQuadraticTriangle>();
  r += TestOneInterpolationDerivs<vtkCubicLine>();
  r += TestOneInterpolationDerivs<vtkQuadraticLinearQuad>();
  r += TestOneInterpolationDerivs<vtkQuadraticLinearWedge>();
  r += TestOneInterpolationDerivs<vtkTriQuadraticHexahedron>();
  r += TestOneInterpolationDerivs<vtkTriQuadraticPyramid>();

  return r;
}
