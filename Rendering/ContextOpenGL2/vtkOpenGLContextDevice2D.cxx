// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkOpenGLContextDevice2D.h"

#include "vtkAbstractContextBufferId.h"
#include "vtkBrush.h"
#include "vtkContext2D.h"
#include "vtkFloatArray.h"
#include "vtkImageData.h"
#include "vtkImageResize.h"
#include "vtkMath.h"
#include "vtkMatrix3x3.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkOpenGLContextDeviceBufferObjectBuilder.h"
#include "vtkOpenGLError.h"
#include "vtkOpenGLGL2PSHelper.h"
#include "vtkOpenGLHelper.h"
#include "vtkOpenGLIndexBufferObject.h"
#include "vtkOpenGLRenderTimerLog.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkOpenGLRenderer.h"
#include "vtkOpenGLShaderCache.h"
#include "vtkOpenGLState.h"
#include "vtkOpenGLTexture.h"
#include "vtkOpenGLVertexArrayObject.h"
#include "vtkOpenGLVertexBufferObject.h"
#include "vtkOpenGLVertexBufferObjectCache.h"
#include "vtkOpenGLVertexBufferObjectGroup.h"
#include "vtkPath.h"
#include "vtkPen.h"
#include "vtkPointData.h"
#include "vtkPoints.h"
#include "vtkPoints2D.h"
#include "vtkPolyData.h"
#include "vtkRect.h"
#include "vtkRenderTimerLog.h"
#include "vtkSetGet.h"
#include "vtkShaderProgram.h"
#include "vtkSmartPointer.h"
#include "vtkTextProperty.h"
#include "vtkTextRenderer.h"
#include "vtkTexture.h"
#include "vtkTextureUnitManager.h"
#include "vtkTransform.h"
#include "vtkTransformFeedback.h"
#include "vtkUnsignedCharArray.h"
#include "vtkVector.h"
#include "vtkViewport.h"
#include "vtkWindow.h"

#include "vtkOpenGLContextDevice2DPrivate.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <sstream>

#define BUFFER_OFFSET(i) (reinterpret_cast<char*>(i))

VTK_ABI_NAMESPACE_BEGIN
namespace
{
void copyColors(std::vector<unsigned char>& newColors, unsigned char* colors, int nc)
{
  for (int j = 0; j < nc; j++)
  {
    newColors.push_back(colors[j]);
  }
}

const char* myVertShader = "in vec2 vertexMC;\n"
                           "uniform mat4 WCDCMatrix;\n"
                           "uniform mat4 MCWCMatrix;\n"
                           "uniform float pointSize;\n"
                           "#ifdef haveColors\n"
                           "in vec4 vertexScalar;\n"
                           "out vec4 vertexColor;\n"
                           "#endif\n"
                           "#ifdef haveTCoords\n"
                           "in vec2 tcoordMC;\n"
                           "out vec2 tcoord;\n"
                           "#endif\n"
                           "#ifdef haveLines\n"
                           "in vec2 tcoordMC;\n"
                           "out float ldistance;\n"
                           "#endif\n"
                           "void main() {\n"
                           "#ifdef haveColors\n"
                           "vertexColor = vertexScalar;\n"
                           "#endif\n"
                           "#ifdef haveTCoords\n"
                           "tcoord = tcoordMC;\n"
                           "#endif\n"
                           "#ifdef haveLines\n"
                           "ldistance = tcoordMC.x;\n"
                           "#endif\n"
                           "vec4 vertex = vec4(vertexMC.xy, 0.0, 1.0);\n"
                           "gl_PointSize = pointSize;\n"
                           "gl_Position = vertex*MCWCMatrix*WCDCMatrix; }\n";

const char* myFragShader = "//VTK::Output::Dec\n"
                           "#ifdef haveColors\n"
                           "in vec4 vertexColor;\n"
                           "#else\n"
                           "uniform vec4 vertexColor;\n"
                           "#endif\n"
                           "#ifdef haveTCoords\n"
                           "in vec2 tcoord;\n"
                           "uniform sampler2D texture1;\n"
                           "#endif\n"
                           "#ifdef haveLines\n"
                           "in float ldistance;\n"
                           "uniform int stipple;\n"
                           "#endif\n"
                           "void main() {\n"
                           "#ifdef haveLines\n"
                           "if ((0x01 << int(mod(ldistance,16.0)) & stipple) == 0) { discard; }\n"
                           "#endif\n"
                           "#ifdef haveTCoords\n"
                           " gl_FragData[0] = texture2D(texture1, tcoord);\n"
                           "#else\n"
                           " gl_FragData[0] = vertexColor;\n"
                           "#endif\n"
                           "}\n";

//------------------------------------------------------------------------------
// Returns true when rendering the GL2PS background raster image. Vectorizable
// primitives should not be drawn during these passes.
bool SkipDraw()
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  return gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Background;
}

//------------------------------------------------------------------------------
// Releases the current shader program if it is inconsistent with the GL2PS
// capture state. Returns the current OpenGLGL2PSHelper instance if one exists.
vtkOpenGLGL2PSHelper* PrepProgramForGL2PS(vtkOpenGLHelper& helper)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
  {
    // Always recreate the program when doing GL2PS capture.
    if (helper.Program)
    {
      helper.ReleaseGraphicsResources(nullptr);
    }
  }
  else
  {
    // If there is a feedback transform capturer set on the current shader
    // program and we're not capturing, recreate the program.
    if (helper.Program && helper.Program->GetTransformFeedback())
    {
      helper.ReleaseGraphicsResources(nullptr);
    }
  }

  return gl2ps;
}

//------------------------------------------------------------------------------
// Call before glDraw* commands to ensure that vertices are properly captured
// for GL2PS export.
void PreDraw(vtkOpenGLHelper& helper, int drawMode, size_t numVerts)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture && helper.Program)
  {
    if (vtkTransformFeedback* tfc = helper.Program->GetTransformFeedback())
    {
      tfc->SetNumberOfVertices(drawMode, numVerts);
      tfc->BindBuffer();
    }
  }
}

//------------------------------------------------------------------------------
// Call after glDraw* commands to ensure that vertices are properly captured
// for GL2PS export.
void PostDraw(vtkOpenGLHelper& helper, vtkRenderer* ren, unsigned char col[4])
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture && helper.Program)
  {
    if (vtkTransformFeedback* tfc = helper.Program->GetTransformFeedback())
    {
      tfc->ReadBuffer();
      tfc->ReleaseGraphicsResources();
      gl2ps->ProcessTransformFeedback(tfc, ren, col);
      tfc->ReleaseBufferData();
    }
  }
}

//------------------------------------------------------------------------------
// Returns true if the startAngle and stopAngle (as used in the ellipse drawing
// functions) describe a full circle.
inline bool IsFullCircle(float startAngle, float stopAngle)
{
  // A small number practical for rendering purposes.
  const float TOL = 1e-5f;

  return std::fabs(stopAngle - startAngle) + TOL >= 360.f;
}

} // end anon namespace

//------------------------------------------------------------------------------
vtkStandardNewMacro(vtkOpenGLContextDevice2D);

//------------------------------------------------------------------------------
vtkOpenGLContextDevice2D::vtkOpenGLContextDevice2D()
{
  this->Renderer = nullptr;
  this->InRender = false;
  this->Storage = new vtkOpenGLContextDevice2D::Private;
  this->PolyDataImpl = new vtkOpenGLContextDevice2D::CellArrayHelper(this);
  this->RenderWindow = nullptr;
  this->MaximumMarkerCacheSize = 20;
  this->ProjectionMatrix = vtkTransform::New();
  this->ModelMatrix = vtkTransform::New();
  this->VBO = new vtkOpenGLHelper;
  this->VCBO = new vtkOpenGLHelper;
  this->LinesBO = new vtkOpenGLHelper;
  this->LinesCBO = new vtkOpenGLHelper;
  this->VTBO = new vtkOpenGLHelper;
  this->SBO = new vtkOpenGLHelper;
  this->SCBO = new vtkOpenGLHelper;
  this->LinePattern = 0xFFFF;
}

//------------------------------------------------------------------------------
vtkOpenGLContextDevice2D::~vtkOpenGLContextDevice2D()
{
  delete this->VBO;
  this->VBO = nullptr;
  delete this->VCBO;
  this->VCBO = nullptr;
  delete this->LinesBO;
  this->LinesBO = nullptr;
  delete this->LinesCBO;
  this->LinesCBO = nullptr;
  delete this->SBO;
  this->SBO = nullptr;
  delete this->SCBO;
  this->SCBO = nullptr;
  delete this->VTBO;
  this->VTBO = nullptr;

  while (!this->MarkerCache.empty())
  {
    this->MarkerCache.back().Value->Delete();
    this->MarkerCache.pop_back();
  }

  this->ProjectionMatrix->Delete();
  this->ModelMatrix->Delete();
  delete this->Storage;
  delete this->PolyDataImpl;
}

vtkMatrix4x4* vtkOpenGLContextDevice2D::GetProjectionMatrix()
{
  return this->ProjectionMatrix->GetMatrix();
}

