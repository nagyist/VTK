// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkChart.h"
#include "vtkAxis.h"
#include "vtkBrush.h"
#include "vtkContextMouseEvent.h"
#include "vtkTransform2D.h"

#include "vtkAnnotationLink.h"
#include "vtkObjectFactory.h"
#include "vtkTextProperty.h"

//------------------------------------------------------------------------------
VTK_ABI_NAMESPACE_BEGIN
vtkChart::MouseActions::MouseActions()
{
  this->Pan() = vtkContextMouseEvent::LEFT_BUTTON;
  this->Zoom() = vtkContextMouseEvent::MIDDLE_BUTTON;
  this->Select() = vtkContextMouseEvent::RIGHT_BUTTON;
  this->ZoomAxis() = -1;
  this->SelectPolygon() = -1;
  this->ClickAndDrag() = -1;
}

//------------------------------------------------------------------------------
vtkChart::MouseClickActions::MouseClickActions()
{
  this->Data[0] = vtkContextMouseEvent::LEFT_BUTTON;
  this->Data[1] = vtkContextMouseEvent::RIGHT_BUTTON;
}

//------------------------------------------------------------------------------
vtkCxxSetObjectMacro(vtkChart, AnnotationLink, vtkAnnotationLink);

//------------------------------------------------------------------------------
vtkChart::vtkChart()
{
  this->Geometry[0] = 0;
  this->Geometry[1] = 0;
  this->Point1[0] = 0;
  this->Point1[1] = 0;
  this->Point2[0] = 0;
  this->Point2[1] = 0;
  this->Size.Set(0, 0, 0, 0);
  this->ShowLegend = false;
  this->TitleProperties = vtkTextProperty::New();
  this->TitleProperties->SetJustificationToCentered();
  this->TitleProperties->SetColor(0.0, 0.0, 0.0);
  this->TitleProperties->SetFontSize(12);
  this->TitleProperties->SetFontFamilyToArial();
  this->AnnotationLink = nullptr;
  this->LayoutStrategy = vtkChart::FILL_SCENE;
  this->RenderEmpty = false;
  this->BackgroundBrush = vtkSmartPointer<vtkBrush>::New();
  this->BackgroundBrush->SetColorF(1, 1, 1, 0);
  this->SelectionMode = vtkContextScene::SELECTION_DEFAULT;
  this->SelectionMethod = vtkChart::SELECTION_ROWS;
}

//------------------------------------------------------------------------------
vtkChart::~vtkChart()
{
  for (int i = 0; i < 4; i++)
  {
    if (this->GetAxis(i))
    {
      this->GetAxis(i)->RemoveObservers(vtkChart::UpdateRange);
    }
  }
  this->TitleProperties->Delete();
  if (this->AnnotationLink)
  {
    this->AnnotationLink->Delete();
  }
}

//------------------------------------------------------------------------------
vtkPlot* vtkChart::AddPlot(int)
{
  return nullptr;
}

//------------------------------------------------------------------------------
vtkIdType vtkChart::AddPlot(vtkPlot*)
{
  return -1;
}

//------------------------------------------------------------------------------
bool vtkChart::RemovePlot(vtkIdType)
{
  return false;
}

//------------------------------------------------------------------------------
bool vtkChart::RemovePlotInstance(vtkPlot* plot)
{
  if (plot)
  {
    vtkIdType numberOfPlots = this->GetNumberOfPlots();
    for (vtkIdType i = 0; i < numberOfPlots; ++i)
    {
      if (this->GetPlot(i) == plot)
      {
        return this->RemovePlot(i);
      }
    }
  }
  return false;
}

//------------------------------------------------------------------------------
void vtkChart::ClearPlots() {}

//------------------------------------------------------------------------------
vtkPlot* vtkChart::GetPlot(vtkIdType)
{
  return nullptr;
}

//------------------------------------------------------------------------------
vtkIdType vtkChart::GetNumberOfPlots()
{
  return 0;
}

//------------------------------------------------------------------------------
vtkAxis* vtkChart::GetAxis(int)
{
  return nullptr;
}

//------------------------------------------------------------------------------
void vtkChart::SetAxis(int, vtkAxis*) {}

//------------------------------------------------------------------------------
vtkIdType vtkChart::GetNumberOfAxes()
{
  return 0;
}

//------------------------------------------------------------------------------
void vtkChart::RecalculateBounds() {}

//------------------------------------------------------------------------------
void vtkChart::SetSelectionMethod(int method)
{
  if (method == this->SelectionMethod)
  {
    return;
  }
  this->SelectionMethod = method;
  this->Modified();
}

//------------------------------------------------------------------------------
int vtkChart::GetSelectionMethod()
{
  return this->SelectionMethod;
}

//------------------------------------------------------------------------------
void vtkChart::SetShowLegend(bool visible)
{
  if (this->ShowLegend != visible)
  {
    this->ShowLegend = visible;
    this->Modified();
  }
}

//------------------------------------------------------------------------------
bool vtkChart::GetShowLegend()
{
  return this->ShowLegend;
}

vtkChartLegend* vtkChart::GetLegend()
{
  return nullptr;
}

