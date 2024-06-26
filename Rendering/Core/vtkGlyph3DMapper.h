// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkGlyph3DMapper
 * @brief   vtkGlyph3D on the GPU.
 *
 * Do the same job than vtkGlyph3D but on the GPU. For this reason, it is
 * a mapper not a vtkPolyDataAlgorithm. Also, some methods of vtkGlyph3D
 * don't make sense in vtkGlyph3DMapper: GeneratePointIds, old-style
 * SetSource, PointIdsName, IsPointVisible.
 *
 * @sa
 * vtkGlyph3D
 */

#ifndef vtkGlyph3DMapper_h
#define vtkGlyph3DMapper_h

#include "vtkGlyph3D.h" // for the constants (VTK_SCALE_BY_SCALAR, ...).
#include "vtkMapper.h"
#include "vtkRenderingCoreModule.h" // For export macro
#include "vtkWeakPointer.h"         // needed for vtkWeakPointer.
#include "vtkWrappingHints.h"       // For VTK_MARSHALAUTO

VTK_ABI_NAMESPACE_BEGIN
class vtkCompositeDataDisplayAttributes;
class vtkDataObjectTree;

class VTKRENDERINGCORE_EXPORT VTK_MARSHALAUTO vtkGlyph3DMapper : public vtkMapper
{
public:
  static vtkGlyph3DMapper* New();
  vtkTypeMacro(vtkGlyph3DMapper, vtkMapper);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  enum ArrayIndexes
  {
    SCALE = 0,
    SOURCE_INDEX = 1,
    MASK = 2,
    ORIENTATION = 3,
    SELECTIONID = 4
  };

  /**
   * Specify a source object at a specified table location. New style.
   * Source connection is stored in port 1. This method is equivalent
   * to SetInputConnection(1, id, outputPort).
   */
  void SetSourceConnection(int idx, vtkAlgorithmOutput* algOutput);
  void SetSourceConnection(vtkAlgorithmOutput* algOutput)
  {
    this->SetSourceConnection(0, algOutput);
  }

  /**
   * Assign a data object as input. Note that this method does not
   * establish a pipeline connection. Use SetInputConnection() to
   * setup a pipeline connection.
   */
  void SetInputData(vtkDataObject*);

  /**
   * Specify a source object at a specified table location.
   */
  void SetSourceData(int idx, vtkPolyData* pd);

  /**
   * Specify a data object tree that will be used for the source table. Requires
   * UseSourceTableTree to be true. The top-level nodes of the tree are mapped
   * to the source data inputs.
   *
   * Must only contain vtkPolyData instances on the OpenGL backend. May contain
   * vtkCompositeDataSets with vtkPolyData leaves on OpenGL2.
   */
  void SetSourceTableTree(vtkDataObjectTree* tree);

  /**
   * Set the source to use for he glyph.
   * Note that this method does not connect the pipeline. The algorithm will
   * work on the input data as it is without updating the producer of the data.
   * See SetSourceConnection for connecting the pipeline.
   */
  void SetSourceData(vtkPolyData* pd);

  /**
   * Get a pointer to a source object at a specified table location.
   */
  vtkPolyData* GetSource(int idx = 0);

  /**
   * Convenience method to get the source table tree, if it exists.
   */
  vtkDataObjectTree* GetSourceTableTree();

  ///@{
  /**
   * Turn on/off scaling of source geometry. When turned on, ScaleFactor
   * controls the scale applied. To scale with some data array, ScaleMode should
   * be set accordingly.
   */
  vtkSetMacro(Scaling, bool);
  vtkBooleanMacro(Scaling, bool);
  vtkGetMacro(Scaling, bool);
  ///@}

  ///@{
  /**
   * Either scale by individual components (SCALE_BY_COMPONENTS) or magnitude
   * (SCALE_BY_MAGNITUDE) of the chosen array to SCALE with or disable scaling
   * using data array all together (NO_DATA_SCALING). Default is
   * NO_DATA_SCALING.
   */
  vtkSetMacro(ScaleMode, int);
  vtkGetMacro(ScaleMode, int);
  ///@}

  ///@{
  /**
   * Specify scale factor to scale object by. This is used only when Scaling is
   * On.
   */
  vtkSetMacro(ScaleFactor, double);
  vtkGetMacro(ScaleFactor, double);
  ///@}

  enum ScaleModes
  {
    NO_DATA_SCALING = 0,
    SCALE_BY_MAGNITUDE = 1,
    SCALE_BY_COMPONENTS = 2
  };

