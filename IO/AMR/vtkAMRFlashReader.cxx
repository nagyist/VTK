// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkAMRFlashReader.h"
#include "vtkAMRBox.h"
#include "vtkByteSwap.h"
#include "vtkDataArraySelection.h"
#include "vtkObjectFactory.h"
#include "vtkOverlappingAMR.h"
#include "vtkUniformGrid.h"

#include "vtkCellData.h"
#include "vtkDataSet.h"
#include "vtkDoubleArray.h"
#include "vtkFloatArray.h"
#include "vtkIntArray.h"
#include "vtkLongArray.h"
#include "vtkLongLongArray.h"
#include "vtkShortArray.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnsignedIntArray.h"
#include "vtkUnsignedShortArray.h"

#include <cassert>
#include <cfloat>
#include <map>
#include <vector>
#define H5_USE_16_API
#include "vtk_hdf5.h"

#include "vtkAMRFlashReaderInternal.h"

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkAMRFlashReader);

//------------------------------------------------------------------------------
vtkAMRFlashReader::vtkAMRFlashReader()
{
  this->IsReady = false;
  this->Internal = new vtkFlashReaderInternal;
  this->Initialize();
}

//------------------------------------------------------------------------------
vtkAMRFlashReader::~vtkAMRFlashReader()
{
  delete this->Internal;
  this->Internal = nullptr;
}

//------------------------------------------------------------------------------
void vtkAMRFlashReader::PrintSelf(std::ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//------------------------------------------------------------------------------
void vtkAMRFlashReader::SetFileName(const char* fileName)
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));

  if (fileName && strcmp(fileName, "") != 0 &&
    ((this->FileName == nullptr) || strcmp(fileName, this->FileName) != 0))
  {
    if (this->FileName)
    {
      delete[] this->FileName;
      this->FileName = nullptr;
      this->Internal->SetFileName(nullptr);
    }

    this->FileName = new char[strlen(fileName) + 1];
    strcpy(this->FileName, fileName);
    this->FileName[strlen(fileName)] = '\0';

    this->IsReady = true;
    this->Internal->SetFileName(this->FileName);
    this->LoadedMetaData = false;

    this->SetUpDataArraySelections();
    this->InitializeArraySelections();
  }

  this->Modified();
}

//------------------------------------------------------------------------------
void vtkAMRFlashReader::ReadMetaData()
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  this->Internal->ReadMetaData();
}

//------------------------------------------------------------------------------
int vtkAMRFlashReader::GetBlockLevel(int blockIdx)
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  if (!this->IsReady)
  {
    return (-1);
  }

  this->Internal->ReadMetaData();
  if (blockIdx < 0 || blockIdx >= this->Internal->NumberOfBlocks)
  {
    vtkErrorMacro("Block Index (" << blockIdx << ") is out-of-bounds!");
    return (-1);
  }
  return (this->Internal->Blocks[blockIdx].Level - 1);
}

//------------------------------------------------------------------------------
int vtkAMRFlashReader::GetNumberOfBlocks()
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  if (!this->IsReady)
  {
    return 0;
  }

  this->Internal->ReadMetaData();
  return (this->Internal->NumberOfBlocks);
}

//------------------------------------------------------------------------------
int vtkAMRFlashReader::GetNumberOfLevels()
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  if (!this->IsReady)
  {
    return 0;
  }

  this->Internal->ReadMetaData();
  return (this->Internal->NumberOfLevels);
}

void vtkAMRFlashReader::ComputeStats(
  vtkFlashReaderInternal* internal, std::vector<unsigned int>& numBlocks, double min[3])
{
  min[0] = min[1] = min[2] = DBL_MAX;
  numBlocks.resize(this->Internal->NumberOfLevels, 0);

  for (int i = 0; i < internal->NumberOfBlocks; ++i)
  {
    FlashReaderBlock& theBlock = internal->Blocks[i];
    double* gridMin = theBlock.MinBounds;
    if (gridMin[0] < min[0])
    {
      min[0] = gridMin[0];
    }
    if (gridMin[1] < min[1])
    {
      min[1] = gridMin[1];
    }
    if (gridMin[2] < min[2])
    {
      min[2] = gridMin[2];
    }
    int level = theBlock.Level - 1;
    numBlocks[level]++;
  }
}