//------------------------------------------------------------------------------
void vtkChart::SetTitle(const vtkStdString& title)
{
  if (this->Title != title)
  {
    this->Title = title;
    this->Modified();
  }
}

//------------------------------------------------------------------------------
vtkStdString vtkChart::GetTitle()
{
  return this->Title;
}

//------------------------------------------------------------------------------
bool vtkChart::CalculatePlotTransform(vtkAxis* x, vtkAxis* y, vtkTransform2D* transform)
{
  if (!x || !y || !transform)
  {
    vtkWarningMacro("Called with null arguments.");
    return false;
  }

  vtkVector2d origin(x->GetMinimum(), y->GetMinimum());
  vtkVector2d scale(x->GetMaximum() - x->GetMinimum(), y->GetMaximum() - y->GetMinimum());
  vtkVector2d shift(0.0, 0.0);
  vtkVector2d factor(1.0, 1.0);

  for (int i = 0; i < 2; ++i)
  {
    double safeScale;
    if (scale[i] != 0.0)
    {
      safeScale = fabs(scale[i]);
    }
    else
    {
      safeScale = 1.0;
    }
    double safeOrigin;
    if (origin[i] != 0.0)
    {
      safeOrigin = fabs(origin[i]);
    }
    else
    {
      safeOrigin = 1.0;
    }

    if (fabs(log10(safeOrigin / safeScale)) > 2)
    {
      // the line below probably was meant to be something like
      // scale[i] = pow(10.0, floor(log10(safeOrigin / safeScale) / 3.0) * 3.0);
      // but instead was set to the following
      // shift[i] = floor(log10(safeOrigin / safeScale) / 3.0) * 3.0;
      // which makes no sense as the next line overwrites shift[i] ala
      shift[i] = -origin[i];
    }
    if (fabs(log10(safeScale)) > 10)
    {
      // We need to scale the transform to show all data, do this in blocks.
      factor[i] = pow(10.0, floor(log10(safeScale) / 10.0) * -10.0);
      scale[i] = scale[i] * factor[i];
    }
  }
  x->SetScalingFactor(factor[0]);
  x->SetShift(shift[0]);
  y->SetScalingFactor(factor[1]);
  y->SetShift(shift[1]);

  // Get the scale for the plot area from the x and y axes
  float* min = x->GetPoint1();
  float* max = x->GetPoint2();
  if (fabs(max[0] - min[0]) == 0.0)
  {
    return false;
  }
  float xScale = scale[0] / (max[0] - min[0]);

  // Now the y axis
  min = y->GetPoint1();
  max = y->GetPoint2();
  if (fabs(max[1] - min[1]) == 0.0)
  {
    return false;
  }
  float yScale = scale[1] / (max[1] - min[1]);

  transform->Identity();
  transform->Translate(this->Point1[0], this->Point1[1]);
  // Get the scale for the plot area from the x and y axes
  transform->Scale(1.0 / xScale, 1.0 / yScale);
  transform->Translate(
    -(x->GetMinimum() + shift[0]) * factor[0], -(y->GetMinimum() + shift[1]) * factor[1]);
  return true;
}

//------------------------------------------------------------------------------
bool vtkChart::CalculateUnscaledPlotTransform(vtkAxis* x, vtkAxis* y, vtkTransform2D* transform)
{
  if (!x || !y || !transform)
  {
    vtkWarningMacro("Called with null arguments.");
    return false;
  }

  vtkVector2d scale(x->GetMaximum() - x->GetMinimum(), y->GetMaximum() - y->GetMinimum());

  // Get the scale for the plot area from the x and y axes
  float* min = x->GetPoint1();
  float* max = x->GetPoint2();
  if (fabs(max[0] - min[0]) == 0.0)
  {
    return false;
  }
  double xScale = scale[0] / (max[0] - min[0]);

  // Now the y axis
  min = y->GetPoint1();
  max = y->GetPoint2();
  if (fabs(max[1] - min[1]) == 0.0)
  {
    return false;
  }
  double yScale = scale[1] / (max[1] - min[1]);

  transform->Identity();
  transform->Translate(this->Point1[0], this->Point1[1]);
  // Get the scale for the plot area from the x and y axes
  transform->Scale(1.0 / xScale, 1.0 / yScale);
  transform->Translate(-x->GetMinimum(), -y->GetMinimum());
  return true;
}

//------------------------------------------------------------------------------
void vtkChart::SetBottomBorder(int border)
{
  this->Borders[1] = border >= 0 ? border : 0;
  this->Point1[1] = this->Borders[1];
  this->Point1[1] += static_cast<int>(this->Size.GetY());
}

//------------------------------------------------------------------------------
void vtkChart::SetTopBorder(int border)
{
  this->Borders[2] = border >= 0 ? border : 0;
  this->Point2[1] = this->Geometry[1] - this->Borders[2];
  this->Point2[1] += static_cast<int>(this->Size.GetY());
}

//------------------------------------------------------------------------------
void vtkChart::SetLeftBorder(int border)
{
  this->Borders[0] = border >= 0 ? border : 0;
  this->Point1[0] = this->Borders[0];
  this->Point1[0] += static_cast<int>(this->Size.GetX());
}

