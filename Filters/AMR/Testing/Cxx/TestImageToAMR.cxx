// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
// Test vtkImageToAMR filter.

#include "vtkCellData.h"
#include "vtkPointData.h"

#include "vtkAMRBox.h"
#include "vtkDataObject.h"
#include "vtkGenerateIds.h"
#include "vtkIdTypeArray.h"
#include "vtkImageData.h"
#include "vtkImageToAMR.h"
#include "vtkNew.h"
#include "vtkOverlappingAMR.h"
#include "vtkRTAnalyticSource.h"
#include "vtkUniformGrid.h"
#include "vtkVector.h"

#include <vector>
#define VTK_SUCCESS 0
#define VTK_FAILURE 1

namespace
{
//
int ComputeNumCells(vtkOverlappingAMR* amr)
{
  int n(0);
  for (unsigned int level = 0; level < amr->GetNumberOfLevels(); level++)
  {
    int numDataSets = amr->GetNumberOfBlocks(level);
    for (int i = 0; i < numDataSets; i++)
    {
      vtkUniformGrid* grid = amr->GetDataSet(level, i);
      int numCells = grid->GetNumberOfCells();
      for (int cellId = 0; cellId < numCells; cellId++)
      {
        n += grid->IsCellVisible(cellId) ? 1 : 0;
      }
    }
  }
  return n;
}

vtkIdType FindCell(vtkImageData* image, double point[3])
{
  double pcoords[3];
  int subid = 0;
  return image->vtkImageData::FindCell(point, nullptr, -1, 0.1, subid, pcoords, nullptr);
}
}

int TestImageToAMR(int, char*[])
{
  vtkNew<vtkRTAnalyticSource> imageSource;
  imageSource->SetWholeExtent(0, 0, -128, 128, -128, 128);

  vtkNew<vtkGenerateIds> idFilter;
  idFilter->SetInputConnection(imageSource->GetOutputPort());

  vtkNew<vtkImageToAMR> amrConverter;
  amrConverter->SetInputConnection(idFilter->GetOutputPort());
  amrConverter->SetNumberOfLevels(4);

  std::vector<vtkVector3d> samples;
  for (int i = -118; i < 122; i += 10)
  {
    samples.emplace_back(0.0, (double)i, (double)i);
  }

  for (unsigned int numLevels = 1; numLevels <= 4; numLevels++)
  {
    for (int maxBlocks = 10; maxBlocks <= 50; maxBlocks += 10)
    {
      amrConverter->SetNumberOfLevels(numLevels);
      amrConverter->SetMaximumNumberOfBlocks(maxBlocks);
      amrConverter->Update();
      vtkImageData* image = vtkImageData::SafeDownCast(idFilter->GetOutputDataObject(0));
      vtkOverlappingAMR* amr =
        vtkOverlappingAMR::SafeDownCast(amrConverter->GetOutputDataObject(0));
      if (!amr->CheckValidity())
      {
        return VTK_FAILURE;
      }
      if (amr->GetNumberOfLevels() != numLevels)
      {
        return VTK_FAILURE;
      }
      if (maxBlocks < static_cast<int>(amr->GetNumberOfBlocks()))
      {
        return VTK_FAILURE;
      }
      if (ComputeNumCells(amr) != image->GetNumberOfCells())
      {
        return VTK_FAILURE;
      }

      vtkIdTypeArray* cd =
        vtkArrayDownCast<vtkIdTypeArray>(image->GetCellData()->GetArray("vtkCellIds"));
      assert(cd);
      for (std::vector<vtkVector3d>::iterator itr = samples.begin(); itr != samples.end(); ++itr)
      {
        double* x = (*itr).GetData();
        vtkIdType cellId = FindCell(image, x);
        vtkIdType value = cd->GetValue(cellId);
        assert(cellId == value);

        unsigned int level, id;
        if (amr->FindGrid(x, level, id))
        {
          vtkUniformGrid* grid = amr->GetDataSet(level, id);
          vtkIdTypeArray* cd1 =
            vtkArrayDownCast<vtkIdTypeArray>(grid->GetCellData()->GetArray("vtkCellIds"));
          vtkIdType cellId1 = FindCell(grid, x);
          vtkIdType value1 = cd1->GetValue(cellId1);
          if (value1 != value)
          {
            return VTK_FAILURE;
          }
        }
        else
        {
          return VTK_FAILURE;
        }
      }
    }
  }

  return VTK_SUCCESS;
}
