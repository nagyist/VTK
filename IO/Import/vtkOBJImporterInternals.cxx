// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkOBJImporterInternals.h"
#include "vtkImageReader2.h"
#include "vtkImageReader2Factory.h"
#include "vtkOBJImporter.h"
#include "vtkPolyDataMapper.h"
#include "vtkProperty.h"
#include "vtkRenderWindow.h"
#include "vtkRenderer.h"
#include "vtkSmartPointer.h"
#include "vtkTexture.h"
#include "vtkTransform.h"
#include "vtksys/FStream.hxx"
#include "vtksys/SystemTools.hxx"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>

#if defined(_WIN32)
#pragma warning(disable : 4800)
#endif

VTK_ABI_NAMESPACE_BEGIN
namespace
{
int localVerbosity = 0;
}

void obj_set_material_defaults(vtkOBJImportedMaterial* mtl)
{
  mtl->amb[0] = 0.0;
  mtl->amb[1] = 0.0;
  mtl->amb[2] = 0.0;
  mtl->diff[0] = 1.0;
  mtl->diff[1] = 1.0;
  mtl->diff[2] = 1.0;
  mtl->spec[0] = 0.0;
  mtl->spec[1] = 0.0;
  mtl->spec[2] = 0.0;
  mtl->map_Kd_scale[0] = 1.0;
  mtl->map_Kd_scale[1] = 1.0;
  mtl->map_Kd_scale[2] = 1.0;
  mtl->illum = 2;
  mtl->reflect = 0.0;
  mtl->trans = 1;
  mtl->glossy = 98;
  mtl->specularPower = 0;
  mtl->refract_index = 1;
  mtl->texture_filename[0] = '\0';

  if (localVerbosity > 0)
  {
    vtkGenericWarningMacro("Created a default vtkOBJImportedMaterial, texture filename is "
      << std::string(mtl->texture_filename));
  }
}

// check if the texture file referenced exists
// some files references png when they ship with jpg
// so check for that as well
void checkTextureMapFile(vtkOBJImportedMaterial* current_mtl, std::string& texturePath)
{
  // try texture as specified
  bool bFileExistsNoPath = vtksys::SystemTools::FileExists(current_mtl->texture_filename);
  std::vector<std::string> path_and_file(2);
  path_and_file[0] = texturePath;
  path_and_file[1] = std::string(current_mtl->texture_filename);
  std::string joined = vtksys::SystemTools::JoinPath(path_and_file);
  bool bFileExistsInPath = vtksys::SystemTools::FileExists(joined);
  // if the file does not exist and it has a png extension try for jpg instead
  if (!(bFileExistsNoPath || bFileExistsInPath))
  {
    if (vtksys::SystemTools::GetFilenameLastExtension(current_mtl->texture_filename) == ".png")
    {
      // try jpg
      std::string jpgName =
        vtksys::SystemTools::GetFilenameWithoutLastExtension(current_mtl->texture_filename) +
        ".jpg";
      bFileExistsNoPath = vtksys::SystemTools::FileExists(jpgName);
      path_and_file[0] = texturePath;
      path_and_file[1] = jpgName;
      joined = vtksys::SystemTools::JoinPath(path_and_file);
      bFileExistsInPath = vtksys::SystemTools::FileExists(joined);
      if (bFileExistsInPath || bFileExistsNoPath)
      {
        current_mtl->texture_filename = jpgName;
      }
    }
    if (!(bFileExistsNoPath || bFileExistsInPath))
    {
      vtkGenericWarningMacro(<< "mtl file " << current_mtl->name
                             << " requests texture file that appears not to exist: "
                             << current_mtl->texture_filename << "; texture path: " << texturePath
                             << "\n");
    }
  }
}

namespace
{

class Token
{
public:
  enum TokenType
  {
    Number = 1,
    String,
    Space,
    LineEnd
  };