//------------------------------------------------------------------------------
void vtkChart::SetRightBorder(int border)
{
  this->Borders[3] = border >= 0 ? border : 0;
  this->Point2[0] = this->Geometry[0] - this->Borders[3];
  this->Point2[0] += static_cast<int>(this->Size.GetX());
}

//------------------------------------------------------------------------------
void vtkChart::SetBorders(int left, int bottom, int right, int top)
{
  this->SetLeftBorder(left);
  this->SetRightBorder(right);
  this->SetTopBorder(top);
  this->SetBottomBorder(bottom);
}

void vtkChart::SetSize(const vtkRectf& rect)
{
  this->Size = rect;
  this->Geometry[0] = static_cast<int>(rect.GetWidth());
  this->Geometry[1] = static_cast<int>(rect.GetHeight());
}

vtkRectf vtkChart::GetSize()
{
  return this->Size;
}

void vtkChart::SetActionToButton(int action, int button)
{
  if (action < -1 || action >= MouseActions::MaxAction)
  {
    vtkErrorMacro("Error, invalid action value supplied: " << action);
    return;
  }
  this->Actions[action] = button;
  for (int i = 0; i < MouseActions::MaxAction; ++i)
  {
    if (this->Actions[i] == button && i != action)
    {
      this->Actions[i] = -1;
    }
  }
}

int vtkChart::GetActionToButton(int action)
{
  return this->Actions[action];
}

void vtkChart::SetClickActionToButton(int action, int button)
{
  if (action != vtkChart::SELECT && action != vtkChart::NOTIFY)
  {
    vtkErrorMacro("Error, invalid click action value supplied: " << action);
    return;
  }

  if (action == vtkChart::NOTIFY)
  {
    this->ActionsClick[0] = button;
  }
  else if (action == vtkChart::SELECT)
  {
    this->ActionsClick[1] = button;
  }
}

int vtkChart::GetClickActionToButton(int action)
{
  if (action == vtkChart::NOTIFY)
  {
    return this->ActionsClick[0];
  }
  else if (action == vtkChart::SELECT)
  {
    return this->ActionsClick[1];
  }

  return -1;
}

//------------------------------------------------------------------------------
void vtkChart::SetBackgroundBrush(vtkBrush* brush)
{
  if (brush == nullptr)
  {
    // set to transparent white if brush is null
    this->BackgroundBrush->SetColorF(1, 1, 1, 0);
  }
  else
  {
    this->BackgroundBrush = brush;
  }

  this->Modified();
}

//------------------------------------------------------------------------------
vtkBrush* vtkChart::GetBackgroundBrush()
{
  return this->BackgroundBrush;
}

//------------------------------------------------------------------------------
int vtkChart::GetSelectionModeFromMouseModifiers(
  const vtkContextMouseEvent& mouseEvent, int currentSelectionMode)
{
  if (mouseEvent.GetModifiers() & vtkContextMouseEvent::SHIFT_MODIFIER &&
    mouseEvent.GetModifiers() & vtkContextMouseEvent::CONTROL_MODIFIER)
  {
    return vtkContextScene::SELECTION_TOGGLE;
  }
  else if (mouseEvent.GetModifiers() & vtkContextMouseEvent::CONTROL_MODIFIER)
  {
    return vtkContextScene::SELECTION_ADDITION;
  }
  else if (mouseEvent.GetModifiers() & vtkContextMouseEvent::SHIFT_MODIFIER)
  {
    return vtkContextScene::SELECTION_SUBTRACTION;
  }

  return currentSelectionMode;
}

//------------------------------------------------------------------------------
int vtkChart::GetSelectionModeFromMouseModifiers(const vtkContextMouseEvent& mouseEvent)
{
  return vtkChart::GetSelectionModeFromMouseModifiers(mouseEvent, this->GetSelectionMode());
}

//------------------------------------------------------------------------------
void vtkChart::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  // Print out the chart's geometry if it has been set
  os << indent << "Point1: " << this->Point1[0] << "\t" << this->Point1[1] << endl;
  os << indent << "Point2: " << this->Point2[0] << "\t" << this->Point2[1] << endl;
  os << indent << "Width: " << this->Geometry[0] << endl
     << indent << "Height: " << this->Geometry[1] << endl;
  os << indent << "SelectionMode: " << this->SelectionMode << endl;
}
//------------------------------------------------------------------------------
void vtkChart::AttachAxisRangeListener(vtkAxis* axis)
{
  axis->AddObserver(vtkChart::UpdateRange, this, &vtkChart::AxisRangeForwarderCallback);
}

//------------------------------------------------------------------------------
void vtkChart::AxisRangeForwarderCallback(vtkObject*, unsigned long, void*)
{
  double fullAxisRange[8];
  for (int i = 0; i < 4; i++)
  {
    this->GetAxis(i)->GetRange(&fullAxisRange[i * 2]);
  }
  this->InvokeEvent(vtkChart::UpdateRange, fullAxisRange);
}
VTK_ABI_NAMESPACE_END