  void SetScaleModeToScaleByMagnitude() { this->SetScaleMode(SCALE_BY_MAGNITUDE); }
  void SetScaleModeToScaleByVectorComponents() { this->SetScaleMode(SCALE_BY_COMPONENTS); }
  void SetScaleModeToNoDataScaling() { this->SetScaleMode(NO_DATA_SCALING); }
  const char* GetScaleModeAsString();

  ///@{
  /**
   * Specify range to map scalar values into.
   */
  vtkSetVector2Macro(Range, double);
  vtkGetVectorMacro(Range, double, 2);
  ///@}

  ///@{
  /**
   * Turn on/off orienting of input geometry.
   * When turned on, the orientation array specified
   * using SetOrientationArray() will be used.
   */
  vtkSetMacro(Orient, bool);
  vtkGetMacro(Orient, bool);
  vtkBooleanMacro(Orient, bool);
  ///@}

  ///@{
  /**
   * Orientation mode indicates if the OrientationArray provides the direction
   * vector for the orientation or the rotations around each axes. Default is
   * DIRECTION
   */
  vtkSetClampMacro(OrientationMode, int, DIRECTION, QUATERNION);
  vtkGetMacro(OrientationMode, int);
  void SetOrientationModeToDirection() { this->SetOrientationMode(vtkGlyph3DMapper::DIRECTION); }
  void SetOrientationModeToRotation() { this->SetOrientationMode(vtkGlyph3DMapper::ROTATION); }
  void SetOrientationModeToQuaternion() { this->SetOrientationMode(vtkGlyph3DMapper::QUATERNION); }
  const char* GetOrientationModeAsString();
  ///@}

  enum OrientationModes
  {
    DIRECTION = 0,
    ROTATION = 1,
    QUATERNION = 2
  };

  ///@{
  /**
   * Turn on/off clamping of data values to scale with to the specified range.
   */
  vtkSetMacro(Clamping, bool);
  vtkGetMacro(Clamping, bool);
  vtkBooleanMacro(Clamping, bool);
  ///@}

  ///@{
  /**
   * Enable/disable indexing into table of the glyph sources. When disabled,
   * only the 1st source input will be used to generate the glyph. Otherwise the
   * source index array will be used to select the glyph source. The source
   * index array can be specified using SetSourceIndexArray().
   */
  vtkSetMacro(SourceIndexing, bool);
  vtkGetMacro(SourceIndexing, bool);
  vtkBooleanMacro(SourceIndexing, bool);
  ///@}

  ///@{
  /**
   * If true, and the glyph source dataset is a subclass of vtkDataObjectTree,
   * the top-level members of the tree will be mapped to the glyph source table
   * used for SourceIndexing.
   */
  vtkSetMacro(UseSourceTableTree, bool);
  vtkGetMacro(UseSourceTableTree, bool);
  vtkBooleanMacro(UseSourceTableTree, bool);

  ///@{
  /**
   * Turn on/off custom selection ids. If enabled, the id values set with
   * SetSelectionIdArray are returned from pick events.
   */
  vtkSetMacro(UseSelectionIds, bool);
  vtkBooleanMacro(UseSelectionIds, bool);
  vtkGetMacro(UseSelectionIds, bool);
  ///@}

  /**
   * Redefined to take into account the bounds of the scaled glyphs.
   */
  double* GetBounds() override;

  /**
   * Same as superclass. Appear again to stop warnings about hidden method.
   */
  void GetBounds(double bounds[6]) override;

  /**
   * All the work is done is derived classes.
   */
  void Render(vtkRenderer* ren, vtkActor* act) override;

  ///@{
  /**
   * Tells the mapper to skip glyphing input points that haves false values
   * in the mask array. If there is no mask array (id access mode is set
   * and there is no such id, or array name access mode is set and
   * the there is no such name), masking is silently ignored.
   * A mask array is a vtkBitArray with only one component.
   * Initial value is false.
   */
  vtkSetMacro(Masking, bool);
  vtkGetMacro(Masking, bool);
  vtkBooleanMacro(Masking, bool);
  ///@}

  /**
   * Set the name of the point array to use as a mask for generating the glyphs.
   * This is a convenience method. The same effect can be achieved by using
   * SetInputArrayToProcess(vtkGlyph3DMapper::MASK, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, maskarrayname)
   */
  void SetMaskArray(const char* maskarrayname);

