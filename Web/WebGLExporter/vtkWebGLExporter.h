// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkWebGLExporter
 * @brief   vtkWebGLExporter export the data of the scene to be used in the WebGL.
 */

#ifndef vtkWebGLExporter_h
#define vtkWebGLExporter_h

#include "vtkObject.h"
#include "vtkWebGLExporterModule.h" // needed for export macro

VTK_ABI_NAMESPACE_BEGIN
class vtkActor;
class vtkActor2D;
class vtkCellData;
class vtkMapper;
class vtkPointData;
class vtkPolyData;
class vtkRenderer;
class vtkRendererCollection;
class vtkTriangleFilter;
class vtkWebGLObject;
class vtkWebGLPolyData;

VTK_ABI_NAMESPACE_END

#include <string> // needed for internal structure

VTK_ABI_NAMESPACE_BEGIN
typedef enum
{
  VTK_ONLYCAMERA = 0,
  VTK_ONLYWIDGET = 1,
  VTK_PARSEALL = 2
} VTKParseType;

class VTKWEBGLEXPORTER_EXPORT vtkWebGLExporter : public vtkObject
{
public:
  static vtkWebGLExporter* New();
  vtkTypeMacro(vtkWebGLExporter, vtkObject);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  ///@{
  /**
   * Get all the needed information from the vtkRenderer
   */
  void parseScene(vtkRendererCollection* renderers, const char* viewId, int parseType);
  // Generate and return the Metadata
  void exportStaticScene(vtkRendererCollection* renderers, int width, int height, std::string path);
  const char* GenerateMetadata();
  const char* GetId();
  vtkWebGLObject* GetWebGLObject(int index);
  int GetNumberOfObjects();
  bool hasChanged();
  void SetCenterOfRotation(float a1, float a2, float a3);
  void SetMaxAllowedSize(int mesh, int lines);
  void SetMaxAllowedSize(int size);
  ///@}

  static void ComputeMD5(const unsigned char* content, int size, std::string& hash);

protected:
  vtkWebGLExporter();
  ~vtkWebGLExporter() override;

  void parseRenderer(vtkRenderer* render, const char* viewId, bool onlyWidget, void* mapTime);
  void generateRendererData(vtkRendererCollection* renderers, const char* viewId);
  void parseActor(
    vtkActor* actor, vtkMTimeType actorTime, size_t rendererId, int layer, bool isWidget);
  void parseActor2D(
    vtkActor2D* actor, vtkMTimeType actorTime, size_t renderId, int layer, bool isWidget);
  const char* GenerateExportMetadata();

  // Get the dataset from the mapper
  vtkTriangleFilter* GetPolyData(vtkMapper* mapper, vtkMTimeType& dataMTime);

  vtkTriangleFilter* TriangleFilter;  // Last Polygon Dataset Parse
  double CameraLookAt[10];            // Camera Look At (fov, position[3], up[3], eye[3])
  bool GradientBackground;            // If the scene use a gradient background
  double Background1[3];              // Background color of the rendering screen (RGB)
  double Background2[3];              // Second background color
  double SceneSize[3];                // Size of the bounding box of the scene
  std::string SceneId;                // Id of the parsed scene
  float CenterOfRotation[3];          // Center Of Rotation
  int meshObjMaxSize, lineObjMaxSize; // Max size of object allowed (faces)
  std::string renderersMetaData;
  bool hasWidget;

private:
  vtkWebGLExporter(const vtkWebGLExporter&) = delete;
  void operator=(const vtkWebGLExporter&) = delete;

  class vtkInternal;
  vtkInternal* Internal;
};

VTK_ABI_NAMESPACE_END
#endif