  TokenType Type;
  double NumberValue = 0.0;
  std::string StringValue;
};

bool tokenGetString(size_t& t, std::vector<Token>& tokens, std::string& result)
{
  // must have two more tokens and the next token must be a space
  if (tokens.size() <= t + 2 || tokens[t + 1].Type != Token::Space ||
    tokens[t + 2].Type != Token::String)
  {
    vtkGenericWarningMacro("bad syntax");
    return false;
  }
  result = tokens[t + 2].StringValue;
  t += 2;
  return true;
}

bool tokenGetNumber(size_t& t, std::vector<Token>& tokens, double& result)
{
  // must have two more tokens and the next token must be a space
  if (tokens.size() <= t + 2 || tokens[t + 1].Type != Token::Space ||
    tokens[t + 2].Type != Token::Number)
  {
    vtkGenericWarningMacro("bad syntax");
    return false;
  }
  result = tokens[t + 2].NumberValue;
  t += 2;
  return true;
}

bool tokenGetVector(
  size_t& t, std::vector<Token>& tokens, double* result, size_t resultSize, size_t minNums)
{
  // must have two more tokens and the next token must be a space
  if (tokens.size() <= t + 2 * minNums)
  {
    vtkGenericWarningMacro("bad syntax");
    return false;
  }
  // parse the following numbers
  size_t count = 0;
  while (tokens.size() > t + 2 && tokens[t + 1].Type == Token::Space &&
    tokens[t + 2].Type == Token::Number)
  {
    result[count] = tokens[t + 2].NumberValue;
    t += 2;
    count++;
  }

  // if any values provided then copy the first value to any missing values
  if (count)
  {
    for (size_t i = count; i < resultSize; ++i)
    {
      result[i] = result[count - 1];
    }
  }

  return true;
}

bool tokenGetTexture(size_t& t, std::vector<Token>& tokens, vtkOBJImportedMaterial* current_mtl,
  std::string& texturePath)
{
  // parse the next tokens looking for
  // texture options must all be on one line
  current_mtl->texture_filename = "";
  for (size_t tt = t + 1; tt < tokens.size(); ++tt)
  {
    if (tokens[tt].Type == Token::Space)
    {
      continue;
    }
    if (tokens[tt].Type == Token::LineEnd)
    {
      t = tt;
      return false;
    }

    // string value
    if (tokens[tt].StringValue == "-s")
    {
      tokenGetVector(tt, tokens, current_mtl->map_Kd_scale, 3, 1);
      continue;
    }
    if (tokens[tt].StringValue == "-o")
    {
      tokenGetVector(tt, tokens, current_mtl->map_Kd_offset, 3, 1);
      continue;
    }
    if (tokens[tt].StringValue == "-mm")
    {
      double tmp[2];
      tokenGetVector(tt, tokens, tmp, 2, 1);
      continue;
    }

    // if we got here then must be name of texture file
    // or an unknown option, we combine all tokens
    // form this point forward as they may be a filename
    // with spaces in them
    current_mtl->texture_filename += tokens[tt].StringValue;
    ++tt;
    while (tt < tokens.size() && tokens[tt].Type != Token::LineEnd)
    {
      current_mtl->texture_filename += tokens[tt].StringValue;
      ++tt;
    }
    checkTextureMapFile(current_mtl, texturePath);
    t = tt;
    return true;
  }

  return false;
}
}

VTK_ABI_NAMESPACE_END

#include "mtlsyntax.inl"