  /**
   * Set the point attribute to use as a mask for generating the glyphs.
   * \c fieldAttributeType is one of the following:
   * \li vtkDataSetAttributes::SCALARS
   * \li vtkDataSetAttributes::VECTORS
   * \li vtkDataSetAttributes::NORMALS
   * \li vtkDataSetAttributes::TCOORDS
   * \li vtkDataSetAttributes::TENSORS
   * This is a convenience method. The same effect can be achieved by using
   * SetInputArrayToProcess(vtkGlyph3DMapper::MASK, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldAttributeType)
   */
  void SetMaskArray(int fieldAttributeType);

  /**
   * Tells the mapper to use an orientation array if Orient is true.
   * An orientation array is a vtkDataArray with 3 components. The first
   * component is the angle of rotation along the X axis. The second
   * component is the angle of rotation along the Y axis. The third
   * component is the angle of rotation along the Z axis. Orientation is
   * specified in X,Y,Z order but the rotations are performed in Z,X an Y.
   * This definition is compliant with SetOrientation method on vtkProp3D.
   * By using vector or normal there is a degree of freedom or rotation
   * left (underconstrained). With the orientation array, there is no degree of
   * freedom left.
   * This is convenience method. The same effect can be achieved by using
   * SetInputArrayToProcess(vtkGlyph3DMapper::ORIENTATION, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, orientationarrayname);
   */
  void SetOrientationArray(const char* orientationarrayname);

  /**
   * Tells the mapper to use an orientation array if Orient is true.
   * An orientation array is a vtkDataArray with 3 components. The first
   * component is the angle of rotation along the X axis. The second
   * component is the angle of rotation along the Y axis. The third
   * component is the angle of rotation along the Z axis. Orientation is
   * specified in X,Y,Z order but the rotations are performed in Z,X an Y.
   * This definition is compliant with SetOrientation method on vtkProp3D.
   * By using vector or normal there is a degree of freedom or rotation
   * left (underconstrained). With the orientation array, there is no degree of
   * freedom left.
   * \c fieldAttributeType is one of the following:
   * \li vtkDataSetAttributes::SCALARS
   * \li vtkDataSetAttributes::VECTORS
   * \li vtkDataSetAttributes::NORMALS
   * \li vtkDataSetAttributes::TCOORDS
   * \li vtkDataSetAttributes::TENSORS
   * This is convenience method. The same effect can be achieved by using
   * SetInputArrayToProcess(vtkGlyph3DMapper::ORIENTATION, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldAttributeType);
   */
  void SetOrientationArray(int fieldAttributeType);

  /**
   * Convenience method to set the array to scale with. This is same as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SCALE, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, scalarsarrayname).
   */
  void SetScaleArray(const char* scalarsarrayname);

  /**
   * Convenience method to set the array to scale with. This is same as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SCALE, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldAttributeType).
   */
  void SetScaleArray(int fieldAttributeType);

  /**
   * Convenience method to set the array to use as index within the sources.
   * This is same as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SOURCE_INDEX, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, arrayname).
   */
  void SetSourceIndexArray(const char* arrayname);

  /**
   * Convenience method to set the array to use as index within the sources.
   * This is same as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SOURCE_INDEX, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldAttributeType).
   */
  void SetSourceIndexArray(int fieldAttributeType);

  /**
   * Convenience method to set the array used for selection IDs. This is same
   * as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SELECTIONID, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, selectionidarrayname).

   * If no selection id array is specified, the index of the glyph point is
   * used.
   */
  void SetSelectionIdArray(const char* selectionIdArrayName);

  /**
   * Convenience method to set the array used for selection IDs. This is same
   * as calling
   * SetInputArrayToProcess(vtkGlyph3DMapper::SELECTIONID, 0, 0,
   * vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldAttributeType).

   * If no selection id array is specified, the index of the glyph point is
   * used.
   */
  void SetSelectionIdArray(int fieldAttributeType);

  ///@{
  /**
   * For selection by color id mode (not for end-user, called by
   * vtkGlyphSelectionRenderMode). 0 is reserved for miss. it has to
   * start at 1. Initial value is 1.
   */
  vtkSetMacro(SelectionColorId, unsigned int);
  vtkGetMacro(SelectionColorId, unsigned int);
  ///@}

  ///@{
  /**
   * When the input data object (not the source) is composite data,
   * it is possible to control visibility and pickability on a per-block
   * basis by passing the mapper a vtkCompositeDataDisplayAttributes instance.
   * The color and opacity in the display-attributes instance are ignored
   * for now. By default, the mapper does not own a display-attributes
   * instance. The value of BlockAttributes has no effect when the input
   * is a poly-data object.
   */
  virtual void SetBlockAttributes(vtkCompositeDataDisplayAttributes* attr);
  vtkGetObjectMacro(BlockAttributes, vtkCompositeDataDisplayAttributes);
  ///@}

