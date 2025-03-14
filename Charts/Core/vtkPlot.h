// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

/**
 * @class   vtkPlot
 * @brief   Abstract class for 2D plots.
 *
 *
 * The base class for all plot types used in vtkChart derived charts.
 *
 * @sa
 * vtkPlotPoints vtkPlotLine vtkPlotBar vtkChart vtkChartXY
 */

#ifndef vtkPlot_h
#define vtkPlot_h

#include "vtkChartsCoreModule.h" // For export macro
#include "vtkContextItem.h"
#include "vtkContextPolygon.h" // For vtkContextPolygon
#include "vtkRect.h"           // For vtkRectd ivar
#include "vtkSmartPointer.h"   // Needed to hold SP ivars
#include "vtkStdString.h"      // Needed to hold TooltipLabelFormat ivar
#include "vtkWrappingHints.h"  // For VTK_MARSHALAUTO

VTK_ABI_NAMESPACE_BEGIN
class vtkVariant;
class vtkTable;
class vtkIdTypeArray;
class vtkContextMapper2D;
class vtkPen;
class vtkBrush;
class vtkAxis;
class vtkStringArray;
class vtkAlgorithmOutput;

class VTKCHARTSCORE_EXPORT VTK_MARSHALAUTO vtkPlot : public vtkContextItem
{
public:
  vtkTypeMacro(vtkPlot, vtkContextItem);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  /**
   * Perform any updates to the item that may be necessary before rendering.
   * The scene should take care of calling this on all items before their
   * Paint function is invoked.
   */
  void Update() override;

  ///@{
  /**
   * Set whether the plot renders an entry in the legend. Default is true.
   * vtkPlot::PaintLegend will get called to render the legend marker on when
   * this is true.
   */
  vtkSetMacro(LegendVisibility, bool);
  vtkGetMacro(LegendVisibility, bool);
  vtkBooleanMacro(LegendVisibility, bool);
  ///@}

  /**
   * Paint legend event for the plot, called whenever the legend needs the
   * plot items symbol/mark/line drawn. A rect is supplied with the lower left
   * corner of the rect (elements 0 and 1) and with width x height (elements 2
   * and 3). The plot can choose how to fill the space supplied. The index is used
   * by Plots that return more than one label.
   */
  virtual bool PaintLegend(vtkContext2D* painter, const vtkRectf& rect, int legendIndex);

  ///@{
  /**
   * Sets/gets a printf-style string to build custom tooltip labels from.
   * An empty string generates the default tooltip labels.
   * The following case-sensitive format tags (without quotes) are recognized:
   * '%x' The X value of the plot element
   * '%y' The Y value of the plot element
   * '%i' The IndexedLabels entry for the plot element
   * '%l' The value of the plot's GetLabel() function
   * '%s' (vtkPlotBar only) The Labels entry for the bar segment
   * Any other characters or unrecognized format tags are printed in the
   * tooltip label verbatim.
   */
  virtual void SetTooltipLabelFormat(const vtkStdString& label);
  virtual vtkStdString GetTooltipLabelFormat();
  ///@}

  ///@{
  /**
   * Sets/gets the tooltip notation style.
   */
  virtual void SetTooltipNotation(int notation);
  virtual int GetTooltipNotation();
  ///@}

  ///@{
  /**
   * Sets/gets the tooltip precision.
   */
  virtual void SetTooltipPrecision(int precision);
  virtual int GetTooltipPrecision();
  ///@}

  /**
   * Generate and return the tooltip label string for this plot
   * The segmentIndex parameter is ignored, except for vtkPlotBar
   */
  virtual vtkStdString GetTooltipLabel(
    const vtkVector2d& plotPos, vtkIdType seriesIndex, vtkIdType segmentIndex);

  /**
   * Function to query a plot for the nearest point to the specified coordinate.
   * Returns the index of the data series with which the point is associated, or
   * -1 if no point was found.
   */
  virtual vtkIdType GetNearestPoint(const vtkVector2f& point, const vtkVector2f& tolerance,
    vtkVector2f* location, vtkIdType* segmentId);

  /**
   * Select all points in the specified rectangle.
   */
  virtual bool SelectPoints(const vtkVector2f& min, const vtkVector2f& max);

  /**
   * Select all points in the specified polygon.
   */
  virtual bool SelectPointsInPolygon(const vtkContextPolygon& polygon);