VTK_ABI_NAMESPACE_BEGIN
std::vector<vtkOBJImportedMaterial*> vtkOBJPolyDataProcessor::ParseOBJandMTL(
  std::string Filename, int& result_code)
{
  std::vector<vtkOBJImportedMaterial*> listOfMaterials;
  result_code = 0;

  if (Filename.empty())
  {
    return listOfMaterials;
  }

  vtksys::ifstream in(Filename.c_str(), std::ios::in | std::ios::binary);
  if (!in)
  {
    return listOfMaterials;
  }

  std::vector<Token> tokens;
  std::string contents;
  in.seekg(0, std::ios::end);
  contents.resize(in.tellg());
  in.seekg(0, std::ios::beg);
  in.read(contents.data(), contents.size());
  in.close();

  // watch for UTF-8 BOM
  if (std::string_view(contents.data(), 3) == "\xef\xbb\xbf")
  {
    result_code = parseMTL(contents.c_str() + 3, tokens);
  }
  else
  {
    result_code = parseMTL(contents.c_str(), tokens);
  }

  // now handle the token stream
  vtkOBJImportedMaterial* current_mtl = nullptr;
  for (size_t t = 0; t < tokens.size(); ++t)
  {
    if (tokens[t].Type == Token::Number)
    {
      vtkErrorMacro("Number found outside of a command or option on token# "
        << t << " with number " << tokens[t].NumberValue);
      break;
    }
    if (tokens[t].Type == Token::Space || tokens[t].Type == Token::LineEnd)
    {
      continue;
    }

    // string value
    std::string lcstr = tokens[t].StringValue;
    std::transform(lcstr.begin(), lcstr.end(), lcstr.begin(), ::tolower);
    if (tokens[t].StringValue == "newmtl")
    {
      current_mtl = (new vtkOBJImportedMaterial);
      listOfMaterials.push_back(current_mtl);
      obj_set_material_defaults(current_mtl);
      tokenGetString(t, tokens, current_mtl->name);
      continue;
    }
    if (tokens[t].StringValue == "Ka")
    {
      tokenGetVector(t, tokens, current_mtl->amb, 3, 1);
      continue;
    }
    if (tokens[t].StringValue == "Kd")
    {
      tokenGetVector(t, tokens, current_mtl->diff, 3, 1);
      continue;
    }
    if (tokens[t].StringValue == "Ks")
    {
      tokenGetVector(t, tokens, current_mtl->spec, 3, 1);
      continue;
    }
    if (tokens[t].StringValue == "Ns")
    {
      tokenGetNumber(t, tokens, current_mtl->specularPower);
      continue;
    }
    if (tokens[t].StringValue == "d")
    {
      tokenGetNumber(t, tokens, current_mtl->trans);
      continue;
    }
    if (tokens[t].StringValue == "illum")
    {
      double tmp;
      if (tokenGetNumber(t, tokens, tmp))
      {
        current_mtl->illum = static_cast<int>(tmp);
      }
      continue;
    }
    if (lcstr == "map_ka" || lcstr == "map_kd")
    {
      tokenGetTexture(t, tokens, current_mtl, this->TexturePath);
      continue;
    }

    // vtkErrorMacro("Unknown command in mtl file at token# " <<
    //   t << " and value " << tokens[t].StringValue);
    // consume to the end of the line
    while (t < tokens.size() && tokens[t].Type != Token::LineEnd)
    {
      ++t;
    }
  }

  return listOfMaterials;
}

