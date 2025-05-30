// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkChartXYZ.h"
#include "vtkContextMouseEvent.h"
#include "vtkContextScene.h"
#include "vtkContextView.h"
#include "vtkFloatArray.h"
#include "vtkNew.h"
#include "vtkPlotSurface.h"
#include "vtkRegressionTestImage.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRenderer.h"
#include "vtkTable.h"
#include "vtkUnsignedCharArray.h"
#include "vtkVector.h"

int TestSurfacePlot(int, char*[])
{
  vtkNew<vtkChartXYZ> chart;
  vtkNew<vtkPlotSurface> plot;
  vtkNew<vtkContextView> view;
  view->GetRenderWindow()->SetSize(400, 300);
  view->GetScene()->AddItem(chart);

  chart->SetGeometry(vtkRectf(75.0, 20.0, 250, 260));

  // Create a surface
  vtkNew<vtkTable> table;
  vtkIdType numPoints = 70;
  float inc = 9.424778 / (numPoints - 1);
  for (vtkIdType i = 0; i < numPoints; ++i)
  {
    vtkNew<vtkFloatArray> arr;
    table->AddColumn(arr);
  }
  table->SetNumberOfRows(numPoints);
  for (vtkIdType i = 0; i < numPoints; ++i)
  {
    float x = i * inc;
    for (vtkIdType j = 0; j < numPoints; ++j)
    {
      float y = j * inc;
      table->SetValue(i, j, sin(sqrt(x * x + y * y)));
    }
  }

  // Set up the surface plot we wish to visualize and add it to the chart.
  plot->SetXRange(0, 9.424778);
  plot->SetYRange(0, 9.424778);
  plot->SetInputData(table);
  chart->AddPlot(plot);

  view->GetRenderWindow()->SetMultiSamples(0);
  view->GetInteractor()->Initialize();
  view->GetRenderWindow()->Render();

  // rotate
  vtkContextMouseEvent mouseEvent;
  mouseEvent.SetInteractor(view->GetInteractor());
  vtkVector2f pos;
  vtkVector2f lastPos;

  mouseEvent.SetButton(vtkContextMouseEvent::LEFT_BUTTON);
  lastPos.Set(100, 50);
  mouseEvent.SetLastScenePos(lastPos);
  pos.Set(150, 100);
  mouseEvent.SetScenePos(pos);
  vtkVector2d sP(pos.Cast<double>().GetData());
  vtkVector2d lSP(lastPos.Cast<double>().GetData());
  vtkVector2d scenePos(mouseEvent.GetScenePos().Cast<double>().GetData());
  vtkVector2d lastScenePos(mouseEvent.GetLastScenePos().Cast<double>().GetData());
  chart->MouseMoveEvent(mouseEvent);

  view->GetInteractor()->Start();

  return EXIT_SUCCESS;
}