  ///@{
  /**
   * Set the plot color with integer values (comprised between 0 and 255)
   */
  VTK_MARSHALSETTER(ColorRGBA)
  virtual void SetColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
  virtual void SetColor(unsigned char r, unsigned char g, unsigned char b);
  ///@}

  ///@{
  /**
   * Set the plot color with floating values (comprised between 0.0 and 1.0)
   */
  VTK_MARSHALEXCLUDE(VTK_MARSHAL_EXCLUDE_REASON_IS_REDUNDANT)
  virtual void SetColorF(double r, double g, double b, double a);
  VTK_MARSHALEXCLUDE(VTK_MARSHAL_EXCLUDE_REASON_IS_REDUNDANT)
  virtual void SetColorF(double r, double g, double b);
  ///@}

  ///@{
  /**
   * Get the plot color as integer rgb values (comprised between 0 and 255)
   */
  virtual void GetColor(unsigned char rgb[3]);
  VTK_MARSHALGETTER(ColorRGBA)
  void GetColorRGBA(unsigned char rgba[4]);
  ///@}

  ///@{
  /**
   * Get the plot color as floating rgb values (comprised between 0.0 and 1.0)
   */
  VTK_MARSHALEXCLUDE(VTK_MARSHAL_EXCLUDE_REASON_IS_REDUNDANT)
  virtual void GetColorF(double rgb[3]);
  ///@

  /**
   * Set the width of the line.
   */
  virtual void SetWidth(float width);

  /**
   * Get the width of the line.
   */
  virtual float GetWidth();

  ///@{
  /**
   * Set/get the vtkPen object that controls how this plot draws (out)lines.
   */
  void SetPen(vtkPen* pen);
  vtkPen* GetPen();
  ///@}

  ///@{
  /**
   * Set/get the vtkBrush object that controls how this plot fills shapes.
   */
  void SetBrush(vtkBrush* brush);
  vtkBrush* GetBrush();
  ///@}

  ///@{
  /**
   * Set/get the vtkBrush object that controls how this plot fills selected
   * shapes.
   */
  void SetSelectionPen(vtkPen* pen);
  vtkPen* GetSelectionPen();
  ///@}

  ///@{
  /**
   * Set/get the vtkBrush object that controls how this plot fills selected
   * shapes.
   */
  void SetSelectionBrush(vtkBrush* brush);
  vtkBrush* GetSelectionBrush();
  ///@}

  /**
   * Set the label of this plot.
   */
  virtual void SetLabel(const vtkStdString& label);

  /**
   * Get the label of this plot.
   */
  virtual vtkStdString GetLabel();

  /**
   * Set the plot labels, these are used for stacked chart variants, with the
   * index referring to the stacking index.
   */
  virtual void SetLabels(vtkStringArray* labels);

  /**
   * Get the plot labels. If this array has a length greater than 1 the index
   * refers to the stacked objects in the plot. See vtkPlotBar for example.
   */
  virtual vtkStringArray* GetLabels();

  /**
   * Get the number of labels associated with this plot.
   */
  virtual int GetNumberOfLabels();

  /**
   * Get the label at the specified index.
   */
  vtkStdString GetLabel(vtkIdType index);

  /**
   * Set indexed labels for the plot. If set, this array can be used to provide
   * custom labels for each point in a plot. This array should be the same
   * length as the points array. Default is null (no indexed labels).
   */
  void SetIndexedLabels(vtkStringArray* labels);

  /**
   * Get the indexed labels array.
   */
  virtual vtkStringArray* GetIndexedLabels();

  /**
   * Get the data object that the plot will draw.
   */
  vtkContextMapper2D* GetData();

  ///@{
  /**
   * Use the Y array index for the X value. If true any X column setting will be
   * ignored, and the X values will simply be the index of the Y column.
   */
  vtkGetMacro(UseIndexForXSeries, bool);
  ///@}

  ///@{
  /**
   * Use the Y array index for the X value. If true any X column setting will be
   * ignored, and the X values will simply be the index of the Y column.
   */
  vtkSetMacro(UseIndexForXSeries, bool);
  ///@}

  ///@{
  /**
   * This is a convenience function to set the input table and the x, y column
   * for the plot.
   */
  VTK_MARSHALSETTER(Input)
  virtual void SetInputData(vtkTable* table);
  virtual void SetInputData(
    vtkTable* table, const vtkStdString& xColumn, const vtkStdString& yColumn);
  void SetInputData(vtkTable* table, vtkIdType xColumn, vtkIdType yColumn);
  ///@}