bool bindTexturedPolydataToRenderWindow(vtkRenderWindow* renderWindow, vtkRenderer* renderer,
  vtkOBJPolyDataProcessor* reader, vtkActorCollection* actorCollection)
{
  if (nullptr == (renderWindow))
  {
    vtkErrorWithObjectMacro(reader, "RenderWindow is null, failure!");
    return false;
  }
  if (nullptr == (renderer))
  {
    vtkErrorWithObjectMacro(reader, "Renderer is null, failure!");
    return false;
  }
  if (nullptr == (reader))
  {
    vtkErrorWithObjectMacro(reader, "vtkOBJPolyDataProcessor is null, failure!");
    return false;
  }

  reader->actor_list.clear();
  reader->actor_list.reserve(reader->GetNumberOfOutputPorts());
  actorCollection->RemoveAllItems();

  // keep track of textures used and if multiple parts use the same
  // texture, then have the actors use the same texture. This saves memory
  // etc and makes exporting more efficient.
  std::map<std::string, vtkSmartPointer<vtkTexture>> knownTextures;

  for (int port_idx = 0; port_idx < reader->GetNumberOfOutputPorts(); port_idx++)
  {
    vtkPolyData* objPoly = reader->GetOutput(port_idx);

    vtkSmartPointer<vtkPolyDataMapper> mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputData(objPoly);
    mapper->SetColorModeToDirectScalars();

    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);

    vtkDebugWithObjectMacro(reader,
      "Grabbed objPoly " << objPoly << ", port index " << port_idx << "\n"
                         << "numPolys = " << objPoly->GetNumberOfPolys()
                         << " numPoints = " << objPoly->GetNumberOfPoints());

    // For each named material, load and bind the texture, add it to the renderer

    std::string textureFilename = reader->GetTextureFilename(port_idx);
    if (!textureFilename.empty())
    {
      auto kti = knownTextures.find(textureFilename);
      if (kti == knownTextures.end())
      {
        vtkSmartPointer<vtkImageReader2> imgReader;
        imgReader.TakeReference(
          vtkImageReader2Factory::CreateImageReader2(textureFilename.c_str()));

        if (!imgReader)
        {
          vtkErrorWithObjectMacro(
            reader, "Cannot instantiate image reader for texture: " << textureFilename);
        }
        else
        {
          imgReader->SetFileName(textureFilename.c_str());

          vtkSmartPointer<vtkTexture> vTexture = vtkSmartPointer<vtkTexture>::New();
          vTexture->SetInputConnection(imgReader->GetOutputPort());
          actor->SetTexture(vTexture);
          knownTextures[textureFilename] = vTexture;
        }
      }
      else // this is a texture we already have seen
      {
        actor->SetTexture(kti->second);
      }
    }

    vtkSmartPointer<vtkProperty> properties = vtkSmartPointer<vtkProperty>::New();

    vtkOBJImportedMaterial* raw_mtl_data = reader->GetMaterial(port_idx);
    if (raw_mtl_data)
    {
      // handle texture coordinate transforms
      if (actor->GetTexture() &&
        (raw_mtl_data->map_Kd_scale[0] != 1 || raw_mtl_data->map_Kd_scale[1] != 1 ||
          raw_mtl_data->map_Kd_scale[2] != 1))
      {
        vtkNew<vtkTransform> tf;
        tf->Scale(raw_mtl_data->map_Kd_scale[0], raw_mtl_data->map_Kd_scale[1],
          raw_mtl_data->map_Kd_scale[2]);
        actor->GetTexture()->SetTransform(tf);
      }

      // When the material is created from a MTL file,
      // the name is different than the default "x" name.
      // in this case, we disable vertex coloring
      if (raw_mtl_data->name != "x")
      {
        mapper->ScalarVisibilityOff();
      }

      properties->SetDiffuseColor(raw_mtl_data->diff);
      properties->SetSpecularColor(raw_mtl_data->spec);
      properties->SetAmbientColor(raw_mtl_data->amb);
      properties->SetOpacity(raw_mtl_data->trans);
      properties->SetInterpolationToPhong();
      switch (raw_mtl_data->illum)
      {
        case 0:
          properties->SetLighting(false);
          properties->SetDiffuse(0);
          properties->SetSpecular(0);
          properties->SetAmbient(1.0);
          properties->SetColor(properties->GetDiffuseColor());
          break;
        case 1:
          properties->SetDiffuse(1.0);
          properties->SetSpecular(0);
          properties->SetAmbient(1.0);
          break;
        default:
        case 2:
          properties->SetDiffuse(1.0);
          properties->SetSpecular(1.0);
          properties->SetAmbient(1.0);
          // blinn to phong ~= 4.0
          properties->SetSpecularPower(raw_mtl_data->specularPower / 4.0);
          break;
      }
      actor->SetProperty(properties);
    }
    renderer->AddActor(actor);
    actorCollection->AddItem(actor);

    // properties->ShadingOn(); // use ShadingOn() if loading vtkMaterial from xml
    // available in mtl parser are:
    //    double amb[3];
    //    double diff[3];
    //    double spec[3];
    //    double reflect;
    //    double refract;
    //    double trans;
    //    double shiny;
    //    double glossy;
    //    double refract_index;

    reader->actor_list.push_back(actor); // keep a handle on actors to animate later
  }
  return true;
  /** post-condition of this function: the renderer has had a bunch of actors added to it */
}

vtkOBJImportedMaterial::vtkOBJImportedMaterial()
{
  this->name = "x";
  obj_set_material_defaults(this);
}
VTK_ABI_NAMESPACE_END