vtkMatrix4x4* vtkOpenGLContextDevice2D::GetModelMatrix()
{
  return this->ModelMatrix->GetMatrix();
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::Begin(vtkViewport* viewport)
{
  vtkOpenGLClearErrorMacro();
  // Need the actual pixel size of the viewport - ask OpenGL.
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  this->Storage->Offset.Set(static_cast<int>(vp[0]), static_cast<int>(vp[1]));

  this->Storage->Dim.Set(static_cast<int>(vp[2]), static_cast<int>(vp[3]));

  // push a 2D matrix on the stack
  this->ProjectionMatrix->Push();
  this->ProjectionMatrix->Identity();
  this->PushMatrix();
  this->ModelMatrix->Identity();

  double offset = 0.5;
  double xmin = offset;
  double xmax = vp[2] + offset - 1.0;
  double ymin = offset;
  double ymax = vp[3] + offset - 1.0;
  double znear = -2000;
  double zfar = 2000;

  double matrix[4][4];
  vtkMatrix4x4::Identity(*matrix);

  matrix[0][0] = 2 / (xmax - xmin);
  matrix[1][1] = 2 / (ymax - ymin);
  matrix[2][2] = -2 / (zfar - znear);

  matrix[0][3] = -(xmin + xmax) / (xmax - xmin);
  matrix[1][3] = -(ymin + ymax) / (ymax - ymin);
  matrix[2][3] = -(znear + zfar) / (zfar - znear);

  this->ProjectionMatrix->SetMatrix(*matrix);

  // Store the previous state before changing it
  this->Renderer = vtkRenderer::SafeDownCast(viewport);
  this->RenderWindow = vtkOpenGLRenderWindow::SafeDownCast(this->Renderer->GetRenderWindow());
  vtkOpenGLState* ostate = this->RenderWindow->GetState();

  this->Storage->SaveGLState(ostate);
  ostate->vtkglDisable(GL_DEPTH_TEST);
  ostate->vtkglEnable(GL_BLEND);

  this->RenderWindow->GetShaderCache()->ReleaseCurrentShader();

  // Enable simple line smoothing if multisampling is on.
#ifdef GL_LINE_SMOOTH
  if (this->Renderer->GetRenderWindow()->GetMultiSamples())
  {
    this->RenderWindow->GetState()->vtkglEnable(GL_LINE_SMOOTH);
  }
#endif

  this->InRender = true;
  vtkOpenGLCheckErrorMacro("failed after Begin");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::End()
{
  if (!this->InRender)
  {
    return;
  }

  this->ProjectionMatrix->Pop();
  this->PopMatrix();

  vtkOpenGLClearErrorMacro();

  // Restore the GL state that we changed
  vtkOpenGLState* ostate = this->RenderWindow->GetState();
  this->Storage->RestoreGLState(ostate);

  // Disable simple line smoothing if multisampling is on.
#ifdef GL_LINE_SMOOTH
  if (this->Renderer->GetRenderWindow()->GetMultiSamples())
  {
    this->RenderWindow->GetState()->vtkglDisable(GL_LINE_SMOOTH);
  }
#endif

  this->PolyDataImpl->HandleEndFrame();

  this->RenderWindow = nullptr;
  this->InRender = false;

  vtkOpenGLCheckErrorMacro("failed after End");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::BufferIdModeBegin(vtkAbstractContextBufferId* bufferId)
{
  assert("pre: not_yet" && !this->GetBufferIdMode());
  assert("pre: bufferId_exists" && bufferId != nullptr);

  vtkOpenGLClearErrorMacro();

  this->BufferId = bufferId;

  // Save OpenGL state.
  vtkOpenGLState* ostate = this->RenderWindow->GetState();
  this->Storage->SaveGLState(ostate, true);

  int lowerLeft[2];
  int usize, vsize;
  this->Renderer->GetTiledSizeAndOrigin(&usize, &vsize, lowerLeft, lowerLeft + 1);

  // push a 2D matrix on the stack
  this->ProjectionMatrix->Push();
  this->ProjectionMatrix->Identity();
  this->PushMatrix();
  this->ModelMatrix->Identity();

  double xmin = 0.5;
  double xmax = usize + 0.5;
  double ymin = 0.5;
  double ymax = vsize + 0.5;
  double znear = -1;
  double zfar = 1;

  double matrix[4][4];
  vtkMatrix4x4::Identity(*matrix);

  matrix[0][0] = 2 / (xmax - xmin);
  matrix[1][1] = 2 / (ymax - ymin);
  matrix[2][2] = -2 / (zfar - znear);

  matrix[0][3] = -(xmin + xmax) / (xmax - xmin);
  matrix[1][3] = -(ymin + ymax) / (ymax - ymin);
  matrix[2][3] = -(znear + zfar) / (zfar - znear);

  this->ProjectionMatrix->SetMatrix(*matrix);

  ostate->vtkglDrawBuffer(GL_BACK_LEFT);
  ostate->vtkglClearColor(0.0, 0.0, 0.0, 0.0); // id=0 means no hit, just background
  ostate->vtkglClear(GL_COLOR_BUFFER_BIT);
  ostate->vtkglDisable(GL_STENCIL_TEST);
  ostate->vtkglDisable(GL_DEPTH_TEST);
  ostate->vtkglDisable(GL_BLEND);

  vtkOpenGLCheckErrorMacro("failed after BufferIdModeBegin");

  assert("post: started" && this->GetBufferIdMode());
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::BufferIdModeEnd()
{
  assert("pre: started" && this->GetBufferIdMode());

  vtkOpenGLClearErrorMacro();

  // Assume the renderer has been set previously during rendering (sse Begin())
  int lowerLeft[2];
  int usize, vsize;
  this->Renderer->GetTiledSizeAndOrigin(&usize, &vsize, lowerLeft, lowerLeft + 1);
  this->BufferId->SetValues(lowerLeft[0], lowerLeft[1]);

  this->ProjectionMatrix->Pop();
  this->PopMatrix();

  this->Storage->RestoreGLState(this->RenderWindow->GetState(), true);

  this->BufferId = nullptr;

  vtkOpenGLCheckErrorMacro("failed after BufferIdModeEnd");

  assert("post: done" && !this->GetBufferIdMode());
}

void vtkOpenGLContextDevice2D::SetMatrices(vtkShaderProgram* prog)
{
  prog->SetUniformMatrix("WCDCMatrix", this->ProjectionMatrix->GetMatrix());
  prog->SetUniformMatrix("MCWCMatrix", this->ModelMatrix->GetMatrix());
}

void vtkOpenGLContextDevice2D::BuildVBO(
  vtkOpenGLHelper* cellBO, float* f, int nv, unsigned char* colors, int nc, float* tcoords)
{
  // build up temporary vtkDataArrays without copying the data.
  vtkNew<vtkFloatArray> positionsArray;
  vtkNew<vtkUnsignedCharArray> colorsArray;
  vtkNew<vtkFloatArray> tcoordsArray;

  positionsArray->SetNumberOfComponents(2);
  positionsArray->SetArray(f, nv * 2, 1); // do not take ownership of 'f'

  colorsArray->SetNumberOfComponents(nc);
  colorsArray->SetArray(colors, nv * nc, 1); // do not take ownership of 'colors'

  tcoordsArray->SetNumberOfComponents(2);
  tcoordsArray->SetArray(tcoords, nv * 2, 1); // do not take ownership of 'tcoords'

  // use 'anonymous' cache identifier because of raw typed array pointers.
  this->Storage->BufferObjectBuilder.BuildVBO(
    cellBO, positionsArray, colorsArray, tcoordsArray, /*cacheIdentifier=*/0, this->RenderWindow);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ReadyVBOProgram()
{
  vtkOpenGLGL2PSHelper* gl2ps = PrepProgramForGL2PS(*this->VBO);

  if (!this->VBO->Program)
  {
    vtkTransformFeedback* tf = nullptr;
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      tf = vtkTransformFeedback::New();
      tf->AddVarying(vtkTransformFeedback::Vertex_ClipCoordinate_F, "gl_Position");
    }
    std::string vs = "//VTK::System::Dec\n";
    vs += myVertShader;
    std::string fs = "//VTK::System::Dec\n";
    fs += myFragShader;
    this->VBO->Program =
      this->RenderWindow->GetShaderCache()->ReadyShaderProgram(vs.c_str(), fs.c_str(), "", tf);
    if (tf)
    {
      tf->Delete();
      tf = nullptr;
    }
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->VBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadyVCBOProgram()
{
  vtkOpenGLGL2PSHelper* gl2ps = PrepProgramForGL2PS(*this->VCBO);

  if (!this->VCBO->Program)
  {
    vtkTransformFeedback* tf = nullptr;
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      tf = vtkTransformFeedback::New();
      tf->AddVarying(vtkTransformFeedback::Vertex_ClipCoordinate_F, "gl_Position");
      tf->AddVarying(vtkTransformFeedback::Color_RGBA_F, "vertexColor");
    }
    std::string vs = "//VTK::System::Dec\n#define haveColors\n";
    vs += myVertShader;
    std::string fs = "//VTK::System::Dec\n#define haveColors\n";
    fs += myFragShader;
    this->VCBO->Program =
      this->RenderWindow->GetShaderCache()->ReadyShaderProgram(vs.c_str(), fs.c_str(), "", tf);
    if (tf)
    {
      tf->Delete();
      tf = nullptr;
    }
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->VCBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadyLinesBOProgram()
{
  vtkOpenGLGL2PSHelper* gl2ps = PrepProgramForGL2PS(*this->LinesBO);

  if (!this->LinesBO->Program)
  {
    vtkTransformFeedback* tf = nullptr;
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      tf = vtkTransformFeedback::New();
      tf->AddVarying(vtkTransformFeedback::Vertex_ClipCoordinate_F, "gl_Position");
    }
    std::string vs = "//VTK::System::Dec\n#define haveLines\n";
    vs += myVertShader;
    std::string fs = "//VTK::System::Dec\n#define haveLines\n";
    fs += myFragShader;
    this->LinesBO->Program =
      this->RenderWindow->GetShaderCache()->ReadyShaderProgram(vs.c_str(), fs.c_str(), "", tf);
    if (tf)
    {
      tf->Delete();
      tf = nullptr;
    }
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->LinesBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadyLinesCBOProgram()
{
  vtkOpenGLGL2PSHelper* gl2ps = PrepProgramForGL2PS(*this->LinesCBO);

  if (!this->LinesCBO->Program)
  {
    vtkTransformFeedback* tf = nullptr;
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      tf = vtkTransformFeedback::New();
      tf->AddVarying(vtkTransformFeedback::Vertex_ClipCoordinate_F, "gl_Position");
      tf->AddVarying(vtkTransformFeedback::Color_RGBA_F, "vertexColor");
    }
    std::string vs = "//VTK::System::Dec\n#define haveColors\n#define haveLines\n";
    vs += myVertShader;
    std::string fs = "//VTK::System::Dec\n#define haveColors\n#define haveLines\n";
    fs += myFragShader;
    this->LinesCBO->Program =
      this->RenderWindow->GetShaderCache()->ReadyShaderProgram(vs.c_str(), fs.c_str(), "", tf);
    if (tf)
    {
      tf->Delete();
      tf = nullptr;
    }
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->LinesCBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadyVTBOProgram()
{
  if (!this->VTBO->Program)
  {
    std::string vs = "//VTK::System::Dec\n#define haveTCoords\n";
    vs += myVertShader;
    std::string fs = "//VTK::System::Dec\n#define haveTCoords\n";
    fs += myFragShader;
    this->VTBO->Program =
      this->RenderWindow->GetShaderCache()->ReadyShaderProgram(vs.c_str(), fs.c_str(), "");
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->VTBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadySBOProgram()
{
  if (!this->SBO->Program)
  {
    this->SBO->Program = this->RenderWindow->GetShaderCache()->ReadyShaderProgram(
      // vertex shader
      "//VTK::System::Dec\n"
      "in vec2 vertexMC;\n"
      "uniform mat4 WCDCMatrix;\n"
      "uniform mat4 MCWCMatrix;\n"
      "uniform float pointSize;\n"
      "void main() {\n"
      "vec4 vertex = vec4(vertexMC.xy, 0.0, 1.0);\n"
      "gl_PointSize = pointSize;\n"
      "gl_Position = vertex*MCWCMatrix*WCDCMatrix; }\n",
      // fragment shader
      "//VTK::System::Dec\n"
      "//VTK::Output::Dec\n"
      "uniform vec4 vertexColor;\n"
      "uniform sampler2D texture1;\n"
      "void main() { gl_FragData[0] = vertexColor*texture2D(texture1, gl_PointCoord); }",
      // geometry shader
      "");
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->SBO->Program);
  }
}

void vtkOpenGLContextDevice2D::ReadySCBOProgram()
{
  if (!this->SCBO->Program)
  {
    this->SCBO->Program = this->RenderWindow->GetShaderCache()->ReadyShaderProgram(
      // vertex shader
      "//VTK::System::Dec\n"
      "in vec2 vertexMC;\n"
      "in vec4 vertexScalar;\n"
      "uniform mat4 WCDCMatrix;\n"
      "uniform mat4 MCWCMatrix;\n"
      "uniform float pointSize;\n"
      "out vec4 vertexColor;\n"
      "void main() {\n"
      "vec4 vertex = vec4(vertexMC.xy, 0.0, 1.0);\n"
      "vertexColor = vertexScalar;\n"
      "gl_PointSize = pointSize;\n"
      "gl_Position = vertex*MCWCMatrix*WCDCMatrix; }\n",
      // fragment shader
      "//VTK::System::Dec\n"
      "//VTK::Output::Dec\n"
      "in vec4 vertexColor;\n"
      "uniform sampler2D texture1;\n"
      "void main() { gl_FragData[0] = vertexColor*texture2D(texture1, gl_PointCoord); }",
      // geometry shader
      "");
  }
  else
  {
    this->RenderWindow->GetShaderCache()->ReadyShaderProgram(this->SCBO->Program);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPoly(float* f, int n, unsigned char* colors, int nc)
{
  assert("f must be non-null" && f != nullptr);
  assert("n must be greater than 0" && n > 0);

  if (SkipDraw())
  {
    return;
  }

  if (this->Pen->GetLineType() == vtkPen::NO_PEN)
  {
    return;
  }

  // Skip transparent elements.
  if (!colors && this->Pen->GetColorObject().GetAlpha() == 0)
  {
    return;
  }

  vtkOpenGLClearErrorMacro();
  this->SetLineType(this->Pen->GetLineType());

  vtkOpenGLHelper* cbo = nullptr;
  if (colors)
  {
    this->ReadyLinesCBOProgram();
    cbo = this->LinesCBO;
  }
  else
  {
    this->ReadyLinesBOProgram();
    cbo = this->LinesBO;
    if (cbo->Program)
    {
      cbo->Program->SetUniform4uc("vertexColor", this->Pen->GetColor());
    }
  }
  if (!cbo->Program)
  {
    return;
  }

  cbo->Program->SetUniformi("stipple", this->LinePattern);

  this->SetMatrices(cbo->Program);

  // for line stipple we need to compute the scaled
  // cumulative linear distance
  double* scale = this->ModelMatrix->GetScale();
  std::vector<float> distances;
  distances.resize(n * 2);
  float totDist = 0.0;
  distances[0] = 0.0;
  for (int i = 1; i < n; i++)
  {
    float xDel = scale[0] * (f[i * 2] - f[i * 2 - 2]);
    float yDel = scale[1] * (f[i * 2 + 1] - f[i * 2 - 1]);
    // discarding infinite coordinates
    totDist += (std::abs(yDel) != std::numeric_limits<float>::infinity() &&
                 std::abs(xDel) != std::numeric_limits<float>::infinity())
      ? sqrt(xDel * xDel + yDel * yDel)
      : 0;
    distances[i * 2] = totDist;
  }

  // For GL2PS captures, use the path that draws lines instead of triangles --
  // GL2PS can handle stipples and linewidths just fine.
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();

  if (this->Pen->GetWidth() > 1.0 &&
    !(gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture))
  {
    // convert to triangles and draw, this is because
    // OpenGL no longer supports wide lines directly
    float hwidth = this->Pen->GetWidth() / 2.0;
    std::vector<float> newVerts;
    std::vector<unsigned char> newColors;
    std::vector<float> newDistances;
    newDistances.resize((n - 1) * 12);
    for (int i = 0; i < n - 1; i++)
    {
      // for each line segment draw two triangles
      // start by computing the direction
      vtkVector2f dir(
        (f[i * 2 + 2] - f[i * 2]) * scale[0], (f[i * 2 + 3] - f[i * 2 + 1]) * scale[1]);
      vtkVector2f norm(-dir.GetY(), dir.GetX());
      norm.Normalize();
      norm.SetX(hwidth * norm.GetX() / scale[0]);
      norm.SetY(hwidth * norm.GetY() / scale[1]);

      newVerts.push_back(f[i * 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 1] + norm.GetY());
      newVerts.push_back(f[i * 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 1] - norm.GetY());
      newVerts.push_back(f[i * 2 + 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 3] - norm.GetY());

      newVerts.push_back(f[i * 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 1] + norm.GetY());
      newVerts.push_back(f[i * 2 + 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 3] - norm.GetY());
      newVerts.push_back(f[i * 2 + 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 3] + norm.GetY());

      if (colors)
      {
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
      }

      newDistances[i * 12] = distances[i * 2];
      newDistances[i * 12 + 2] = distances[i * 2];
      newDistances[i * 12 + 4] = distances[i * 2 + 2];
      newDistances[i * 12 + 6] = distances[i * 2];
      newDistances[i * 12 + 8] = distances[i * 2 + 2];
      newDistances[i * 12 + 10] = distances[i * 2 + 2];
    }

    this->BuildVBO(cbo, newVerts.data(), static_cast<int>(newVerts.size() / 2),
      colors ? newColors.data() : nullptr, nc, newDistances.data());

    PreDraw(*cbo, GL_TRIANGLES, newVerts.size() / 2);
    auto timer = this->RenderWindow->GetRenderTimer();
    VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
        << "::" << __func__ << "|glDrawArrays(cacheIdentifier: "
        << "null,"
        << "mode:GL_TRIANGLES,n:" << static_cast<GLsizei>(newVerts.size() / 2),
      timer);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(newVerts.size() / 2));
    PostDraw(*cbo, this->Renderer, this->Pen->GetColor());
  }
  else
  {
    this->SetLineWidth(this->Pen->GetWidth());
    this->BuildVBO(cbo, f, n, colors, nc, distances.data());
    PreDraw(*cbo, GL_LINE_STRIP, n);
    auto timer = this->RenderWindow->GetRenderTimer();
    VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
        << "::" << __func__ << "|glDrawArrays(cacheIdentifier: "
        << "null,"
        << "mode:GL_LINE_STRIP,n:" << n,
      timer);
    glDrawArrays(GL_LINE_STRIP, 0, n);
    PostDraw(*cbo, this->Renderer, this->Pen->GetColor());
    this->SetLineWidth(1.0);
  }

  vtkOpenGLCheckErrorMacro("failed after DrawPoly");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawLines(float* f, int n, unsigned char* colors, int nc)
{
  assert("f must be non-null" && f != nullptr);
  assert("n must be greater than 0" && n > 0);

  if (SkipDraw())
  {
    return;
  }

  if (this->Pen->GetLineType() == vtkPen::NO_PEN)
  {
    return;
  }

  // Skip transparent elements.
  if (!colors && this->Pen->GetColorObject().GetAlpha() == 0)
  {
    return;
  }

  vtkOpenGLClearErrorMacro();

  this->SetLineType(this->Pen->GetLineType());

  vtkOpenGLHelper* cbo = nullptr;
  if (colors)
  {
    this->ReadyLinesCBOProgram();
    cbo = this->LinesCBO;
  }
  else
  {
    this->ReadyLinesBOProgram();
    cbo = this->LinesBO;
    if (!cbo->Program)
    {
      return;
    }
    cbo->Program->SetUniform4uc("vertexColor", this->Pen->GetColor());
  }
  if (!cbo->Program)
  {
    return;
  }

  cbo->Program->SetUniformi("stipple", this->LinePattern);

  this->SetMatrices(cbo->Program);

  // for line stipple we need to compute the scaled
  // cumulative linear distance
  double* scale = this->ModelMatrix->GetScale();
  std::vector<float> distances;
  distances.resize(n * 2);
  float totDist = 0.0;
  distances[0] = 0.0;
  for (int i = 1; i < n; i++)
  {
    float xDel = scale[0] * (f[i * 2] - f[i * 2 - 2]);
    float yDel = scale[1] * (f[i * 2 + 1] - f[i * 2 - 1]);
    totDist += sqrt(xDel * xDel + yDel * yDel);
    distances[i * 2] = totDist;
  }

  if (this->Pen->GetWidth() > 1.0)
  {
    // convert to triangles and draw, this is because
    // OpenGL no longer supports wide lines directly
    float hwidth = this->Pen->GetWidth() / 2.0;
    std::vector<float> newVerts;
    std::vector<unsigned char> newColors;
    std::vector<float> newDistances;
    newDistances.resize((n / 2) * 12);
    for (int i = 0; i < n - 1; i += 2)
    {
      // for each line segment draw two triangles
      // start by computing the direction
      vtkVector2f dir(
        (f[i * 2 + 2] - f[i * 2]) * scale[0], (f[i * 2 + 3] - f[i * 2 + 1]) * scale[1]);
      vtkVector2f norm(-dir.GetY(), dir.GetX());
      norm.Normalize();
      norm.SetX(hwidth * norm.GetX() / scale[0]);
      norm.SetY(hwidth * norm.GetY() / scale[1]);

      newVerts.push_back(f[i * 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 1] + norm.GetY());
      newVerts.push_back(f[i * 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 1] - norm.GetY());
      newVerts.push_back(f[i * 2 + 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 3] - norm.GetY());

      newVerts.push_back(f[i * 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 1] + norm.GetY());
      newVerts.push_back(f[i * 2 + 2] - norm.GetX());
      newVerts.push_back(f[i * 2 + 3] - norm.GetY());
      newVerts.push_back(f[i * 2 + 2] + norm.GetX());
      newVerts.push_back(f[i * 2 + 3] + norm.GetY());

      if (colors)
      {
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
        copyColors(newColors, colors + i * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
        copyColors(newColors, colors + (i + 1) * nc, nc);
      }

      newDistances[i * 6] = distances[i * 2];
      newDistances[i * 6 + 2] = distances[i * 2];
      newDistances[i * 6 + 4] = distances[i * 2 + 2];
      newDistances[i * 6 + 6] = distances[i * 2];
      newDistances[i * 6 + 8] = distances[i * 2 + 2];
      newDistances[i * 6 + 10] = distances[i * 2 + 2];
    }

    this->BuildVBO(cbo, newVerts.data(), static_cast<int>(newVerts.size() / 2),
      colors ? newColors.data() : nullptr, nc, newDistances.data());
    PreDraw(*cbo, GL_TRIANGLES, newVerts.size() / 2);
    auto timer = this->RenderWindow->GetRenderTimer();
    VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
        << "::" << __func__ << "|glDrawArrays(cacheIdentifier: "
        << "null,"
        << "mode:GL_TRIANGLES,n:" << static_cast<GLsizei>(newVerts.size() / 2),
      timer);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(newVerts.size() / 2));
    PostDraw(*cbo, this->Renderer, this->Pen->GetColor());
  }
  else
  {
    this->SetLineWidth(this->Pen->GetWidth());
    this->BuildVBO(cbo, f, n, colors, nc, distances.data());
    PreDraw(*cbo, GL_LINES, n);
    auto timer = this->RenderWindow->GetRenderTimer();
    VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
        << "::" << __func__ << "|glDrawArrays(cacheIdentifier: "
        << "null,"
        << "mode:GL_LINES,n:" << n,
      timer);
    glDrawArrays(GL_LINES, 0, n);
    PostDraw(*cbo, this->Renderer, this->Pen->GetColor());
    this->SetLineWidth(1.0);
  }

  vtkOpenGLCheckErrorMacro("failed after DrawLines");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPoints(float* f, int n, unsigned char* c, int nc)
{
  // build up temporary vtkDataArrays without copying the data.
  vtkNew<vtkFloatArray> positionsArray;
  vtkNew<vtkUnsignedCharArray> colorsArray;
  vtkNew<vtkFloatArray> tcoordsArray;

  positionsArray->SetNumberOfComponents(2);
  positionsArray->SetArray(f, n * 2, 1); // do not take ownership of 'points'

  if (c != nullptr)
  {
    colorsArray->SetNumberOfComponents(nc);
    colorsArray->SetArray(c, n * nc, 1); // do not take ownership of 'colors'
  }

  // use 'anonymous' cache identifier because of raw typed array pointers.
  this->DrawPoints(positionsArray, colorsArray, /*cacheIdentifier=*/0);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPoints(
  vtkDataArray* positions, vtkUnsignedCharArray* colors, std::uintptr_t cacheIdentifier)
{
  if (SkipDraw())
  {
    return;
  }

  // Skip transparent elements.
  bool noColors = (colors == nullptr);
  noColors |= (colors && colors->GetNumberOfTuples() == 0);
  if (noColors && this->Pen->GetColorObject().GetAlpha() == 0)
  {
    return;
  }

  vtkOpenGLClearErrorMacro();

  vtkOpenGLHelper* cbo = nullptr;
  if (colors && colors->GetNumberOfTuples() > 0)
  {
    this->ReadyVCBOProgram();
    cbo = this->VCBO;
    if (!cbo->Program)
    {
      return;
    }
  }
  else
  {
    this->ReadyVBOProgram();
    cbo = this->VBO;
    if (!cbo->Program)
    {
      return;
    }
    cbo->Program->SetUniform4uc("vertexColor", this->Pen->GetColor());
  }

  this->SetPointSize(this->Pen->GetWidth());
#ifdef GL_ES_VERSION_3_0
  cbo->Program->SetUniformf("pointSize", this->Pen->GetWidth());
#endif

  this->Storage->BufferObjectBuilder.BuildVBO(
    cbo, positions, colors, nullptr, cacheIdentifier, this->RenderWindow);
  this->SetMatrices(cbo->Program);

  PreDraw(*cbo, GL_POINTS, positions->GetNumberOfTuples());
  auto timer = this->RenderWindow->GetRenderTimer();
  VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
      << "::" << __func__ << "|glDrawArrays(cacheIdentifier: " << cacheIdentifier
      << "mode:GL_POINTS,n:" << positions->GetNumberOfTuples(),
    timer);
  glDrawArrays(GL_POINTS, 0, positions->GetNumberOfTuples());
  PostDraw(*cbo, this->Renderer, this->Pen->GetColor());

  vtkOpenGLCheckErrorMacro("failed after DrawPoints");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPointSprites(
  vtkImageData* sprite, float* points, int n, unsigned char* colors, int nc_comps)
{
  // build up temporary vtkDataArrays without copying the data.
  vtkNew<vtkFloatArray> positionsArray;
  vtkNew<vtkUnsignedCharArray> colorsArray;
  vtkNew<vtkFloatArray> tcoordsArray;

  positionsArray->SetNumberOfComponents(2);
  positionsArray->SetArray(points, n * 2, 1); // do not take ownership of 'points'

  if (colors != nullptr)
  {
    colorsArray->SetNumberOfComponents(nc_comps);
    colorsArray->SetArray(colors, n * nc_comps, 1); // do not take ownership of 'colors'
  }

  // use 'anonymous' cache identifier because of raw typed array pointers.
  this->DrawPointSprites(sprite, positionsArray, colorsArray, /*cacheIdentifier=*/0);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPointSprites(vtkImageData* sprite, vtkDataArray* positions,
  vtkUnsignedCharArray* colors, std::uintptr_t cacheIdentifier)
{
  //  // Draw these to the background -- we don't currently export them to GL2PS.
  //  if (SkipDraw())
  //    {
  //    return;
  //    }

  vtkOpenGLClearErrorMacro();
  if (positions && positions->GetNumberOfTuples() > 0)
  {
    this->SetPointSize(this->Pen->GetWidth());

    vtkOpenGLHelper* cbo = nullptr;
    if (colors && colors->GetNumberOfTuples() > 0)
    {
      this->ReadySCBOProgram();
      cbo = this->SCBO;
      if (!cbo->Program)
      {
        return;
      }
    }
    else
    {
      this->ReadySBOProgram();
      cbo = this->SBO;
      if (!cbo->Program)
      {
        return;
      }
      cbo->Program->SetUniform4uc("vertexColor", this->Pen->GetColor());
    }
#ifdef GL_ES_VERSION_3_0
    cbo->Program->SetUniformf("pointSize", this->Pen->GetWidth());
#endif

    this->Storage->BufferObjectBuilder.BuildVBO(
      cbo, positions, colors, nullptr, cacheIdentifier, this->RenderWindow);
    this->SetMatrices(cbo->Program);

    if (sprite)
    {
      if (!this->Storage->SpriteTexture)
      {
        this->Storage->SpriteTexture = vtkTexture::New();
      }
      int properties = this->Brush->GetTextureProperties();
      this->Storage->SpriteTexture->SetInputData(sprite);
      this->Storage->SpriteTexture->SetRepeat(properties & vtkContextDevice2D::Repeat);
      this->Storage->SpriteTexture->SetInterpolate(properties & vtkContextDevice2D::Linear);
      this->Storage->SpriteTexture->Render(this->Renderer);
      int tunit = vtkOpenGLTexture::SafeDownCast(this->Storage->SpriteTexture)->GetTextureUnit();
      cbo->Program->SetUniformi("texture1", tunit);
    }

#ifdef GL_POINT_SPRITE
    // We can actually use point sprites here
    if (this->RenderWindow->IsPointSpriteBugPresent())
    {
      glEnable(GL_POINT_SPRITE);
      glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_TRUE);
    }
    glPointParameteri(GL_POINT_SPRITE_COORD_ORIGIN, GL_LOWER_LEFT);
#endif

    auto timer = this->RenderWindow->GetRenderTimer();
    VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
        << "::" << __func__ << "|glDrawArrays(cacheIdentifier: " << cacheIdentifier
        << "mode:GL_POINTS,n:" << positions->GetNumberOfTuples(),
      timer);
    glDrawArrays(GL_POINTS, 0, positions->GetNumberOfTuples());

#ifdef GL_POINT_SPRITE
    if (this->RenderWindow->IsPointSpriteBugPresent())
    {
      glTexEnvi(GL_POINT_SPRITE, GL_COORD_REPLACE, GL_FALSE);
      glDisable(GL_POINT_SPRITE);
    }
#endif

    if (sprite)
    {
      this->Storage->SpriteTexture->PostRender(this->Renderer);
    }
  }
  else
  {
    vtkWarningMacro(<< "Points supplied without a valid image or pointer.");
  }
  vtkOpenGLCheckErrorMacro("failed after DrawPointSprites");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawMarkers(
  int shape, bool highlight, float* points, int n, unsigned char* colors, int nc_comps)
{
  // build up temporary vtkDataArrays without copying the data.
  vtkNew<vtkFloatArray> positionsArray;
  vtkNew<vtkUnsignedCharArray> colorsArray;
  vtkNew<vtkFloatArray> tcoordsArray;

  positionsArray->SetNumberOfComponents(2);
  positionsArray->SetArray(points, n * 2, 1); // do not take ownership of 'points'

  if (colors != nullptr)
  {
    colorsArray->SetNumberOfComponents(nc_comps);
    colorsArray->SetArray(colors, n * nc_comps, 1); // do not take ownership of 'colors'
  }

  // use 'anonymous' cache identifier because of raw typed array pointers.
  this->DrawMarkers(shape, highlight, positionsArray, colorsArray, /*cacheIdentifier=*/0);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawMarkers(int shape, bool highlight, vtkDataArray* positions,
  vtkUnsignedCharArray* colors, std::uintptr_t cacheIdentifier)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps)
  {
    switch (gl2ps->GetActiveState())
    {
      case vtkOpenGLGL2PSHelper::Capture:
      {
        // i don't think anyone does interactive rendering to a gl2ps context.
        // grab raw pointers and draw.
        float* f = vtkArrayDownCast<vtkFloatArray>(positions)->GetPointer(0);
        int nv = positions->GetNumberOfTuples();
        auto c = colors->GetPointer(0);
        int nc_comps = colors->GetNumberOfComponents();
        this->DrawMarkersGL2PS(shape, highlight, f, nv, c, nc_comps);
        return;
      }
      case vtkOpenGLGL2PSHelper::Background:
        return; // Do nothing.
      case vtkOpenGLGL2PSHelper::Inactive:
        break; // Render as normal.
    }
  }

  // Get a point sprite for the shape
  vtkImageData* sprite = this->GetMarker(shape, this->Pen->GetWidth(), highlight);
  this->DrawPointSprites(sprite, positions, colors, cacheIdentifier);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawQuad(float* f, int n)
{
  if (SkipDraw())
  {
    return;
  }

  if (!f || n <= 0)
  {
    vtkWarningMacro(<< "Points supplied that were not of type float.");
    return;
  }

  // convert quads to triangles
  std::vector<float> tverts;
  int numTVerts = 6 * n / 4;
  tverts.resize(numTVerts * 2);
  int offset[6] = { 0, 1, 2, 0, 2, 3 };
  for (int i = 0; i < numTVerts; i++)
  {
    int index = 2 * (4 * (i / 6) + offset[i % 6]);
    tverts[i * 2] = f[index];
    tverts[i * 2 + 1] = f[index + 1];
  }

  this->CoreDrawTriangles(tverts);
}

void vtkOpenGLContextDevice2D::CoreDrawTriangles(
  std::vector<float>& tverts, unsigned char* colors, int numComp)
{
  if (SkipDraw())
  {
    return;
  }

  vtkOpenGLClearErrorMacro();

  float* texCoord = nullptr;
  vtkOpenGLHelper* cbo = nullptr;
  if (this->Brush->GetTexture())
  {
    this->ReadyVTBOProgram();
    cbo = this->VTBO;
    if (!cbo->Program)
    {
      return;
    }
    this->SetTexture(this->Brush->GetTexture(), this->Brush->GetTextureProperties());
    this->Storage->Texture->Render(this->Renderer);
    texCoord = this->Storage->TexCoords(tverts.data(), static_cast<int>(tverts.size() / 2));

    int tunit = vtkOpenGLTexture::SafeDownCast(this->Storage->Texture)->GetTextureUnit();
    cbo->Program->SetUniformi("texture1", tunit);
  }
  else if (colors && numComp > 0)
  {
    this->ReadyVCBOProgram();
    cbo = this->VCBO;
  }
  else
  {
    // Skip transparent elements.
    if (this->Brush->GetColorObject().GetAlpha() == 0)
    {
      return;
    }
    this->ReadyVBOProgram();
    cbo = this->VBO;
  }

  if (!cbo->Program)
  {
    return;
  }

  cbo->Program->SetUniform4uc("vertexColor", this->Brush->GetColor());

  this->BuildVBO(
    cbo, tverts.data(), static_cast<int>(tverts.size() / 2), colors, numComp, texCoord);

  this->SetMatrices(cbo->Program);

  PreDraw(*cbo, GL_TRIANGLES, tverts.size() / 2);

  auto timer = this->RenderWindow->GetRenderTimer();
  VTK_SCOPED_RENDER_EVENT(this->GetClassNameInternal()
      << "::" << __func__ << "|glDrawArrays(cacheIdentifier: "
      << "null,"
      << "mode:GL_TRIANGLES,n:" << static_cast<GLsizei>(tverts.size() / 2),
    timer);
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tverts.size() / 2));
  PostDraw(*cbo, this->Renderer, this->Brush->GetColor());

  if (this->Storage->Texture)
  {
    this->Storage->Texture->PostRender(this->Renderer);
    delete[] texCoord;
  }
  vtkOpenGLCheckErrorMacro("failed after DrawQuad");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawQuadStrip(float* f, int n)
{
  if (SkipDraw())
  {
    return;
  }

  if (!f || n <= 0)
  {
    vtkWarningMacro(<< "Points supplied that were not of type float.");
    return;
  }

  // convert quad strips to triangles
  std::vector<float> tverts;
  int numTVerts = 3 * (n - 2);
  tverts.resize(numTVerts * 2);
  int offset[6] = { 0, 1, 3, 0, 3, 2 };
  for (int i = 0; i < numTVerts; i++)
  {
    int index = 2 * (2 * (i / 6) + offset[i % 6]);
    tverts[i * 2] = f[index];
    tverts[i * 2 + 1] = f[index + 1];
  }

  this->CoreDrawTriangles(tverts);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPolygon(float* f, int n)
{
  if (SkipDraw())
  {
    return;
  }

  if (!f || n <= 0)
  {
    vtkWarningMacro(<< "Points supplied that were not of type float.");
    return;
  }

  // convert polygon to triangles
  std::vector<float> tverts;
  int numTVerts = 3 * (n - 2);
  tverts.reserve(numTVerts * 2);
  for (int i = 0; i < n - 2; i++)
  {
    tverts.push_back(f[0]);
    tverts.push_back(f[1]);
    tverts.push_back(f[i * 2 + 2]);
    tverts.push_back(f[i * 2 + 3]);
    tverts.push_back(f[i * 2 + 4]);
    tverts.push_back(f[i * 2 + 5]);
  }

  this->CoreDrawTriangles(tverts);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawColoredPolygon(
  float* f, int n, unsigned char* colors, int nc_comps)
{
  if (SkipDraw())
  {
    return;
  }

  if (!f || n <= 0)
  {
    vtkWarningMacro(<< "Points supplied that were not of type float.");
    return;
  }

  // convert polygon to triangles
  int numTVerts = 3 * (n - 2);

  std::vector<float> tverts;
  tverts.reserve(numTVerts * 2);

  std::vector<unsigned char> tcolors;
  if (colors)
  {
    tcolors.resize(numTVerts * nc_comps);
  }
  std::vector<unsigned char>::iterator colIt = tcolors.begin();

  for (int i = 0; i < n - 2; i++)
  {
    tverts.push_back(f[0]);
    tverts.push_back(f[1]);
    tverts.push_back(f[i * 2 + 2]);
    tverts.push_back(f[i * 2 + 3]);
    tverts.push_back(f[i * 2 + 4]);
    tverts.push_back(f[i * 2 + 5]);
    if (colors)
    {
      std::copy(colors, colors + nc_comps, colIt);
      colIt += nc_comps;
      std::copy(colors + ((i + 1) * nc_comps), colors + ((i + 3) * nc_comps), colIt);
      colIt += 2 * nc_comps;
    }
  }

  this->CoreDrawTriangles(tverts, colors ? tcolors.data() : nullptr, nc_comps);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawEllipseWedge(float x, float y, float outRx, float outRy,
  float inRx, float inRy, float startAngle, float stopAngle)

{
  assert("pre: positive_outRx" && outRx >= 0.0f);
  assert("pre: positive_outRy" && outRy >= 0.0f);
  assert("pre: positive_inRx" && inRx >= 0.0f);
  assert("pre: positive_inRy" && inRy >= 0.0f);
  assert("pre: ordered_rx" && inRx <= outRx);
  assert("pre: ordered_ry" && inRy <= outRy);

  if (SkipDraw())
  {
    return;
  }

  if (outRy == 0.0f && outRx == 0.0f)
  {
    // we make sure maxRadius will never be null.
    return;
  }

  // If the 'wedge' is actually a full circle, gl2ps can just insert a circle
  // instead of using a polygonal approximation.
  if (IsFullCircle(startAngle, stopAngle))
  {
    vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      this->DrawWedgeGL2PS(x, y, outRx, outRy, inRx, inRy);
      return;
    }
  }

  int iterations = this->GetNumberOfArcIterations(outRx, outRy, startAngle, stopAngle);

  // step in radians.
  double step = vtkMath::RadiansFromDegrees(stopAngle - startAngle) / (iterations);

  // step have to be lesser or equal to maxStep computed inside
  // GetNumberOfIterations()

  double rstart = vtkMath::RadiansFromDegrees(startAngle);

  // the A vertices (0,2,4,..) are on the inner side
  // the B vertices (1,3,5,..) are on the outer side
  // (A and B vertices terms come from triangle strip definition in
  // OpenGL spec)
  // we are iterating counterclockwise

  // convert polygon to triangles
  std::vector<float> tverts;
  int numTVerts = 6 * iterations;
  tverts.resize(numTVerts * 2);
  int offset[6] = { 0, 1, 3, 0, 3, 2 };
  for (int i = 0; i < numTVerts; i++)
  {
    int index = i / 6 + offset[i % 6] / 2;
    double radiusX = (offset[i % 6] % 2) ? outRx : inRx;
    double radiusY = (offset[i % 6] % 2) ? outRy : inRy;
    double a = rstart + index * step;
    tverts.push_back(radiusX * cos(a) + x);
    tverts.push_back(radiusY * sin(a) + y);
  }

  this->CoreDrawTriangles(tverts);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawEllipticArc(
  float x, float y, float rX, float rY, float startAngle, float stopAngle)
{
  assert("pre: positive_rX" && rX >= 0);
  assert("pre: positive_rY" && rY >= 0);

  if (SkipDraw())
  {
    return;
  }

  if (rX == 0.0f && rY == 0.0f)
  {
    // we make sure maxRadius will never be null.
    return;
  }

  // If the 'arc' is actually a full circle, gl2ps can just insert a circle
  // instead of using a polygonal approximation.
  if (IsFullCircle(startAngle, stopAngle))
  {
    vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
    if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
    {
      this->DrawCircleGL2PS(x, y, rX, rY);
      return;
    }
  }

  vtkOpenGLClearErrorMacro();

  int iterations = this->GetNumberOfArcIterations(rX, rY, startAngle, stopAngle);

  float* p = new float[2 * (iterations + 1)];

  // step in radians.
  double step = vtkMath::RadiansFromDegrees(stopAngle - startAngle) / (iterations);

  // step have to be lesser or equal to maxStep computed inside
  // GetNumberOfIterations()
  double rstart = vtkMath::RadiansFromDegrees(startAngle);

  // we are iterating counterclockwise
  for (int i = 0; i <= iterations; ++i)
  {
    double a = rstart + i * step;
    p[2 * i] = rX * cos(a) + x;
    p[2 * i + 1] = rY * sin(a) + y;
  }

  this->DrawPolygon(p, iterations + 1);
  this->DrawPoly(p, iterations + 1);
  delete[] p;

  vtkOpenGLCheckErrorMacro("failed after DrawEllipseArc");
}

//------------------------------------------------------------------------------
int vtkOpenGLContextDevice2D::GetNumberOfArcIterations(
  float rX, float rY, float startAngle, float stopAngle)
{
  assert("pre: positive_rX" && rX >= 0.0f);
  assert("pre: positive_rY" && rY >= 0.0f);
  assert("pre: not_both_null" && (rX > 0.0 || rY > 0.0));

  // 1.0: pixel precision. 0.5 (subpixel precision, useful with multisampling)
  double error = 4.0; // experience shows 4.0 is visually enough.

  // The tessellation is the most visible on the biggest radius.
  double maxRadius;
  if (rX >= rY)
  {
    maxRadius = rX;
  }
  else
  {
    maxRadius = rY;
  }

  if (error > maxRadius)
  {
    // to make sure the argument of asin() is in a valid range.
    error = maxRadius;
  }

  // Angle of a sector so that its chord is `error' pixels.
  // This is will be our maximum angle step.
  double maxStep = 2.0 * asin(error / (2.0 * maxRadius));

  // ceil because we want to make sure we don't underestimate the number of
  // iterations by 1.
  return static_cast<int>(ceil(vtkMath::RadiansFromDegrees(stopAngle - startAngle) / maxStep));
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ComputeStringBounds(const vtkStdString& string, float bounds[4])
{
  this->ComputeStringBoundsInternal(string, bounds);
  bounds[0] = 0.f;
  bounds[1] = 0.f;
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ComputeJustifiedStringBounds(const char* string, float bounds[4])
{
  this->ComputeStringBoundsInternal(string, bounds);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawString(float* point, const vtkStdString& string)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps)
  {
    switch (gl2ps->GetActiveState())
    {
      case vtkOpenGLGL2PSHelper::Capture:
      {
        float tx = point[0];
        float ty = point[1];
        this->TransformPoint(tx, ty);
        double x[3] = { tx, ty, 0. };
        gl2ps->DrawString(string, this->TextProp, x, 0., this->Renderer);
        return;
      }
      case vtkOpenGLGL2PSHelper::Background:
        return; // Do nothing.
      case vtkOpenGLGL2PSHelper::Inactive:
        break; // Render as normal.
    }
  }

  vtkTextRenderer* tren = vtkTextRenderer::GetInstance();
  if (!tren)
  {
    vtkErrorMacro("No text renderer available. Link to vtkRenderingFreeType "
                  "to get the default implementation.");
    return;
  }

  vtkOpenGLClearErrorMacro();

  double* mv = this->ModelMatrix->GetMatrix()->Element[0];
  float xScale = mv[0];
  float yScale = mv[5];

  float p[] = { std::floor(point[0] * xScale) / xScale, std::floor(point[1] * yScale) / yScale };

  // TODO this currently ignores vtkContextScene::ScaleTiles. Not sure how to
  // get at that from here, but this is better than ignoring scaling altogether.
  // TODO Also, FreeType supports anisotropic DPI. Might be needed if the
  // tileScale isn't homogeneous, but we'll need to update the textrenderer API
  // and see if MPL/mathtext can support it.
  int tileScale[2];
  this->RenderWindow->GetTileScale(tileScale);
  int dpi = this->RenderWindow->GetDPI() * std::max(tileScale[0], tileScale[1]);

  // Cache rendered text strings
  vtkTextureImageCache<UTF8TextPropertyKey>::CacheData& cache =
    this->Storage->TextTextureCache.GetCacheData(UTF8TextPropertyKey(this->TextProp, string, dpi));
  vtkImageData* image = cache.ImageData;
  if (image->GetNumberOfPoints() == 0 && image->GetNumberOfCells() == 0)
  {
    int textDims[2];
    if (!tren->RenderString(this->TextProp, string, image, textDims, dpi))
    {
      vtkErrorMacro("Error rendering string: " << string);
      return;
    }
    if (!tren->GetMetrics(this->TextProp, string, cache.Metrics, dpi))
    {
      vtkErrorMacro("Error computing bounding box for string: " << string);
      return;
    }
  }
  vtkTexture* texture = cache.Texture;
  texture->Render(this->Renderer);

  int imgDims[3];
  image->GetDimensions(imgDims);

  float textWidth =
    static_cast<float>(cache.Metrics.BoundingBox[1] - cache.Metrics.BoundingBox[0] + 1);
  float textHeight =
    static_cast<float>(cache.Metrics.BoundingBox[3] - cache.Metrics.BoundingBox[2] + 1);

  float width = textWidth / xScale;
  float height = textHeight / yScale;

  float xw = textWidth / static_cast<float>(imgDims[0]);
  float xh = textHeight / static_cast<float>(imgDims[1]);

  // Align the text (the 0 point of the bounding box is aligned to the
  // rotated and justified anchor point, so just translate by the bbox origin):
  p[0] += cache.Metrics.BoundingBox[0] / xScale;
  p[1] += cache.Metrics.BoundingBox[2] / yScale;

  float points[] = { p[0], p[1], p[0] + width, p[1], p[0] + width, p[1] + height, p[0], p[1],
    p[0] + width, p[1] + height, p[0], p[1] + height };

  float texCoord[] = { 0.0f, 0.0f, xw, 0.0f, xw, xh, 0.0f, 0.0f, xw, xh, 0.0f, xh };

  vtkOpenGLClearErrorMacro();

  this->ReadyVTBOProgram();
  vtkOpenGLHelper* cbo = this->VTBO;
  if (!cbo->Program)
  {
    return;
  }
  int tunit = vtkOpenGLTexture::SafeDownCast(texture)->GetTextureUnit();
  cbo->Program->SetUniformi("texture1", tunit);

  this->BuildVBO(cbo, points, 6, nullptr, 0, texCoord);
  this->SetMatrices(cbo->Program);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  texture->PostRender(this->Renderer);

  vtkOpenGLCheckErrorMacro("failed after DrawString");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawMathTextString(float point[2], const vtkStdString& string)
{
  // The default text renderer detects and handles mathtext now. Just use the
  // regular implementation.
  this->DrawString(point, string);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawImage(float p[2], float scale, vtkImageData* image)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps)
  {
    switch (gl2ps->GetActiveState())
    {
      case vtkOpenGLGL2PSHelper::Capture:
        this->DrawImageGL2PS(p, scale, image);
        return;
      case vtkOpenGLGL2PSHelper::Background:
        return; // Do nothing.
      case vtkOpenGLGL2PSHelper::Inactive:
        break; // Draw as normal.
    }
  }

  vtkOpenGLClearErrorMacro();

  this->SetTexture(image);
  this->Storage->Texture->Render(this->Renderer);
  int* extent = image->GetExtent();
  float points[] = { p[0], p[1], p[0] + scale * extent[1] + 1.0f, p[1],
    p[0] + scale * extent[1] + 1.0f, p[1] + scale * extent[3] + 1.0f, p[0], p[1],
    p[0] + scale * extent[1] + 1.0f, p[1] + scale * extent[3] + 1.0f, p[0],
    p[1] + scale * extent[3] + 1.0f };

  float texCoord[] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

  vtkOpenGLClearErrorMacro();

  this->ReadyVTBOProgram();
  vtkOpenGLHelper* cbo = this->VTBO;
  if (!cbo->Program)
  {
    return;
  }
  int tunit = vtkOpenGLTexture::SafeDownCast(this->Storage->Texture)->GetTextureUnit();
  cbo->Program->SetUniformi("texture1", tunit);

  this->BuildVBO(cbo, points, 6, nullptr, 0, texCoord);
  this->SetMatrices(cbo->Program);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  this->Storage->Texture->PostRender(this->Renderer);

  vtkOpenGLCheckErrorMacro("failed after DrawImage");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPolyData(
  float p[2], float scale, vtkPolyData* polyData, vtkUnsignedCharArray* colors, int scalarMode)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps)
  {
    switch (gl2ps->GetActiveState())
    {
      case vtkOpenGLGL2PSHelper::Capture:
        // TODO Implement PolyDataGL2PS
        // this->DrawPolyDataGL2PS(pos, image);
        return;
      case vtkOpenGLGL2PSHelper::Background:
        return; // Do nothing.
      case vtkOpenGLGL2PSHelper::Inactive:
        break; // Draw as normal.
    }
  }

  if (SkipDraw())
  {
    return;
  }

  if (polyData->GetLines()->GetNumberOfCells() > 0)
  {
    this->PolyDataImpl->Draw(CellArrayHelper::LINE, polyData, polyData->GetPoints(), p[0], p[1],
      scale, scalarMode, colors);
  }

  if (polyData->GetPolys()->GetNumberOfCells() > 0)
  {
    this->PolyDataImpl->Draw(CellArrayHelper::POLYGON, polyData, polyData->GetPoints(), p[0], p[1],
      scale, scalarMode, colors);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawImage(const vtkRectf& pos, vtkImageData* image)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps)
  {
    switch (gl2ps->GetActiveState())
    {
      case vtkOpenGLGL2PSHelper::Capture:
        this->DrawImageGL2PS(pos, image);
        return;
      case vtkOpenGLGL2PSHelper::Background:
        return; // Do nothing.
      case vtkOpenGLGL2PSHelper::Inactive:
        break; // Draw as normal.
    }
  }

  int tunit = this->RenderWindow->GetTextureUnitManager()->Allocate();
  if (tunit < 0)
  {
    vtkErrorMacro("Hardware does not support the number of textures defined.");
    return;
  }

  this->RenderWindow->GetState()->vtkglActiveTexture(GL_TEXTURE0 + tunit);

  vtkVector2f tex(1.0, 1.0);

  // Call this *after* calling vtkglActiveTexture() to ensure the texture
  // is bound to the correct texture unit.
  GLuint index = this->Storage->TextureFromImage(image, tex);

  float points[] = { pos.GetX(), pos.GetY(), pos.GetX() + pos.GetWidth(), pos.GetY(),
    pos.GetX() + pos.GetWidth(), pos.GetY() + pos.GetHeight(), pos.GetX(), pos.GetY(),
    pos.GetX() + pos.GetWidth(), pos.GetY() + pos.GetHeight(), pos.GetX(),
    pos.GetY() + pos.GetHeight() };

  float texCoord[] = { 0.0f, 0.0f, tex[0], 0.0f, tex[0], tex[1], 0.0f, 0.0f, tex[0], tex[1], 0.0f,
    tex[1] };

  this->ReadyVTBOProgram();
  vtkOpenGLHelper* cbo = this->VTBO;
  if (!cbo->Program)
  {
    return;
  }
  cbo->Program->SetUniformi("texture1", tunit);

  this->BuildVBO(cbo, points, 6, nullptr, 0, texCoord);
  this->SetMatrices(cbo->Program);

  glDrawArrays(GL_TRIANGLES, 0, 6);

  this->RenderWindow->GetTextureUnitManager()->Free(tunit);

  glDeleteTextures(1, &index);

  vtkOpenGLCheckErrorMacro("failed after DrawImage");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetColor4(unsigned char*)
{
  vtkErrorMacro("color cannot be set this way\n");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetColor(unsigned char*)
{
  vtkErrorMacro("color cannot be set this way\n");
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetTexture(vtkImageData* image, int properties)
{
  if (image == nullptr)
  {
    if (this->Storage->Texture)
    {
      this->Storage->Texture->Delete();
      this->Storage->Texture = nullptr;
    }
    return;
  }
  if (this->Storage->Texture == nullptr)
  {
    this->Storage->Texture = vtkTexture::New();
  }
  this->Storage->Texture->SetInputData(image);
  this->Storage->TextureProperties = properties;
  this->Storage->Texture->SetRepeat(properties & vtkContextDevice2D::Repeat);
  this->Storage->Texture->SetInterpolate(properties & vtkContextDevice2D::Linear);
  this->Storage->Texture->EdgeClampOn();
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetPointSize(float size)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
  {
    gl2ps->SetPointSize(size);
  }
  this->RenderWindow->GetState()->vtkglPointSize(size);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetLineWidth(float width)
{
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
  {
    gl2ps->SetLineWidth(width);
  }
  this->RenderWindow->GetState()->vtkglLineWidth(width);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetLineType(int type)
{
  this->LinePattern = 0x0000;
  switch (type)
  {
    case vtkPen::NO_PEN:
      this->LinePattern = 0x0000;
      break;
    case vtkPen::DASH_LINE:
      this->LinePattern = 0x00FF;
      break;
    case vtkPen::DOT_LINE:
      this->LinePattern = 0x0101;
      break;
    case vtkPen::DASH_DOT_LINE:
      this->LinePattern = 0x0C0F;
      break;
    case vtkPen::DASH_DOT_DOT_LINE:
      this->LinePattern = 0x1C47;
      break;
    case vtkPen::DENSE_DOT_LINE:
      this->LinePattern = 0x1111;
      break;
    default:
      this->LinePattern = 0xFFFF;
  }

  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  if (gl2ps && gl2ps->GetActiveState() == vtkOpenGLGL2PSHelper::Capture)
  {
    gl2ps->SetLineStipple(this->LinePattern);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::MultiplyMatrix(vtkMatrix3x3* m)
{
  // We must construct a 4x4 matrix from the 3x3 matrix for OpenGL
  double* M = m->GetData();
  double matrix[16];

  matrix[0] = M[0];
  matrix[1] = M[1];
  matrix[2] = 0.0;
  matrix[3] = M[2];
  matrix[4] = M[3];
  matrix[5] = M[4];
  matrix[6] = 0.0;
  matrix[7] = M[5];
  matrix[8] = 0.0;
  matrix[9] = 0.0;
  matrix[10] = 1.0;
  matrix[11] = 0.0;
  matrix[12] = M[6];
  matrix[13] = M[7];
  matrix[14] = 0.0;
  matrix[15] = M[8];

  this->ModelMatrix->Concatenate(matrix);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetMatrix(vtkMatrix3x3* m)
{
  // We must construct a 4x4 matrix from the 3x3 matrix for OpenGL
  double* M = m->GetData();
  double matrix[16];

  matrix[0] = M[0];
  matrix[1] = M[1];
  matrix[2] = 0.0;
  matrix[3] = M[2];
  matrix[4] = M[3];
  matrix[5] = M[4];
  matrix[6] = 0.0;
  matrix[7] = M[5];
  matrix[8] = 0.0;
  matrix[9] = 0.0;
  matrix[10] = 1.0;
  matrix[11] = 0.0;
  matrix[12] = M[6];
  matrix[13] = M[7];
  matrix[14] = 0.0;
  matrix[15] = M[8];

  this->ModelMatrix->SetMatrix(matrix);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::GetMatrix(vtkMatrix3x3* m)
{
  assert("pre: non_null" && m != nullptr);
  // We must construct a 4x4 matrix from the 3x3 matrix for OpenGL
  double* M = m->GetData();
  double* matrix = this->ModelMatrix->GetMatrix()->Element[0];

  M[0] = matrix[0];
  M[1] = matrix[1];
  M[2] = matrix[3];
  M[3] = matrix[4];
  M[4] = matrix[5];
  M[5] = matrix[7];
  M[6] = matrix[12];
  M[7] = matrix[13];
  M[8] = matrix[15];

  m->Modified();
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::PushMatrix()
{
  this->ModelMatrix->Push();
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::PopMatrix()
{
  this->ModelMatrix->Pop();
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::SetClipping(int* dim)
{
  // If the window is using tile scaling, we need to update the clip coordinates
  // relative to the tile being rendered.
  // (see paraview/paraview#17308)
  double tileViewPort[4];
  this->Renderer->GetVTKWindow()->GetTileViewport(tileViewPort);
  this->Renderer->NormalizedDisplayToDisplay(tileViewPort[0], tileViewPort[1]);
  this->Renderer->NormalizedDisplayToDisplay(tileViewPort[2], tileViewPort[3]);

  vtkRecti tileRect{ vtkContext2D::FloatToInt(tileViewPort[0]),
    vtkContext2D::FloatToInt(tileViewPort[1]), 0, 0 };
  tileRect.AddPoint(
    vtkContext2D::FloatToInt(tileViewPort[2]), vtkContext2D::FloatToInt(tileViewPort[3]));
  // tileRect is the tile being rendered in the current RenderWindow in pixels.

  double viewport[4];
  this->Renderer->GetViewport(viewport);
  this->Renderer->NormalizedDisplayToDisplay(viewport[0], viewport[1]);
  this->Renderer->NormalizedDisplayToDisplay(viewport[2], viewport[3]);
  vtkRecti rendererRect{ vtkContext2D::FloatToInt(viewport[0]),
    vtkContext2D::FloatToInt(viewport[1]), 0, 0 };
  rendererRect.AddPoint(
    vtkContext2D::FloatToInt(viewport[2]), vtkContext2D::FloatToInt(viewport[3]));
  // rendererRect is the viewport in pixels.

  // `dim` is specified as (x,y,width,height) relative to the viewport that this
  // prop is rendering in. So let's fit it in the viewport rect i.e.
  // rendererRect
  vtkRecti clipRect{ dim[0], dim[1], dim[2], dim[3] };
  clipRect.MoveTo(clipRect.GetX() + rendererRect.GetX(), clipRect.GetY() + rendererRect.GetY());
  clipRect.Intersect(rendererRect);

  // Now, clamp the clipRect to the region being shown on the current tile. This
  // generally has no effect since clipRect is wholly contained in tileRect
  // unless tile scaling was being used. In either case, this method will return
  // true as long as the rectangle intersection will produce a valid rectangle.
  if (clipRect.Intersect(tileRect))
  {
    // offset clipRect relative to current tile i.e. window.
    clipRect.MoveTo(clipRect.GetX() - tileRect.GetX(), clipRect.GetY() - tileRect.GetY());
  }
  else
  {
    // clipping region results in empty region, just set to empty.
    clipRect = vtkRecti{ 0, 0, 0, 0 };
  }

  assert(clipRect.GetWidth() >= 0 && clipRect.GetHeight() >= 0);

  this->RenderWindow->GetState()->vtkglScissor(
    clipRect.GetX(), clipRect.GetY(), clipRect.GetWidth(), clipRect.GetHeight());
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::EnableClipping(bool enable)
{
  this->RenderWindow->GetState()->SetEnumState(GL_SCISSOR_TEST, enable);
}

//------------------------------------------------------------------------------
bool vtkOpenGLContextDevice2D::SetStringRendererToFreeType()
{
  // FreeType is the only choice - nothing to do here
  return true;
}

//------------------------------------------------------------------------------
bool vtkOpenGLContextDevice2D::SetStringRendererToQt()
{
  // The Qt based strategy is not available
  return false;
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ReleaseGraphicsResources(vtkWindow* window)
{
  this->VBO->ReleaseGraphicsResources(window);
  this->VCBO->ReleaseGraphicsResources(window);
  this->LinesBO->ReleaseGraphicsResources(window);
  this->LinesCBO->ReleaseGraphicsResources(window);
  this->SBO->ReleaseGraphicsResources(window);
  this->SCBO->ReleaseGraphicsResources(window);
  this->VTBO->ReleaseGraphicsResources(window);
  if (this->Storage->Texture)
  {
    this->Storage->Texture->ReleaseGraphicsResources(window);
  }
  if (this->Storage->SpriteTexture)
  {
    this->Storage->SpriteTexture->ReleaseGraphicsResources(window);
  }
  this->Storage->TextTextureCache.ReleaseGraphicsResources(window);
}

//------------------------------------------------------------------------------
bool vtkOpenGLContextDevice2D::HasGLSL()
{
  return true;
}

//------------------------------------------------------------------------------
vtkImageData* vtkOpenGLContextDevice2D::GetMarker(int shape, int size, bool highlight)
{
  // Generate the cache key for this marker
  vtkTypeUInt64 key = highlight ? (1U << 31) : 0U;
  key |= static_cast<vtkTypeUInt16>(shape);
  key <<= 32;
  key |= static_cast<vtkTypeUInt32>(size);

  // Try to find the marker in the cache
  std::list<vtkMarkerCacheObject>::iterator match =
    std::find(this->MarkerCache.begin(), this->MarkerCache.end(), key);

  // Was it in the cache?
  if (match != this->MarkerCache.end())
  {
    // Yep -- move it to the front and return the data.
    if (match == this->MarkerCache.begin())
    {
      return match->Value;
    }
    else
    {
      vtkMarkerCacheObject result = *match;
      this->MarkerCache.erase(match);
      this->MarkerCache.push_front(result);
      return result.Value;
    }
  }

  // Nope -- we'll need to generate it. Create the image data:
  vtkMarkerCacheObject result;
  result.Key = key;
  result.Value = this->GenerateMarker(shape, size, highlight);

  // If there was an issue generating the marker, just return nullptr.
  if (!result.Value)
  {
    vtkErrorMacro(<< "Error generating marker: shape,size: " << shape << "," << size);
    return nullptr;
  }

  // Check the current cache size.
  while (this->MarkerCache.size() > static_cast<size_t>(this->MaximumMarkerCacheSize - 1) &&
    !this->MarkerCache.empty())
  {
    this->MarkerCache.back().Value->Delete();
    this->MarkerCache.pop_back();
  }

  // Add to the cache
  this->MarkerCache.push_front(result);
  return result.Value;
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ComputeStringBoundsInternal(
  const std::string& string, float bounds[4])
{
  vtkTextRenderer* tren = vtkTextRenderer::GetInstance();
  if (!tren)
  {
    vtkErrorMacro("No text renderer available. Link to vtkRenderingFreeType "
                  "to get the default implementation.");
    return;
  }

  // TODO this currently ignores vtkContextScene::ScaleTiles. Not sure how to
  // get at that from here, but this is better than ignoring scaling altogether.
  // TODO Also, FreeType supports anisotropic DPI. Might be needed if the
  // tileScale isn't homogeneous, but we'll need to update the textrenderer API
  // and see if MPL/mathtext can support it.
  int tileScale[2];
  this->RenderWindow->GetTileScale(tileScale);
  int dpi = this->RenderWindow->GetDPI() * std::max(tileScale[0], tileScale[1]);

  int bbox[4];
  if (!tren->GetBoundingBox(this->TextProp, string, bbox, dpi))
  {
    vtkErrorMacro("Error computing bounding box for string: " << string);
    return;
  }

  // Check for invalid bounding box
  if (bbox[0] >= bbox[1] || bbox[2] >= bbox[3])
  {
    bounds[0] = 0.f;
    bounds[1] = 0.f;
    bounds[2] = 0.f;
    bounds[3] = 0.f;
    return;
  }

  double* mv = this->ModelMatrix->GetMatrix()->Element[0];
  float xScale = mv[0];
  float yScale = mv[5];
  bounds[0] = static_cast<float>(bbox[0]) / xScale;
  bounds[1] = static_cast<float>(bbox[2]) / yScale;
  bounds[2] = static_cast<float>((bbox[1] - bbox[0] + 1) / xScale);
  bounds[3] = static_cast<float>((bbox[3] - bbox[2] + 1) / yScale);
}

//------------------------------------------------------------------------------
vtkImageData* vtkOpenGLContextDevice2D::GenerateMarker(int shape, int width, bool highlight)
{
  // Set up the image data, if highlight then the mark shape is different
  vtkImageData* result = vtkImageData::New();

  result->SetExtent(0, width - 1, 0, width - 1, 0, 0);
  result->AllocateScalars(VTK_UNSIGNED_CHAR, 4);

  unsigned char* image = static_cast<unsigned char*>(result->GetScalarPointer());
  memset(image, 0, width * width * 4);

  // Generate the marker image at the required size
  switch (shape)
  {
    case VTK_MARKER_CROSS:
    {
      int center = (width + 1) / 2;
      for (int i = 0; i < center; ++i)
      {
        int j = width - i - 1;
        memset(image + (4 * (width * i + i)), 255, 4);
        memset(image + (4 * (width * i + j)), 255, 4);
        memset(image + (4 * (width * j + i)), 255, 4);
        memset(image + (4 * (width * j + j)), 255, 4);
        if (highlight)
        {
          memset(image + (4 * (width * (j - 1) + (i))), 255, 4);
          memset(image + (4 * (width * (i + 1) + (i))), 255, 4);
          memset(image + (4 * (width * (i) + (i + 1))), 255, 4);
          memset(image + (4 * (width * (i) + (j - 1))), 255, 4);
          memset(image + (4 * (width * (i + 1) + (j))), 255, 4);
          memset(image + (4 * (width * (j - 1) + (j))), 255, 4);
          memset(image + (4 * (width * (j) + (j - 1))), 255, 4);
          memset(image + (4 * (width * (j) + (i + 1))), 255, 4);
        }
      }
      break;
    }
    default: // Maintaining old behavior, which produces plus for unknown shape
      vtkWarningMacro(<< "Invalid marker shape: " << shape);
      [[fallthrough]];
    case VTK_MARKER_PLUS:
    {
      int center = (width + 1) / 2;
      for (int i = 0; i < center; ++i)
      {
        int j = width - i - 1;
        int c = center - 1;
        memset(image + (4 * (width * c + i)), 255, 4);
        memset(image + (4 * (width * c + j)), 255, 4);
        memset(image + (4 * (width * i + c)), 255, 4);
        memset(image + (4 * (width * j + c)), 255, 4);
        if (highlight)
        {
          memset(image + (4 * (width * (c - 1) + i)), 255, 4);
          memset(image + (4 * (width * (c + 1) + i)), 255, 4);
          memset(image + (4 * (width * (c - 1) + j)), 255, 4);
          memset(image + (4 * (width * (c + 1) + j)), 255, 4);
          memset(image + (4 * (width * i + (c - 1))), 255, 4);
          memset(image + (4 * (width * i + (c + 1))), 255, 4);
          memset(image + (4 * (width * j + (c - 1))), 255, 4);
          memset(image + (4 * (width * j + (c + 1))), 255, 4);
        }
      }
      break;
    }
    case VTK_MARKER_SQUARE:
    {
      memset(image, 255, width * width * 4);
      break;
    }
    case VTK_MARKER_CIRCLE:
    {
      double r = width / 2.0;
      double r2 = r * r;
      for (int i = 0; i < width; ++i)
      {
        double dx2 = (i - r) * (i - r);
        for (int j = 0; j < width; ++j)
        {
          double dy2 = (j - r) * (j - r);
          if ((dx2 + dy2) < r2)
          {
            memset(image + (4 * width * i) + (4 * j), 255, 4);
          }
        }
      }
      break;
    }
    case VTK_MARKER_DIAMOND:
    {
      int r = width / 2;
      for (int i = 0; i < width; ++i)
      {
        int dx = abs(i - r);
        for (int j = 0; j < width; ++j)
        {
          int dy = abs(j - r);
          if (r - dx >= dy)
          {
            memset(image + (4 * width * i) + (4 * j), 255, 4);
          }
        }
      }
      break;
    }
  }
  return result;
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Renderer: ";
  if (this->Renderer)
  {
    os << endl;
    this->Renderer->PrintSelf(os, indent.GetNextIndent());
  }
  else
  {
    os << "(none)" << endl;
  }
  os << indent << "MaximumMarkerCacheSize: " << this->MaximumMarkerCacheSize << endl;
  os << indent << "MarkerCache: " << this->MarkerCache.size() << " entries." << endl;
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawMarkersGL2PS(
  int shape, bool highlight, float* points, int n, unsigned char* colors, int nc_comps)
{
  switch (shape)
  {
    case VTK_MARKER_CROSS:
      this->DrawCrossMarkersGL2PS(highlight, points, n, colors, nc_comps);
      break;
    default:
      // default is here for consistency with old impl -- defaults to plus for
      // unrecognized shapes.
    case VTK_MARKER_PLUS:
      this->DrawPlusMarkersGL2PS(highlight, points, n, colors, nc_comps);
      break;
    case VTK_MARKER_SQUARE:
      this->DrawSquareMarkersGL2PS(highlight, points, n, colors, nc_comps);
      break;
    case VTK_MARKER_CIRCLE:
      this->DrawCircleMarkersGL2PS(highlight, points, n, colors, nc_comps);
      break;
    case VTK_MARKER_DIAMOND:
      this->DrawDiamondMarkersGL2PS(highlight, points, n, colors, nc_comps);
      break;
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawCrossMarkersGL2PS(
  bool highlight, float* points, int n, unsigned char* colors, int nc_comps)
{
  float oldWidth = this->Pen->GetWidth();
  unsigned char oldColor[4];
  this->Pen->GetColor(oldColor);
  int oldLineType = this->Pen->GetLineType();

  float halfWidth = oldWidth * 0.5f;
  float deltaX = halfWidth;
  float deltaY = halfWidth;

  this->TransformSize(deltaX, deltaY);

  if (highlight)
  {
    this->Pen->SetWidth(1.5);
  }
  else
  {
    this->Pen->SetWidth(0.5);
  }
  this->Pen->SetLineType(vtkPen::SOLID_LINE);

  float curLine[4];
  unsigned char color[4];
  for (int i = 0; i < n; ++i)
  {
    float* point = points + (i * 2);
    if (colors)
    {
      color[3] = 255;
      switch (nc_comps)
      {
        case 4:
        case 3:
          memcpy(color, colors + (i * nc_comps), nc_comps);
          break;
        case 2:
          color[3] = colors[i * nc_comps + 1];
          [[fallthrough]];
        case 1:
          memset(color, colors[i * nc_comps], 3);
          break;
        default:
          vtkErrorMacro(<< "Invalid number of color components: " << nc_comps);
          break;
      }

      this->Pen->SetColor(color);
    }

    // The first line of the cross:
    curLine[0] = point[0] + deltaX;
    curLine[1] = point[1] + deltaY;
    curLine[2] = point[0] - deltaX;
    curLine[3] = point[1] - deltaY;
    this->DrawPoly(curLine, 2);

    // And the second:
    curLine[0] = point[0] + deltaX;
    curLine[1] = point[1] - deltaY;
    curLine[2] = point[0] - deltaX;
    curLine[3] = point[1] + deltaY;
    this->DrawPoly(curLine, 2);
  }

  this->Pen->SetWidth(oldWidth);
  this->Pen->SetColor(oldColor);
  this->Pen->SetLineType(oldLineType);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawPlusMarkersGL2PS(
  bool highlight, float* points, int n, unsigned char* colors, int nc_comps)
{
  float oldWidth = this->Pen->GetWidth();
  unsigned char oldColor[4];
  this->Pen->GetColor(oldColor);
  int oldLineType = this->Pen->GetLineType();

  float halfWidth = oldWidth * 0.5f;
  float deltaX = halfWidth;
  float deltaY = halfWidth;

  this->TransformSize(deltaX, deltaY);

  if (highlight)
  {
    this->Pen->SetWidth(1.5);
  }
  else
  {
    this->Pen->SetWidth(0.5);
  }
  this->Pen->SetLineType(vtkPen::SOLID_LINE);

  float curLine[4];
  unsigned char color[4];
  for (int i = 0; i < n; ++i)
  {
    float* point = points + (i * 2);
    if (colors)
    {
      color[3] = 255;
      switch (nc_comps)
      {
        case 4:
        case 3:
          memcpy(color, colors + (i * nc_comps), nc_comps);
          break;
        case 2:
          color[3] = colors[i * nc_comps + 1];
          [[fallthrough]];
        case 1:
          memset(color, colors[i * nc_comps], 3);
          break;
        default:
          vtkErrorMacro(<< "Invalid number of color components: " << nc_comps);
          break;
      }

      this->Pen->SetColor(color);
    }

    // The first line of the plus:
    curLine[0] = point[0] - deltaX;
    curLine[1] = point[1];
    curLine[2] = point[0] + deltaX;
    curLine[3] = point[1];
    this->DrawPoly(curLine, 2);

    // And the second:
    curLine[0] = point[0];
    curLine[1] = point[1] - deltaY;
    curLine[2] = point[0];
    curLine[3] = point[1] + deltaY;
    this->DrawPoly(curLine, 2);
  }

  this->Pen->SetWidth(oldWidth);
  this->Pen->SetColor(oldColor);
  this->Pen->SetLineType(oldLineType);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawSquareMarkersGL2PS(
  bool /*highlight*/, float* points, int n, unsigned char* colors, int nc_comps)
{
  unsigned char oldColor[4];
  this->Brush->GetColor(oldColor);

  this->Brush->SetColor(this->Pen->GetColor());

  float halfWidth = this->GetPen()->GetWidth() * 0.5f;
  float deltaX = halfWidth;
  float deltaY = halfWidth;

  this->TransformSize(deltaX, deltaY);

  float quad[8];
  unsigned char color[4];
  for (int i = 0; i < n; ++i)
  {
    float* point = points + (i * 2);
    if (colors)
    {
      color[3] = 255;
      switch (nc_comps)
      {
        case 4:
        case 3:
          memcpy(color, colors + (i * nc_comps), nc_comps);
          break;
        case 2:
          color[3] = colors[i * nc_comps + 1];
          [[fallthrough]];
        case 1:
          memset(color, colors[i * nc_comps], 3);
          break;
        default:
          vtkErrorMacro(<< "Invalid number of color components: " << nc_comps);
          break;
      }

      this->Brush->SetColor(color);
    }

    quad[0] = point[0] - deltaX;
    quad[1] = point[1] - deltaY;
    quad[2] = point[0] + deltaX;
    quad[3] = quad[1];
    quad[4] = quad[2];
    quad[5] = point[1] + deltaY;
    quad[6] = quad[0];
    quad[7] = quad[5];

    this->DrawQuad(quad, 4);
  }

  this->Brush->SetColor(oldColor);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawCircleMarkersGL2PS(
  bool /*highlight*/, float* points, int n, unsigned char* colors, int nc_comps)
{
  float radius = this->GetPen()->GetWidth() * 0.475;

  unsigned char oldColor[4];
  this->Brush->GetColor(oldColor);

  this->Brush->SetColor(this->Pen->GetColor());

  unsigned char color[4];
  for (int i = 0; i < n; ++i)
  {
    float* point = points + (i * 2);
    if (colors)
    {
      color[3] = 255;
      switch (nc_comps)
      {
        case 4:
        case 3:
          memcpy(color, colors + (i * nc_comps), nc_comps);
          break;
        case 2:
          color[3] = colors[i * nc_comps + 1];
          [[fallthrough]];
        case 1:
          memset(color, colors[i * nc_comps], 3);
          break;
        default:
          vtkErrorMacro(<< "Invalid number of color components: " << nc_comps);
          break;
      }

      this->Brush->SetColor(color);
    }

    this->DrawEllipseWedge(point[0], point[1], radius, radius, 0, 0, 0, 360);
  }

  this->Brush->SetColor(oldColor);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawDiamondMarkersGL2PS(
  bool /*highlight*/, float* points, int n, unsigned char* colors, int nc_comps)
{
  unsigned char oldColor[4];
  this->Brush->GetColor(oldColor);

  this->Brush->SetColor(this->Pen->GetColor());

  float halfWidth = this->GetPen()->GetWidth() * 0.5f;
  float deltaX = halfWidth;
  float deltaY = halfWidth;

  this->TransformSize(deltaX, deltaY);

  float quad[8];
  unsigned char color[4];
  for (int i = 0; i < n; ++i)
  {
    float* point = points + (i * 2);
    if (colors)
    {
      color[3] = 255;
      switch (nc_comps)
      {
        case 4:
        case 3:
          memcpy(color, colors + (i * nc_comps), nc_comps);
          break;
        case 2:
          color[3] = colors[i * nc_comps + 1];
          [[fallthrough]];
        case 1:
          memset(color, colors[i * nc_comps], 3);
          break;
        default:
          vtkErrorMacro(<< "Invalid number of color components: " << nc_comps);
          break;
      }

      this->Brush->SetColor(color);
    }

    quad[0] = point[0] - deltaX;
    quad[1] = point[1];
    quad[2] = point[0];
    quad[3] = point[1] - deltaY;
    quad[4] = point[0] + deltaX;
    quad[5] = point[1];
    quad[6] = point[0];
    quad[7] = point[1] + deltaY;

    this->DrawQuad(quad, 4);
  }

  this->Brush->SetColor(oldColor);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawImageGL2PS(float p[2], vtkImageData* input)
{
  // Must be unsigned char -- otherwise OpenGL rendering behaves badly anyway.
  if (!vtkDataTypesCompare(input->GetScalarType(), VTK_UNSIGNED_CHAR))
  {
    vtkErrorMacro("Invalid image format: Expected unsigned char scalars.");
    return;
  }

  // Convert to float for GL2PS
  vtkNew<vtkImageData> image;
  image->ShallowCopy(input);
  vtkDataArray* s = image->GetPointData()->GetScalars();
  size_t numVals = (s->GetNumberOfComponents() * s->GetNumberOfTuples());
  unsigned char* vals = static_cast<unsigned char*>(s->GetVoidPointer(0));
  vtkNew<vtkFloatArray> scalars;
  scalars->SetNumberOfComponents(s->GetNumberOfComponents());
  scalars->SetNumberOfTuples(s->GetNumberOfTuples());
  for (size_t i = 0; i < numVals; ++i)
  {
    scalars->SetValue(static_cast<vtkIdType>(i), vals[i] / 255.f);
  }
  image->GetPointData()->SetScalars(scalars);

  // Instance always exists when this method is called:
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();

  float tp[2] = { p[0], p[1] };
  this->TransformPoint(tp[0], tp[1]);
  double pos[3] = { static_cast<double>(tp[0]), static_cast<double>(tp[1]), 0. };
  gl2ps->DrawImage(image, pos);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawImageGL2PS(float p[2], float scale, vtkImageData* image)
{
  if (std::fabs(scale - 1.f) < 1e-5f)
  {
    this->DrawImageGL2PS(p, image);
    return;
  }

  int dims[3];
  image->GetDimensions(dims);
  vtkRectf rect(p[0], p[1], dims[0] * scale, dims[1] * scale);
  this->DrawImageGL2PS(rect, image);
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawImageGL2PS(const vtkRectf& rect, vtkImageData* image)
{
  int dims[3];
  image->GetDimensions(dims);
  int width = static_cast<int>(std::round(rect.GetWidth()));
  int height = static_cast<int>(std::round(rect.GetHeight()));
  if (width == dims[0] && height == dims[1])
  {
    this->DrawImageGL2PS(rect.GetBottomLeft().GetData(), image);
    return;
  }

  vtkNew<vtkImageResize> resize;
  resize->SetInputData(image);
  resize->SetResizeMethod(vtkImageResize::OUTPUT_DIMENSIONS);
  resize->SetOutputDimensions(width, height, -1);
  resize->Update();
  this->DrawImageGL2PS(rect.GetBottomLeft().GetData(), resize->GetOutput());
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawCircleGL2PS(float x, float y, float rX, float rY)
{
  if (this->Brush->GetColorObject().GetAlpha() == 0)
  {
    return;
  }

  // We know this is valid if this method has been called:
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();

  vtkNew<vtkPath> path;
  this->AddEllipseToPath(path, 0.f, 0.f, rX, rY, false);
  this->TransformPath(path);

  double origin[3] = { x, y, 0.f };

  // Fill
  unsigned char fillColor[4];
  this->Brush->GetColor(fillColor);

  std::stringstream label;
  label << "vtkOpenGLContextDevice2D::DrawCircleGL2PS(" << x << ", " << y << ", " << rX << ", "
        << rY << ") fill:";

  gl2ps->DrawPath(path, origin, origin, fillColor, nullptr, 0.0, -1.f, label.str().c_str());

  // and stroke
  unsigned char strokeColor[4];
  this->Pen->GetColor(strokeColor);
  float strokeWidth = this->Pen->GetWidth();

  label.str("");
  label.clear();
  label << "vtkOpenGLContextDevice2D::DrawCircleGL2PS(" << x << ", " << y << ", " << rX << ", "
        << rY << ") stroke:";
  gl2ps->DrawPath(
    path, origin, origin, strokeColor, nullptr, 0.0, strokeWidth, label.str().c_str());
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::DrawWedgeGL2PS(
  float x, float y, float outRx, float outRy, float inRx, float inRy)
{
  if (this->Brush->GetColorObject().GetAlpha() == 0)
  {
    return;
  }

  vtkNew<vtkPath> path;
  this->AddEllipseToPath(path, 0.f, 0.f, outRx, outRy, false);
  this->AddEllipseToPath(path, 0.f, 0.f, inRx, inRy, true);

  std::stringstream label;
  label << "vtkOpenGLGL2PSContextDevice2D::DrawWedgeGL2PS(" << x << ", " << y << ", " << outRx
        << ", " << outRy << ", " << inRx << ", " << inRy << ") path:";

  unsigned char color[4];
  this->Brush->GetColor(color);

  double rasterPos[3] = { static_cast<double>(x), static_cast<double>(y), 0. };

  this->TransformPoint(x, y);
  double windowPos[3] = { static_cast<double>(x), static_cast<double>(y), 0. };

  // We know the helper exists and that we are capturing if this function has
  // been called.
  vtkOpenGLGL2PSHelper* gl2ps = vtkOpenGLGL2PSHelper::GetInstance();
  gl2ps->DrawPath(path, rasterPos, windowPos, color, nullptr, 0.0, -1.f, label.str().c_str());
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::AddEllipseToPath(
  vtkPath* path, float x, float y, float rx, float ry, bool reverse)
{
  if (rx < 1e-5 || ry < 1e-5)
  {
    return;
  }

  // method based on http://www.tinaja.com/glib/ellipse4.pdf
  static const float MAGIC = (4.f / 3.f) * (sqrt(2.f) - 1.f);

  if (!reverse)
  {
    path->InsertNextPoint(x - rx, y, 0, vtkPath::MOVE_TO);
    path->InsertNextPoint(x - rx, ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(-rx * MAGIC, y + ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x, y + ry, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(rx * MAGIC, y + ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x + rx, ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x + rx, y, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(x + rx, -ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(rx * MAGIC, y - ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x, y - ry, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(-rx * MAGIC, y - ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x - rx, -ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x - rx, y, 0, vtkPath::CUBIC_CURVE);
  }
  else
  {
    path->InsertNextPoint(x - rx, y, 0, vtkPath::MOVE_TO);
    path->InsertNextPoint(x - rx, -ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(-rx * MAGIC, y - ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x, y - ry, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(rx * MAGIC, y - ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x + rx, -ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x + rx, y, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(x + rx, ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(rx * MAGIC, y + ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x, y + ry, 0, vtkPath::CUBIC_CURVE);

    path->InsertNextPoint(-rx * MAGIC, y + ry, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x - rx, ry * MAGIC, 0, vtkPath::CUBIC_CURVE);
    path->InsertNextPoint(x - rx, y, 0, vtkPath::CUBIC_CURVE);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::TransformPath(vtkPath* path) const
{
  // Transform the path with the modelview matrix:
  double modelview[16];
  vtkMatrix4x4::DeepCopy(modelview, this->ModelMatrix->GetMatrix());

  // Transform the 2D path.
  float newPoint[3] = { 0, 0, 0 };
  vtkPoints* points = path->GetPoints();
  for (vtkIdType i = 0; i < path->GetNumberOfPoints(); ++i)
  {
    double* point = points->GetPoint(i);
    newPoint[0] = modelview[0] * point[0] + modelview[1] * point[1] + modelview[3];
    newPoint[1] = modelview[4] * point[0] + modelview[5] * point[1] + modelview[7];
    points->SetPoint(i, newPoint);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::TransformPoint(float& x, float& y) const
{
  double modelview[16];
  vtkMatrix4x4::DeepCopy(modelview, this->ModelMatrix->GetMatrix());

  float inX = x;
  float inY = y;
  x = modelview[0] * inX + modelview[1] * inY + modelview[3];
  y = modelview[4] * inX + modelview[5] * inY + modelview[7];
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::TransformSize(float& dx, float& dy) const
{
  double modelview[16];
  vtkMatrix4x4::DeepCopy(modelview, this->ModelMatrix->GetMatrix());

  dx /= modelview[0];
  dy /= modelview[5];
}

//------------------------------------------------------------------------------
void vtkOpenGLContextDevice2D::ReleaseCache(std::uintptr_t cacheIdentifier)
{
  this->Storage->BufferObjectBuilder.Erase(cacheIdentifier, this->RenderWindow);
}
VTK_ABI_NAMESPACE_END
