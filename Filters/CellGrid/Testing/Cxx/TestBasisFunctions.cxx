// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkCellArray.h"
#include "vtkCellAttribute.h"
#include "vtkCellGrid.h"
#include "vtkDGArrayOutputAccessor.h"
#include "vtkDGArraysInputAccessor.h"
#include "vtkDGCell.h"
#include "vtkDGInterpolateCalculator.h"
#include "vtkDGOperation.h"
#include "vtkDGOperation.txx"
#include "vtkDataSetAttributes.h"
#include "vtkDoubleArray.h"
#include "vtkFiltersCellGrid.h"
#include "vtkInterpolateCalculator.h"
#include "vtkNew.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkSmartPointer.h"
#include "vtkTypeFloat32Array.h"
#include "vtkTypeInt32Array.h"
#include "vtkVector.h"

#include "vtkXMLPolyDataWriter.h"

#include "vtkCellGridWriter.h"

#include "vtkDGEdge.h"
#include "vtkDGHex.h"
#include "vtkDGPyr.h"
#include "vtkDGQuad.h"
#include "vtkDGTet.h"
#include "vtkDGTri.h"
#include "vtkDGVert.h"
#include "vtkDGWdg.h"

#include "vtk_eigen.h"
#include VTK_EIGEN(Eigen)

#include <stdexcept>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

using namespace vtk::literals;

