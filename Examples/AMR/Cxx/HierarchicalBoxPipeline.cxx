// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
// This example demonstrates how hierarchical box (uniform rectilinear)
// AMR datasets can be processed using the vtkOverlappingAMR class.
//
// The command line arguments are:
// -I        => run in interactive mode; unless this is used, the program will
//              not allow interaction and exit
// -D <path> => path to the data; the data should be in <path>/Data/

#include "vtkCamera.h"
#include "vtkCellDataToPointData.h"
#include "vtkCompositeDataGeometryFilter.h"
#include "vtkCompositeDataPipeline.h"
#include "vtkContourFilter.h"
#include "vtkDebugLeaks.h"
#include "vtkExtractLevel.h"
#include "vtkHierarchicalPolyDataMapper.h"
#include "vtkOutlineCornerFilter.h"
#include "vtkProperty.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRenderer.h"
#include "vtkShrinkPolyData.h"
#include "vtkTestUtilities.h"
#include "vtkXMLUniformGridAMRReader.h"

int main(int argc, char* argv[])
{
  // Standard rendering classes
  vtkRenderer* ren = vtkRenderer::New();
  vtkCamera* cam = ren->GetActiveCamera();
  cam->SetPosition(-5.1828, 5.89733, 8.97969);
  cam->SetFocalPoint(14.6491, -2.08677, -8.92362);
  cam->SetViewUp(0.210794, 0.95813, -0.193784);

  vtkRenderWindow* renWin = vtkRenderWindow::New();
  renWin->AddRenderer(ren);
  vtkRenderWindowInteractor* iren = vtkRenderWindowInteractor::New();
  iren->SetRenderWindow(renWin);

  char* cfname = vtkTestUtilities::ExpandDataFileName(argc, argv, "Data/chombo3d/chombo3d.vtm");

  vtkXMLUniformGridAMRReader* reader = vtkXMLUniformGridAMRReader::New();
  reader->SetFileName(cfname);
  delete[] cfname;

  // geometry filter
  vtkCompositeDataGeometryFilter* geom = vtkCompositeDataGeometryFilter::New();
  geom->SetInputConnection(0, reader->GetOutputPort(0));

  vtkShrinkPolyData* shrink = vtkShrinkPolyData::New();
  shrink->SetShrinkFactor(0.5);
  shrink->SetInputConnection(0, geom->GetOutputPort(0));

  // Rendering objects
  vtkHierarchicalPolyDataMapper* shMapper = vtkHierarchicalPolyDataMapper::New();
  shMapper->SetInputConnection(0, shrink->GetOutputPort(0));
  vtkActor* shActor = vtkActor::New();
  shActor->SetMapper(shMapper);
  shActor->GetProperty()->SetColor(0, 0, 1);
  ren->AddActor(shActor);

  // corner outline
  vtkOutlineCornerFilter* ocf = vtkOutlineCornerFilter::New();
  vtkCompositeDataPipeline* pipeline = vtkCompositeDataPipeline::New();
  ocf->SetExecutive(pipeline);
  pipeline->Delete();
  ocf->SetInputConnection(0, reader->GetOutputPort(0));

  // Rendering objects
  // This one is actually just a vtkPolyData so it doesn't need a hierarchical
  // mapper, but we use this one to test hierarchical mapper with polydata input
  vtkHierarchicalPolyDataMapper* ocMapper = vtkHierarchicalPolyDataMapper::New();
  ocMapper->SetInputConnection(0, ocf->GetOutputPort(0));
  vtkActor* ocActor = vtkActor::New();
  ocActor->SetMapper(ocMapper);
  ocActor->GetProperty()->SetColor(1, 0, 0);
  ren->AddActor(ocActor);

  // cell 2 point and contour
  vtkExtractLevel* el = vtkExtractLevel::New();
  el->SetInputConnection(0, reader->GetOutputPort(0));
  el->AddLevel(2);

  vtkCellDataToPointData* c2p = vtkCellDataToPointData::New();
  pipeline = vtkCompositeDataPipeline::New();
  c2p->SetExecutive(pipeline);
  pipeline->Delete();
  c2p->SetInputConnection(0, el->GetOutputPort(0));

  vtkContourFilter* contour = vtkContourFilter::New();
  pipeline = vtkCompositeDataPipeline::New();
  contour->SetExecutive(pipeline);
  pipeline->Delete();
  contour->SetInputConnection(0, c2p->GetOutputPort(0));
  contour->SetValue(0, -0.013);
  contour->SetInputArrayToProcess(0, 0, 0, vtkDataObject::FIELD_ASSOCIATION_POINTS, "phi");

  // Rendering objects
  vtkHierarchicalPolyDataMapper* contMapper = vtkHierarchicalPolyDataMapper::New();
  contMapper->SetInputConnection(0, contour->GetOutputPort(0));
  vtkActor* contActor = vtkActor::New();
  contActor->SetMapper(contMapper);
  contActor->GetProperty()->SetColor(1, 0, 0);
  ren->AddActor(contActor);

  ren->SetBackground(1, 1, 1);
  renWin->SetSize(300, 300);
  ren->ResetCamera();
  iren->Start();

  ocf->Delete();
  ocMapper->Delete();
  ocActor->Delete();
  c2p->Delete();
  contour->Delete();
  contMapper->Delete();
  contActor->Delete();
  el->Delete();
  geom->Delete();
  shMapper->Delete();
  shActor->Delete();
  ren->Delete();
  renWin->Delete();
  iren->Delete();
  reader->Delete();
  shrink->Delete();

  return 0;
}