  ///@{
  /**
   * This is a convenience function to set the input connection for the plot.
   */
  VTK_MARSHALEXCLUDE(VTK_MARSHAL_EXCLUDE_REASON_IS_INTERNAL);
  virtual void SetInputConnection(vtkAlgorithmOutput* input);
  ///@}

  /**
   * Get the input table used by the plot.
   */
  VTK_MARSHALGETTER(Input)
  virtual vtkTable* GetInput();

  /**
   * Get the input connection used by the plot.
   */
  VTK_MARSHALEXCLUDE(VTK_MARSHAL_EXCLUDE_REASON_IS_INTERNAL);
  vtkAlgorithmOutput* GetInputConnection();

  /**
   * Convenience function to set the input arrays. For most plots index 0
   * is the x axis, and index 1 is the y axis. The name is the name of the
   * column in the vtkTable.
   */
  virtual void SetInputArray(int index, const vtkStdString& name);

  ///@{
  /**
   * Convenient function to directly set/get the names of columns
   * used for X and Y axis respectively.
   */
  void SetXAxisInputArrayToProcess(const std::string& name);
  std::string GetXAxisInputArrayToProcess();
  void SetYAxisInputArrayToProcess(const std::string& name);
  std::string GetYAxisInputArrayToProcess();
  ///@}

  ///@{
  /**
   * Set whether the plot can be selected. True by default.
   * If not, then SetSelection(), SelectPoints() or SelectPointsInPolygon()
   * won't have any effect.
   * \sa SetSelection(), SelectPoints(), SelectPointsInPolygon()
   */
  vtkSetMacro(Selectable, bool);
  vtkGetMacro(Selectable, bool);
  vtkBooleanMacro(Selectable, bool);
  ///@}

  ///@{
  /**
   * Sets the list of points that must be selected.
   * If Selectable is false, then this method does nothing.
   * \sa SetSelectable()
   */
  virtual void SetSelection(vtkIdTypeArray* id);
  vtkGetObjectMacro(Selection, vtkIdTypeArray);
  ///@}

  ///@{
  /**
   * Get/set the X axis associated with this plot.
   */
  vtkGetObjectMacro(XAxis, vtkAxis);
  virtual void SetXAxis(vtkAxis* axis);
  ///@}

  ///@{
  /**
   * Get/set the Y axis associated with this plot.
   */
  vtkGetObjectMacro(YAxis, vtkAxis);
  virtual void SetYAxis(vtkAxis* axis);
  ///@}

  ///@{
  /**
   * Get/set the origin shift and scaling factor used by the plot, this is
   * normally 0.0 offset and 1.0 scaling, but can be used to render data outside
   * of the single precision range. The chart that owns the plot should set this
   * and ensure the appropriate matrix is used when rendering the plot.
   */
  void SetShiftScale(const vtkRectd& shiftScale);
  vtkRectd GetShiftScale();
  ///@}

  /**
   * Get the bounds for this plot as (Xmin, Xmax, Ymin, Ymax).

   * See \a GetUnscaledInputBounds for more information.
   */
  virtual void GetBounds(double bounds[4]) { bounds[0] = bounds[1] = bounds[2] = bounds[3] = 0.0; }

  /**
   * Provide un-log-scaled bounds for the plot inputs.

   * This function is analogous to GetBounds() with 2 exceptions:
   * 1. It will never return log-scaled bounds even when the
   * x- and/or y-axes are log-scaled.
   * 2. It will always return the bounds along the *input* axes
   * rather than the output chart coordinates. Thus GetXAxis()
   * returns the axis associated with the first 2 bounds entries
   * and GetYAxis() returns the axis associated with the next 2
   * bounds entries.

   * For example, vtkPlotBar's GetBounds() method
   * will swap axis bounds when its orientation is vertical while
   * its GetUnscaledInputBounds() will not swap axis bounds.

   * This method is provided so user interfaces can determine
   * whether or not to allow log-scaling of a particular vtkAxis.

   * Subclasses of vtkPlot are responsible for implementing this
   * function to transform input plot data.

   * The returned \a bounds are stored as (Xmin, Xmax, Ymin, Ymax).
   */
  virtual void GetUnscaledInputBounds(double bounds[4])
  {
    // Implemented here by calling GetBounds() to support plot
    // subclasses that do no log-scaling or plot orientation.
    this->GetBounds(bounds);
  }

  ///@{
  /**
   * A General setter/getter that should be overridden. It can silently drop
   * options, case is important
   */
  virtual void SetProperty(const vtkStdString& property, const vtkVariant& var);
  virtual vtkVariant GetProperty(const vtkStdString& property);
  ///@}