//------------------------------------------------------------------------------
int vtkAMRFlashReader::FillMetaData()
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  assert("pre: metadata object is nullptr" && (this->Metadata != nullptr));

  this->Internal->ReadMetaData();

  double origin[3];
  std::vector<unsigned int> blocksPerLevel;
  this->ComputeStats(this->Internal, blocksPerLevel, origin);

  this->Metadata->Initialize(blocksPerLevel);
  this->Metadata->SetGridDescription(vtkStructuredData::VTK_STRUCTURED_XYZ_GRID);
  this->Metadata->SetOrigin(origin);

  std::vector<int> b2level;
  b2level.resize(this->Internal->NumberOfLevels + 1, 0);

  for (int i = 0; i < this->Internal->NumberOfBlocks; ++i)
  {
    FlashReaderBlock& theBlock = this->Internal->Blocks[i];

    // Start numbering levels from 0!
    int level = this->Internal->Blocks[i].Level - 1;
    int id = b2level[level];
    int internalIdx = i;
    int* dims = this->Internal->BlockGridDimensions;

    // compute spacing
    double spacing[3];
    for (int d = 0; d < 3; ++d)
    {
      spacing[d] =
        (dims[d] > 1) ? (theBlock.MaxBounds[d] - theBlock.MinBounds[d]) / (dims[d] - 1.0) : 1.0;
    }

    // compute AMRBox
    vtkAMRBox box(
      theBlock.MinBounds, dims, spacing, origin, vtkStructuredData::VTK_STRUCTURED_XYZ_GRID);

    this->Metadata->SetSpacing(level, spacing);
    this->Metadata->SetAMRBox(level, id, box);
    this->Metadata->SetAMRBlockSourceIndex(level, id, internalIdx);

    b2level[level]++;
  } // END for all blocks

  this->SetMaxLevel(this->Internal->NumberOfLevels);

  return (1);
}

//------------------------------------------------------------------------------
vtkUniformGrid* vtkAMRFlashReader::GetAMRGrid(int blockIdx)
{
  if (!this->IsReady)
  {
    return nullptr;
  }

  double blockMin[3];
  double blockMax[3];
  double spacings[3];

  for (int i = 0; i < 3; ++i)
  {
    blockMin[i] = this->Internal->Blocks[blockIdx].MinBounds[i];
    blockMax[i] = this->Internal->Blocks[blockIdx].MaxBounds[i];
    spacings[i] = (this->Internal->BlockGridDimensions[i] > 1)
      ? (blockMax[i] - blockMin[i]) / (this->Internal->BlockGridDimensions[i] - 1.0)
      : 1.0;
  }

  vtkUniformGrid* ug = vtkUniformGrid::New();
  ug->SetDimensions(this->Internal->BlockGridDimensions);
  ug->SetOrigin(blockMin[0], blockMin[1], blockMin[2]);
  ug->SetSpacing(spacings);
  return (ug);
}

//------------------------------------------------------------------------------
void vtkAMRFlashReader::GetAMRGridData(int blockIdx, vtkUniformGrid* block, const char* field)
{
  assert("pre: AMR block is nullptr" && (block != nullptr));
  this->Internal->GetBlockAttribute(field, blockIdx, block);
}

//------------------------------------------------------------------------------
void vtkAMRFlashReader::SetUpDataArraySelections()
{
  assert("pre: Internal Flash Reader is nullptr" && (this->Internal != nullptr));
  this->Internal->ReadMetaData();

  int numAttrs = static_cast<int>(this->Internal->AttributeNames.size());
  for (int i = 0; i < numAttrs; ++i)
  {
    this->CellDataArraySelection->AddArray(this->Internal->AttributeNames[i].c_str());
  }
}
VTK_ABI_NAMESPACE_END