namespace
{

bool TestDeRhamBases(vtkCellGrid* grid, vtkDeRhamCell* drCell);

void test(bool condition, const std::string& msg)
{
  if (!condition)
  {
    std::cerr << "ERROR: " << msg << "\n";
    throw std::runtime_error(msg);
  }
}

bool nearly_eq(double aa, double bb, double tol = 1e-7)
{
  return std::abs(bb - aa) < tol;
}

// Test that \a dimension entries are the identity matrix (and the
// remaining entries are zero).
bool is_identitity(const double* gradient, int dimension)
{
  for (int ii = 0; ii < 3; ++ii)
  {
    for (int jj = 0; jj < 3; ++jj)
    {
      if (ii == jj)
      {
        if (ii < dimension)
        {
          if (!nearly_eq(1., gradient[3 * ii + jj]))
          {
            return false;
          }
        }
        else
        {
          if (!nearly_eq(0., gradient[3 * ii + jj]))
          {
            return false;
          }
        }
      }
      else if (!nearly_eq(0., gradient[3 * ii + jj]))
      {
        return false;
      }
    }
  }
  return true;
}

void AddCoordinates(vtkCellGrid* grid, vtkDGCell* dgCell)
{
  vtkStringToken cellTypeName = dgCell->GetClassName();

  auto* pcoords = dgCell->GetReferencePoints();
  vtkNew<vtkDoubleArray> xcoords;
  xcoords->DeepCopy(pcoords);
  xcoords->SetName("coords");
  auto* dsCoords = grid->GetAttributes("coordinates"_token);
  dsCoords->SetScalars(xcoords);

  vtkIdType nn = xcoords->GetNumberOfTuples();
  std::vector<vtkTypeInt64> connTuple;
  connTuple.resize(nn);
  for (vtkIdType ii = 0; ii < nn; ++ii)
  {
    connTuple[ii] = ii;
  }

  vtkNew<vtkTypeInt32Array> cellConn;
  cellConn->SetName("conn");
  cellConn->SetNumberOfComponents(nn);
  cellConn->SetNumberOfTuples(1);
  cellConn->SetIntegerTuple(0, connTuple.data());

  auto* dsConn = grid->GetAttributes(cellTypeName);
  dsConn->SetScalars(cellConn);

  auto& cellSpec = dgCell->GetCellSpec();
  cellSpec.Connectivity = cellConn;
  cellSpec.SourceShape = dgCell->GetShape();

  vtkNew<vtkCellAttribute> shapeAtt;
  vtkCellAttribute::CellTypeInfo shapeInfo;
  shapeInfo.DOFSharing = "coordinates"_token;
  shapeInfo.FunctionSpace = "HGRAD"_token;
  shapeInfo.Basis = "C"_token;
  shapeInfo.Order = 1;
  shapeInfo.ArraysByRole["connectivity"] = cellConn;
  shapeInfo.ArraysByRole["values"] = xcoords;
  shapeAtt->Initialize("shape", "ℝ³", 3);
  // Vertices do not admit an H(grad) C1 basis; use a constant basis.
  if (cellTypeName == "vtkDGVert"_token)
  {
    shapeInfo.FunctionSpace = "constant"_token;
    shapeInfo.Order = 0;
  }
  shapeAtt->SetCellTypeInfo(cellTypeName, shapeInfo);

  grid->SetShapeAttribute(shapeAtt);
}

bool EvaluateBasisFunctions(vtkCellGrid* grid, vtkDGCell* dgCell)
{
  std::cout << "Test basis evaluation for \"" << dgCell->GetClassName() << "\".\n";
  auto* shape = grid->GetShapeAttribute();
  auto shapeTags = dgCell->GetAttributeTags(shape, true);
  auto calc = dgCell->GetResponders()->AttributeCalculator<vtkInterpolateCalculator>(
    dgCell, shape, shapeTags);
  if (!calc)
  {
    return false;
  }

  auto* icalc = vtkDGInterpolateCalculator::SafeDownCast(calc);
  if (!icalc)
  {
    return false;
  }

  auto* pcoords = dgCell->GetReferencePoints();
  vtkIdType nn = pcoords->GetNumberOfTuples();
  vtkVector3d rst;
  vtkNew<vtkPolyData> pdata;
  vtkNew<vtkPoints> ppts;
  vtkNew<vtkCellArray> verts;
  ppts->SetData(pcoords);
  pdata->SetPoints(ppts);
  pdata->SetVerts(verts);

  vtkNew<vtkDoubleArray> ddr;
  ddr->SetName("d/dr");
  ddr->SetNumberOfComponents(3);
  ddr->SetNumberOfTuples(nn);

  vtkNew<vtkDoubleArray> dds;
  dds->SetName("d/ds");
  dds->SetNumberOfComponents(3);
  dds->SetNumberOfTuples(nn);

  vtkNew<vtkDoubleArray> ddt;
  ddt->SetName("d/dt");
  ddt->SetNumberOfComponents(3);
  ddt->SetNumberOfTuples(nn);

  pdata->GetPointData()->AddArray(ddr);
  pdata->GetPointData()->AddArray(dds);
  pdata->GetPointData()->AddArray(ddt);

  std::vector<double> value;
  value.resize(9);
  for (vtkIdType ii = 0; ii < nn; ++ii)
  {
    pcoords->GetTuple(ii, rst.GetData());
    icalc->EvaluateDerivative(/* cell */ 0, rst, value, 1e-5);

    verts->InsertNextCell(1, &ii);
    ddr->SetTuple(ii, value.data());
    dds->SetTuple(ii, value.data() + 3);
    ddt->SetTuple(ii, value.data() + 6);
  }

  std::string fname = vtkDGCell::GetShapeName(dgCell->GetShape()).Data();
  fname = "one-" + fname + "-gradients.vtp";
  vtkNew<vtkXMLPolyDataWriter> wri;
  wri->SetDataModeToAscii();
  wri->SetFileName(fname.c_str());
  wri->SetInputDataObject(0, pdata);
  wri->Write();

  auto shapeInfo = shape->GetCellTypeInfo(dgCell->GetClassName());
  auto op = dgCell->GetOperatorEntry("Basis", shapeInfo);
  if (!op)
  {
    std::cerr << "  ERROR: No basis operator for \"" << dgCell->GetClassName() << "\".\n";
    return false;
  }

  // Test basis evaluation for HGrad (and constant, for vertex) function space
  // Note: This only tests linear/constant shape functions.
  vtkNew<vtkIdTypeArray> cellId;
  cellId->SetNumberOfTuples(nn);
  cellId->FillComponent(0, 0);
  vtkNew<vtkDoubleArray> result;
  result->SetNumberOfComponents(3);
  result->SetNumberOfTuples(nn);
  vtkNew<vtkDoubleArray> pc2;
  pc2->DeepCopy(pcoords);
  vtkDGOperation<vtkDGArraysInputAccessor, vtkDGArrayOutputAccessor> shapeEvaluator;
  shapeEvaluator.Prepare(dgCell, shape, "Basis"_token);
  vtkDGArraysInputAccessor inIt(cellId, pc2);
  vtkDGArrayOutputAccessor outIt(result);
  shapeEvaluator.Evaluate(inIt, outIt, 0, nn);
  std::cout << "  basis   ii: (r,s,t) → (x,y,z)\n";
  std::array<double, 3> params;
  std::vector<double> rval(3, 0.);
  for (vtkIdType ii = 0; ii < nn; ++ii)
  {
    pc2->GetTuple(ii, params.data());
    result->GetTuple(ii, rval.data());
    std::cout << "    " << ii << ":"
              << " (" << params[0] << " " << params[1] << " " << params[2] << ") →"
              << " (" << rval[0] << " " << rval[1] << " " << rval[2] << ")\n";
    test(nearly_eq(params[0], rval[0]) && nearly_eq(params[1], rval[1]) &&
        nearly_eq(params[2], rval[2]),
      "Element that matches reference element should have identity transform.");
  }

  auto grop = dgCell->GetOperatorEntry("BasisGradient", shapeInfo);
  if (!grop)
  {
    std::cerr << "  ERROR: No gradient operator for \"" << dgCell->GetClassName() << "\".\n";
    return false;
  }

  // Test gradient evaluation for HGrad (and constant, for vertex) function space
  // Note: This only tests linear/constant shape functions.
  vtkNew<vtkDoubleArray> gradient;
  gradient->SetNumberOfComponents(9);
  gradient->SetNumberOfTuples(nn);
  vtkDGOperation<vtkDGArraysInputAccessor, vtkDGArrayOutputAccessor> shapeGradientEvaluator;
  shapeGradientEvaluator.Prepare(dgCell, shape, "BasisGradient"_token);
  inIt.Restart();
  vtkDGArrayOutputAccessor gradIt(gradient);
  shapeGradientEvaluator.Evaluate(inIt, gradIt, 0, nn);
  std::cout << "  gradient ii: (r,s,t) → ∇(r,s,t)\n";
  std::vector<double> gval(9, 0.0);
  for (vtkIdType ii = 0; ii < nn; ++ii)
  {
    pc2->GetTuple(ii, params.data());
    gradient->GetTuple(ii, gval.data());
    std::cout << "    " << ii << ":"
              << " (" << params[0] << " " << params[1] << " " << params[2] << ") →"
              << " ((" << gval[0] << " " << gval[1] << " " << gval[2] << ")"
              << " (" << gval[3] << " " << gval[4] << " " << gval[5] << ")"
              << " (" << gval[6] << " " << gval[7] << " " << gval[8] << "))\n";
    test(is_identitity(gval.data(), dgCell->GetDimension()),
      "Cell gradient should be identity for each parametric dimension.");
  }

  if (auto* deRhamCell = vtkDeRhamCell::SafeDownCast(dgCell))
  {
    if (!TestDeRhamBases(grid, deRhamCell))
    {
      return false;
    }
  }

  return true;
}

void AddDeRhamFields(vtkCellGrid* grid, vtkDeRhamCell* drCell,
  std::vector<vtkCellAttribute*>& divFieldP, std::vector<vtkCellAttribute*>& curlFieldP)
{
  // Construct a div and a curl attribute for each face/edge, respectively.
  int numDivSides = drCell->GetNumberOfSidesOfDimension(drCell->GetDimension() - 1);
  for (int ii = 0; ii < numDivSides; ++ii)
  {
    std::ostringstream fname;
    fname << "div" << ii;
    vtkNew<vtkCellAttribute> divField;
    divField->Initialize(fname.str(), "ℝ³", 3);

    std::string divGroupName = (drCell->GetDimension() == 3 ? "faces of " : "edges of ");
    divGroupName += drCell->GetClassName();
    auto* divGroup = grid->GetAttributes(divGroupName);

    int divComp = drCell->GetNumberOfSidesOfDimension(drCell->GetDimension() - 1);

    // Create an empty tuple.
    std::vector<double> tuple(divComp, 0.0);
    // Set the ii-th value to 1.0:
    tuple[ii] = 1.0;

    vtkNew<vtkDoubleArray> divCoeff;
    divCoeff->SetName(fname.str().c_str());
    divCoeff->SetNumberOfComponents(divComp);
    divCoeff->SetNumberOfTuples(1);
    divCoeff->SetTuple(0, tuple.data());
    divGroup->AddArray(divCoeff);

    vtkCellAttribute::CellTypeInfo divTypeInfo;
    divTypeInfo.FunctionSpace = "HDIV"_token;
    divTypeInfo.Basis = "I"_token;
    divTypeInfo.Order = 1;
    divTypeInfo.ArraysByRole["values"_token] = divCoeff;

    divField->SetCellTypeInfo(drCell->GetClassName(), divTypeInfo);
    grid->AddCellAttribute(divField);
    divFieldP.push_back(divField);
  }

  int numCurlSides = drCell->GetNumberOfSidesOfDimension(1);
  for (int ii = 0; ii < numCurlSides; ++ii)
  {
    std::ostringstream fname;
    fname << "curl" << ii;
    vtkNew<vtkCellAttribute> curlField;
    curlField->Initialize(fname.str(), "ℝ³", 3);
    std::string curlGroupName = "edges of ";
    curlGroupName += drCell->GetClassName();
    auto* curlGroup = grid->GetAttributes(curlGroupName);

    int curlComp = drCell->GetNumberOfSidesOfDimension(1);

    // Create an empty tuple.
    std::vector<double> tuple(curlComp, 0.0);
    // Set the ii-th value to 1.0:
    tuple[ii] = 1.0;

    vtkNew<vtkDoubleArray> curlCoeff;
    curlCoeff->SetName(fname.str().c_str());
    curlCoeff->SetNumberOfComponents(curlComp);
    curlCoeff->SetNumberOfTuples(1);
    curlCoeff->SetTuple(0, tuple.data());
    curlGroup->AddArray(curlCoeff);

    vtkCellAttribute::CellTypeInfo curlTypeInfo;
    curlTypeInfo.FunctionSpace = "HCURL"_token;
    curlTypeInfo.Basis = "I"_token;
    curlTypeInfo.Order = 1;
    curlTypeInfo.ArraysByRole["values"_token] = curlCoeff;

    curlField->SetCellTypeInfo(drCell->GetClassName(), curlTypeInfo);
    grid->AddCellAttribute(curlField);
    curlFieldP.push_back(curlField);
  }
}

void AddSidePoint(vtkDGCell* dgCell, int sideId, vtkPolyData* polydata)
{
  vtkVector3d pp = dgCell->GetParametricCenterOfSide(sideId);
  vtkIdType ptId = polydata->GetPoints()->InsertNextPoint(pp.GetData());
  polydata->GetVerts()->InsertNextCell(1, &ptId);
}

void AddMidSidePoints(vtkCellGrid* grid, vtkDeRhamCell* drCell, vtkPolyData* polydata)
{
  (void)grid;
  // Always add a mid-cell point.
  // if (drCell->GetDimension() == 2)
  {
    AddSidePoint(drCell, -1, polydata);
  }
  int nst = drCell->GetNumberOfSideTypes();
  for (int ii = 0; ii < nst; ++ii)
  {
    auto sideRange = drCell->GetSideRangeForType(ii);
    for (int ss = sideRange.first; ss < sideRange.second; ++ss)
    {
      AddSidePoint(drCell, ss, polydata);
    }
  }
}

bool TestDeRhamBases(vtkCellGrid* grid, vtkDeRhamCell* drCell)
{
  // Add curl and div fields to the \a grid
  std::vector<vtkCellAttribute*> divFields;
  std::vector<vtkCellAttribute*> curlFields;
  AddDeRhamFields(grid, drCell, divFields, curlFields);

  // Create a poly-data and add points related to the reference cell.
  vtkNew<vtkPolyData> testPoints;
  vtkNew<vtkPoints> points;
  vtkNew<vtkCellArray> verts;
  testPoints->SetPoints(points);
  testPoints->SetVerts(verts);
  AddMidSidePoints(grid, drCell, testPoints);
  vtkNew<vtkDoubleArray> mspt;
  mspt->DeepCopy(points->GetData());

  auto* shapeField = grid->GetShapeAttribute();
  auto shapeTypeInfo = shapeField->GetCellTypeInfo(drCell->GetClassName());
  auto shapeGradOp = drCell->GetOperatorEntry("BasisGradient"_token, shapeTypeInfo);
  if (!shapeGradOp)
  {
    std::cerr << "  ERROR: No shape-basis operator for \"" << drCell->GetClassName() << "\".\n";
    return false;
  }
  vtkIdType nn = testPoints->GetNumberOfVerts();
  vtkNew<vtkIdTypeArray> cellId;
  cellId->SetNumberOfTuples(nn);
  cellId->FillComponent(0, 0);
  vtkNew<vtkDoubleArray> jacobians;
  jacobians->SetNumberOfComponents(9);
  jacobians->SetNumberOfTuples(nn);
  vtkDGOperation<vtkDGArraysInputAccessor, vtkDGArrayOutputAccessor> shapeEvaluator;
  shapeEvaluator.Prepare(drCell, shapeField, "BasisGradient"_token);
  vtkDGArraysInputAccessor inIt(cellId, mspt);
  vtkDGArrayOutputAccessor outIt(jacobians);
  shapeEvaluator.Evaluate(inIt, outIt, 0, nn);

  // Test evaluation for HCurl and HDiv function spaces.
  // Note: This only tests linear shape functions and DG I1 curl-/div-attributes.

  int ii = 0;
  for (const auto& divField : divFields)
  {
    vtkDGOperation<vtkDGArraysInputAccessor, vtkDGArrayOutputAccessor> fieldEvaluator(
      drCell, divField, "Basis"_token);
    auto divTypeInfo = divField->GetCellTypeInfo(drCell->GetClassName());
    auto divOp = drCell->GetOperatorEntry("Basis"_token, divTypeInfo);
    if (!divOp)
    {
      std::cerr << "  ERROR: No div-basis operator for \"" << drCell->GetClassName() << "\".\n";
      return false;
    }

    std::ostringstream dname;
    dname << "divf_" << ii << "(r,s,t)";
    vtkNew<vtkDoubleArray> divVals;
    divVals->SetNumberOfComponents(divOp.OperatorSize);
    divVals->SetNumberOfTuples(nn);
    divVals->SetName(dname.str().c_str());
    std::cout << "  " << divField->GetName().Data() << " ii: (r,s,t) → ∇·f(r,s,t)\n";
    inIt.Restart();
    vtkDGArrayOutputAccessor divIt(divVals);
    fieldEvaluator.Evaluate(inIt, divIt, 0, nn);
    testPoints->GetPointData()->AddArray(divVals);

    vtkVector3d d3;
    std::array<double, 3> params;
    for (vtkIdType pp = 0; pp < nn; ++pp)
    {
      mspt->GetTuple(pp, params.data());
      divVals->GetTuple(pp, d3.GetData());
#if 0
      if (drCell->GetDimension() == 3)
      {
        d3 = vtkVector3d(divVals[3 * pp], divVals[3 * pp + 1], divVals[3 * pp + 2]);
      }
      // Enable this once 2-d cells have divOp/curlOp.OperatorSize == 2.
      else
      {
        d3 = vtkVector3d(divVals[2 * pp], divVals[2 * pp + 1], 0.);
      }
#endif
      std::cout << "    " << pp << ":"
                << " (" << params[0] << " " << params[1] << " " << params[2] << ") →"
                << " " << d3 << "\n";
    }

    ++ii;
  }

  ii = 0;
  for (const auto& curlField : curlFields)
  {
    auto curlTypeInfo = curlField->GetCellTypeInfo(drCell->GetClassName());
    auto curlOp = drCell->GetOperatorEntry("Basis"_token, curlTypeInfo);
    if (!curlOp)
    {
      std::cerr << "  ERROR: No curl-basis operator for \"" << drCell->GetClassName() << "\".\n";
      return false;
    }

    std::ostringstream cname;
    cname << "curlf_" << ii << "(r,s,t)";
    vtkNew<vtkDoubleArray> curlVals;
    curlVals->SetName(cname.str().c_str());
    curlVals->SetNumberOfComponents(curlOp.OperatorSize);
    curlVals->SetNumberOfTuples(nn);
    std::cout << "  " << curlField->GetName().Data() << " ii: (r,s,t) → ∇×f(r,s,t)\n";
    vtkDGOperation<vtkDGArraysInputAccessor, vtkDGArrayOutputAccessor> fieldEvaluator(
      drCell, curlField, "Basis"_token);
    inIt.Restart();
    vtkDGArrayOutputAccessor curlIt(curlVals);
    fieldEvaluator.Evaluate(inIt, curlIt, 0, nn);
    testPoints->GetPointData()->AddArray(curlVals);

    vtkVector3d c3;
    std::array<double, 3> params;
    for (vtkIdType pp = 0; pp < nn; ++pp)
    {
      mspt->GetTuple(pp, params.data());
      curlVals->GetTuple(pp, c3.GetData());
#if 0
      if (drCell->GetDimension() == 3)
      {
        c3 = vtkVector3d(curlVals[3 * pp], curlVals[3 * pp + 1], curlVals[3 * pp + 2]);
      }
      // Enable this once 2-d cells have divOp/curlOp.OperatorSize == 2.
      else
      {
        c3 = vtkVector3d(curlVals[2 * pp], curlVals[2 * pp + 1], 0.);
      }
#endif
      std::cout << "    " << pp << ":"
                << " (" << params[0] << " " << params[1] << " " << params[2] << ") →"
                << " " << c3 << "\n";
    }

    ++ii;
  }

  std::string fname = vtkDGCell::GetShapeName(drCell->GetShape()).Data();
  fname = "one-" + fname + "-div+curl.vtp";
  vtkNew<vtkXMLPolyDataWriter> wri;
  wri->SetDataModeToAscii();
  wri->SetFileName(fname.c_str());
  wri->SetInputDataObject(0, testPoints);
  wri->Write();

  return true;
}

template <typename CellType>
bool TestDGCellType()
{
  bool ok = true;
  vtkNew<vtkCellGrid> grid;
  vtkNew<CellType> dgCell;
  // Create a grid with a single cell (of type CellType) whose world-space
  // coordinates are exactly its reference-space coordinates.
  if (!grid->AddCellMetadata(dgCell.GetPointer()))
  {
    return false;
  }
  AddCoordinates(grid, dgCell);
  ok &= EvaluateBasisFunctions(grid, dgCell);

  std::string fname = vtkDGCell::GetShapeName(dgCell->GetShape()).Data();
  fname = "one-" + fname + ".dg";
  vtkNew<vtkCellGridWriter> wri;
  wri->SetFileName(fname.c_str());
  wri->SetInputDataObject(0, grid);
  wri->Write();
  return ok;
}

}

int TestBasisFunctions(int vtkNotUsed(argc), char* vtkNotUsed(argv)[])
{
#if defined(_WIN32) || defined(_WIN64)
  SetConsoleOutputCP(CP_UTF8); // Treat strings printed to std::cout as UTF-8.
#endif
  vtkFiltersCellGrid::RegisterCellsAndResponders();

  // clang-format off
  if (
    !TestDGCellType<vtkDGEdge>() ||
    !TestDGCellType<vtkDGHex>() ||
    !TestDGCellType<vtkDGPyr>() ||
    !TestDGCellType<vtkDGQuad>() ||
    !TestDGCellType<vtkDGTet>() ||
    !TestDGCellType<vtkDGTri>() ||
    !TestDGCellType<vtkDGVert>() ||
    !TestDGCellType<vtkDGWdg>())
  {
    return EXIT_FAILURE;
  }
  // clang-format on

  return EXIT_SUCCESS;
}