  ///@{
  /**
   * Clamp the given 2D pos into the provided bounds
   * Return true if the pos has been clamped, false otherwise.
   */
  static bool ClampPos(double pos[2], double bounds[4]);
  virtual bool ClampPos(double pos[2]);
  ///@}

  /**
   * Returns true if the supplied x, y coordinate is inside the item.
   */
  bool Hit(const vtkContextMouseEvent& mouse) override;

  /**
   * Update the internal cache. Returns true if cache was successfully updated. Default does
   * nothing.
   * This method is called by Update() when either the plot's data has changed or
   * CacheRequiresUpdate() returns true. It is not necessary to call this method explicitly.
   */
  virtual bool UpdateCache() { return true; }

  /**
   * Utility function that fills up `selectedPoints` with tuples from `points`. Indices
   * from `selectedIds` are used to index into `points`.
   */
  static void FilterSelectedPoints(
    vtkDataArray* points, vtkDataArray* selectedPoints, vtkIdTypeArray* selectedIds);

protected:
  vtkPlot();
  ~vtkPlot() override;

  /**
   * Get the properly formatted number for the supplied position and axis.
   */
  vtkStdString GetNumber(double position, vtkAxis* axis);

  ///@{
  /**
   * Transform the mouse event in the control-points space. This is needed when
   * using logScale or shiftscale.
   */
  virtual void TransformScreenToData(const vtkVector2f& in, vtkVector2f& out);
  virtual void TransformDataToScreen(const vtkVector2f& in, vtkVector2f& out);
  virtual void TransformScreenToData(double inX, double inY, double& outX, double& outY);
  virtual void TransformDataToScreen(double inX, double inY, double& outX, double& outY);
  ///@}

  /**
   * Test if the internal cache requires an update.
   */
  virtual bool CacheRequiresUpdate();

  /**
   * The point cache is marked dirty until it has been initialized.
   */
  vtkTimeStamp BuildTime;

  /**
   * This object stores the vtkPen that controls how the plot is drawn.
   */
  vtkSmartPointer<vtkPen> Pen;

  /**
   * This object stores the vtkBrush that controls how the plot is drawn.
   */
  vtkSmartPointer<vtkBrush> Brush;

  /**
   * This object stores the vtkPen that controls how the selected elements
   * of the plot are drawn.
   */
  vtkSmartPointer<vtkPen> SelectionPen;

  /**
   * This object stores the vtkBrush that controls how the selected elements
   * of the plot are drawn.
   */
  vtkSmartPointer<vtkBrush> SelectionBrush;

  /**
   * Plot labels, used by legend.
   */
  vtkSmartPointer<vtkStringArray> Labels;

  /**
   * Holds Labels when they're auto-created
   */
  vtkSmartPointer<vtkStringArray> AutoLabels;

  /**
   * Holds Labels when they're auto-created
   */
  vtkSmartPointer<vtkStringArray> IndexedLabels;

  /**
   * Use the Y array index for the X value. If true any X column setting will be
   * ignored, and the X values will simply be the index of the Y column.
   */
  bool UseIndexForXSeries;

  /**
   * This data member contains the data that will be plotted, it inherits
   * from vtkAlgorithm.
   */
  vtkSmartPointer<vtkContextMapper2D> Data;

  /**
   * Whether plot points can be selected or not.
   */
  bool Selectable;

  /**
   * Selected indices for the table the plot is rendering
   */
  vtkIdTypeArray* Selection;

  /**
   * The X axis associated with this plot.
   */
  vtkAxis* XAxis;

  /**
   * The X axis associated with this plot.
   */
  vtkAxis* YAxis;

  /**
   * A printf-style string to build custom tooltip labels from.
   * See the accessor/mutator functions for full documentation.
   */
  vtkStdString TooltipLabelFormat;

  /**
   * The default printf-style string to build custom tooltip labels from.
   * See the accessor/mutator functions for full documentation.
   */
  vtkStdString TooltipDefaultLabelFormat;

  int TooltipNotation;
  int TooltipPrecision;

  /**
   * The current shift in origin and scaling factor applied to the plot.
   */
  vtkRectd ShiftScale;

  bool LegendVisibility;

private:
  vtkPlot(const vtkPlot&) = delete;
  void operator=(const vtkPlot&) = delete;
};

VTK_ABI_NAMESPACE_END
#endif // vtkPlot_h