  ///@{
  /**
   * Enable or disable frustum culling and LOD of the instances.
   * When enabled, an OpenGL driver supporting GL_ARB_gpu_shader5 extension is mandatory.
   */
  vtkSetMacro(CullingAndLOD, bool);
  vtkGetMacro(CullingAndLOD, bool);

  /**
   * Get the maximum number of LOD. OpenGL context must be bound.
   * The maximum number of LOD depends on GPU capabilities.
   * This method is intended to be reimplemented in inherited classes, current implementation
   * always returns zero.
   */
  virtual vtkIdType GetMaxNumberOfLOD();

  /**
   * Set the number of LOD.
   * This method is intended to be reimplemented in inherited classes, current implementation
   * does nothing.
   */
  virtual void SetNumberOfLOD(vtkIdType vtkNotUsed(nb)) {}

  /**
   * Configure LODs. Culling must be enabled.
   * distance have to be a positive value, it is the distance to the camera scaled by
   * the instanced geometry bounding box.
   * targetReduction have to be between 0 and 1, 0 disable decimation, 1 draw a point.
   * This method is intended to be reimplemented in inherited classes, current implementation
   * does nothing.
   *
   * @sa vtkDecimatePro::SetTargetReduction
   */
  virtual void SetLODDistanceAndTargetReduction(
    vtkIdType vtkNotUsed(index), float vtkNotUsed(distance), float vtkNotUsed(targetReduction))
  {
  }

  /**
   * Enable LOD coloring. It can be useful to configure properly the LODs.
   * Each LOD have a unique color, based on its index.
   */
  vtkSetMacro(LODColoring, bool);
  vtkGetMacro(LODColoring, bool);
  ///@}

  /**
   * WARNING: INTERNAL METHOD - NOT INTENDED FOR GENERAL USE
   * DO NOT USE THIS METHOD OUTSIDE OF THE RENDERING PROCESS
   * Used by vtkHardwareSelector to determine if the prop supports hardware
   * selection.
   */
  bool GetSupportsSelection() override { return true; }

protected:
  vtkGlyph3DMapper();
  ~vtkGlyph3DMapper() override;

  virtual int RequestUpdateExtent(
    vtkInformation* request, vtkInformationVector** inInfo, vtkInformationVector* outInfo);

  int FillInputPortInformation(int port, vtkInformation* info) override;

  vtkPolyData* GetSource(int idx, vtkInformationVector* sourceInfo);
  vtkPolyData* GetSourceTable(int idx, vtkInformationVector* sourceInfo);

  ///@{
  /**
   * Convenience methods to get each of the arrays.
   */
  vtkDataArray* GetMaskArray(vtkDataSet* input);
  vtkDataArray* GetSourceIndexArray(vtkDataSet* input);
  vtkDataArray* GetOrientationArray(vtkDataSet* input);
  vtkDataArray* GetScaleArray(vtkDataSet* input);
  vtkDataArray* GetSelectionIdArray(vtkDataSet* input);
  vtkUnsignedCharArray* GetColors(vtkDataSet* input);
  ///@}

  vtkCompositeDataDisplayAttributes* BlockAttributes;
  bool Scaling;       // Determine whether scaling of geometry is performed
  double ScaleFactor; // Scale factor to use to scale geometry
  int ScaleMode;      // Scale by scalar value or vector magnitude

  double Range[2];      // Range to use to perform scalar scaling
  bool Orient;          // boolean controls whether to "orient" data
  bool Clamping;        // whether to clamp scale factor
  bool SourceIndexing;  // Enable/disable indexing into the glyph table
  bool UseSelectionIds; // Enable/disable custom pick ids
  bool Masking;         // Enable/disable masking.
  int OrientationMode;

  bool UseSourceTableTree; // Map DataObjectTree glyph source into table

  unsigned int SelectionColorId;

  bool CullingAndLOD = false; // Disable culling
  std::vector<std::pair<float, float>> LODs;
  bool LODColoring = false;

private:
  vtkGlyph3DMapper(const vtkGlyph3DMapper&) = delete;
  void operator=(const vtkGlyph3DMapper&) = delete;

  /**
   * Returns true when valid bounds are returned.
   */
  bool GetBoundsInternal(vtkDataSet* ds, double ds_bounds[6]);
};

VTK_ABI_NAMESPACE_END
#endif
