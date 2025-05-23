// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkDIYGhostUtilities.h"

#include "vtkAlgorithm.h"
#include "vtkArrayDispatch.h"
#include "vtkCellData.h"
#include "vtkDIYExplicitAssigner.h"
#include "vtkDataArray.h"
#include "vtkDataArrayRange.h"
#include "vtkDataSet.h"
#include "vtkDataSetSurfaceFilter.h"
#include "vtkFeatureEdges.h"
#include "vtkIdList.h"
#include "vtkIdTypeArray.h"
#include "vtkImageData.h"
#include "vtkIncrementalOctreePointLocator.h"
#include "vtkKdTree.h"
#include "vtkLogger.h"
#include "vtkMathUtilities.h"
#include "vtkMatrix3x3.h"
#include "vtkMultiProcessController.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkRectilinearGrid.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"
#include "vtkStaticPointLocator.h"
#include "vtkStructuredGrid.h"
#include "vtkUnsignedCharArray.h"
#include "vtkUnstructuredGrid.h"

#include <algorithm>
#include <numeric>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <vector>

// clang-format off
#include "vtk_diy2.h"
#include VTK_DIY2(diy/master.hpp)
#include VTK_DIY2(diy/mpi.hpp)
// clang-format on

namespace
{
//@{
/**
 * Convenient typedefs
 */
using ExtentType = vtkDIYGhostUtilities::ExtentType;
using VectorType = vtkDIYGhostUtilities::VectorType;
using QuaternionType = vtkDIYGhostUtilities::QuaternionType;
template <class T>
using BlockMapType = vtkDIYGhostUtilities::BlockMapType<T>;
using Links = vtkDIYGhostUtilities::Links;
using LinkMap = vtkDIYGhostUtilities::LinkMap;
template <class DataSetT>
using DataSetTypeToBlockTypeConverter =
  vtkDIYGhostUtilities::DataSetTypeToBlockTypeConverter<DataSetT>;
//@}

//@{
/**
 * Block typedefs
 */
using ImageDataBlock = vtkDIYGhostUtilities::ImageDataBlock;
using RectilinearGridBlock = vtkDIYGhostUtilities::RectilinearGridBlock;
using StructuredGridBlock = vtkDIYGhostUtilities::StructuredGridBlock;
using UnstructuredDataBlock = vtkDIYGhostUtilities::UnstructuredDataBlock;
using UnstructuredGridBlock = vtkDIYGhostUtilities::UnstructuredGridBlock;
using PolyDataBlock = vtkDIYGhostUtilities::PolyDataBlock;

using ImageDataBlockStructure = ::ImageDataBlock::BlockStructureType;
using RectilinearGridBlockStructure = ::RectilinearGridBlock::BlockStructureType;
using StructuredGridBlockStructure = ::StructuredGridBlock::BlockStructureType;
using UnstructuredDataBlockStructure = ::UnstructuredDataBlock::BlockStructureType;
using UnstructuredGridBlockStructure = ::UnstructuredGridBlock::BlockStructureType;
using PolyDataBlockStructure = ::PolyDataBlock::BlockStructureType;

using ImageDataInformation = ::ImageDataBlock::InformationType;
using RectilinearGridInformation = ::RectilinearGridBlock::InformationType;
using StructuredGridInformation = ::StructuredGridBlock::InformationType;
using UnstructuredDataInformation = ::UnstructuredDataBlock::InformationType;
using UnstructuredGridInformation = ::UnstructuredGridBlock::InformationType;
using PolyDataInformation = ::PolyDataBlock::InformationType;
//@}

constexpr unsigned char GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA =
  vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL |
  vtkDataSetAttributes::CellGhostTypes::HIDDENCELL;

//============================================================================
/**
 * Adjacency bits used for grids.
 * For instance, `Adjacency::Something` means that the neighboring block it refers to is on the
 * `Something` of current block
 * Naming is self-explanatory.
 */
enum Adjacency
{
  Left = 0x01,
  Right = 0x02,
  Front = 0x04,
  Back = 0x08,
  Bottom = 0x10,
  Top = 0x20
};

//============================================================================
/**
 * Bit arrangement encoding how neighboring grid blocks overlap. Two grids overlap in a dimension
 * if and only if the extent segment of the corresponding dimension intersect.
 */
enum Overlap
{
  X = 0x01,
  Y = 0x02,
  XY = 0x03,
  Z = 0x04,
  XZ = 0x05,
  YZ = 0x06
};

//----------------------------------------------------------------------------
constexpr char LOCAL_POINT_IDS_ARRAY_NAME[] = "detail::PointIds";

//----------------------------------------------------------------------------
bool IsExtentValid(const int* extent)
{
  return extent[0] <= extent[1] && extent[2] <= extent[3] && extent[4] <= extent[5];
}

//----------------------------------------------------------------------------
/**
 * This function tags an input cell ghost `array` mapped with input `grid` given the input extent.
 * `array` needs to be already allocated.
 * All virgin cells need to be tagged as hidden ghosts and duplicate ghosts, as it means those
 * cells were not set by neighboring blocks
 */
template <class GridDataSetT>
void TreatVirginGhostCellsForStructuredData(vtkUnsignedCharArray* array, GridDataSetT* grid,
  int imin, int imax, int jmin, int jmax, int kmin, int kmax, unsigned char val)
{
  if (!array)
  {
    return;
  }
  const int* gridExtent = grid->GetExtent();
  int ijk[3];
  for (ijk[2] = kmin; ijk[2] < kmax; ++ijk[2])
  {
    for (ijk[1] = jmin; ijk[1] < jmax; ++ijk[1])
    {
      for (ijk[0] = imin; ijk[0] < imax; ++ijk[0])
      {
        vtkIdType cellId = vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk);
        if (!array->GetValue(cellId))
        {
          array->SetValue(cellId, val);
        }
      }
    }
  }
}

//----------------------------------------------------------------------------
/**
 * This function fills an input point ghost `array` mapped with input `grid` given the input extent.
 * `array` needs to be already allocated.
 * All virgin points need to be tagged as hidden ghosts and duplicate ghosts, as it means those
 * points were not set by neighboring blocks
 */
template <class GridDataSetT>
void TreatVirginGhostPointsForStructuredData(vtkUnsignedCharArray* array, GridDataSetT* grid,
  int imin, int imax, int jmin, int jmax, int kmin, int kmax, unsigned char val)
{
  const int* gridExtent = grid->GetExtent();
  int ijk[3];
  for (ijk[2] = kmin; ijk[2] <= kmax; ++ijk[2])
  {
    for (ijk[1] = jmin; ijk[1] <= jmax; ++ijk[1])
    {
      for (ijk[0] = imin; ijk[0] <= imax; ++ijk[0])
      {
        vtkIdType pointId = vtkStructuredData::ComputePointIdForExtent(gridExtent, ijk);
        if (!array->GetValue(pointId))
        {
          array->SetValue(pointId, val);
        }
      }
    }
  }
}

//----------------------------------------------------------------------------
vtkSmartPointer<vtkIdList> ExtractPointIdsInsideBoundingBox(
  vtkPoints* inputPoints, const vtkBoundingBox& bb)
{
  vtkNew<vtkIdList> pointIds;

  if (!inputPoints)
  {
    return pointIds;
  }

  pointIds->Allocate(inputPoints->GetNumberOfPoints());

  double p[3];

  for (vtkIdType pointId = 0; pointId < inputPoints->GetNumberOfPoints(); ++pointId)
  {
    inputPoints->GetPoint(pointId, p);

    if (bb.ContainsPoint(p))
    {
      pointIds->InsertNextId(pointId);
    }
  }

  return pointIds;
}

//----------------------------------------------------------------------------
template <class PointSetT>
void ExchangeBlockStructuresForUnstructuredData(diy::Master& master)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType;
  using BlockInformationType = typename BlockType::InformationType;

  master.foreach (
    [](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      BlockInformationType& info = block->Information;
      vtkPointSet* interfacePoints =
        vtkPointSet::SafeDownCast(info.InterfaceExtractor->GetOutputDataObject(0));
      vtkIdTypeArray* interfaceGlobalPointIds = info.InterfaceGlobalPointIds;

      for (int id = 0; id < cp.link()->size(); ++id)
      {
        const diy::BlockID& blockId = cp.link()->target(id);

        vtkSmartPointer<vtkIdList> ids = ::ExtractPointIdsInsideBoundingBox(
          interfacePoints->GetPoints(), block->NeighborBoundingBoxes.at(blockId.gid));

        if (!interfacePoints->GetNumberOfPoints())
        {
          cp.enqueue<vtkDataArray*>(blockId, nullptr);
          continue;
        }

        // If we use global ids to match interfacing points, no need to send points
        if (interfaceGlobalPointIds)
        {
          vtkNew<vtkIdTypeArray> gids;
          gids->SetNumberOfValues(ids->GetNumberOfIds());
          interfaceGlobalPointIds->GetTuples(ids, gids);

          cp.enqueue<vtkDataArray*>(blockId, gids);
        }
        else
        {
          vtkNew<vtkPoints> points;
          points->SetDataType(interfacePoints->GetPoints()->GetDataType());
          points->SetNumberOfPoints(ids->GetNumberOfIds());
          interfacePoints->GetPoints()->GetData()->GetTuples(ids, points->GetData());

          cp.enqueue<vtkDataArray*>(blockId, points->GetData());
        }
      }
    });

  master.exchange();

  master.foreach (
    [](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      std::vector<int> incoming;
      cp.incoming(incoming);

      for (const int gid : incoming)
      {
        if (!cp.incoming(gid).empty())
        {
          vtkDataArray* data = nullptr;
          cp.dequeue(gid, data);
          auto& blockStructure = block->BlockStructures[gid];

          if (!data)
          {
            continue;
          }

          if (data->GetNumberOfComponents() == 3)
          {
            blockStructure.InterfacingPoints->SetData(data);
            data->FastDelete();
          }
          else
          {
            blockStructure.InterfacingGlobalPointIds =
              vtkSmartPointer<vtkIdTypeArray>::Take(vtkArrayDownCast<vtkIdTypeArray>(data));
          }
        }
      }
    });
}

//----------------------------------------------------------------------------
template <class StructuredDataSetT>
void CloneGeometricStructuresForStructuredData(
  std::vector<StructuredDataSetT*>& inputs, std::vector<StructuredDataSetT*>& outputs)
{
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    StructuredDataSetT* output = outputs[localId];
    output->CopyStructure(inputs[localId]);

    // If there is blanking using a ghost array, the ghost array is copied
    // by CopyStructure in vtkStructuredGrid's implementation.
    // We DO NOT WANT this array right now, we'll deal with it ourself,
    // so let's delete it.
    output->GetCellData()->RemoveArray(vtkDataSetAttributes::GhostArrayName());
    output->GetPointData()->RemoveArray(vtkDataSetAttributes::GhostArrayName());
  }
}

//----------------------------------------------------------------------------
template <class GridDataSetT>
::ExtentType PeelOffGhostLayers(GridDataSetT* grid)
{
  ::ExtentType extent;
  vtkUnsignedCharArray* ghosts = grid->GetCellGhostArray();
  if (!ghosts)
  {
    grid->GetExtent(extent.data());
    return extent;
  }
  int* gridExtent = grid->GetExtent();

  int ijkmin[3] = { gridExtent[0], gridExtent[2], gridExtent[4] };
  // We use `std::max` here to work for grids of dimension 2 and 1.
  // This gives "thickness" to the degenerate dimension
  int ijkmax[3] = { std::max(gridExtent[1], gridExtent[0] + 1),
    std::max(gridExtent[3], gridExtent[2] + 1), std::max(gridExtent[5], gridExtent[4] + 1) };

  // We lock degenerate dimensions
  bool lock[3] = { gridExtent[0] == gridExtent[1], gridExtent[2] == gridExtent[3],
    gridExtent[4] == gridExtent[5] };

  {
    // Strategy:
    // We create a cursor `ijk` that is at the bottom left front corner of the grid.
    // From there, we iterate each cursor dimension until the targeted brick is not a duplicate
    // ghost. When this happens, we stop the loop, and look in each non degenerate dimension if
    // consecutive shift backs land on a ghost or not. If it lands on a ghost, then the
    // corresponding dimension needs to be peeled up to the current position of the cursor.
    // If not, it doesn't.
    int ijk[3] = { ijkmin[0], ijkmin[1], ijkmin[2] };

    while (ijk[0] < ijkmax[0] && ijk[1] < ijkmax[1] && ijk[2] < ijkmax[2] &&
      (ghosts->GetValue(vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk)) &
        vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL))
    {
      for (int dim = 0; dim < 3; ++dim)
      {
        if (!lock[dim])
        {
          ++ijk[dim];
        }
      }
    }

    for (int dim = 0; dim < 3; ++dim)
    {
      if (!lock[dim] && ijk[dim] != ijkmin[dim])
      {
        int tmp = ijk[dim];
        for (--ijk[dim]; ijk[dim] >= ijkmin[dim] &&
             !(ghosts->GetValue(vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk)) &
               vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL);
             --ijk[dim])
        {
        }
        extent[2 * dim] = ijk[dim] + 1;
        ijk[dim] = tmp;
      }
      else
      {
        extent[2 * dim] = gridExtent[2 * dim];
      }
    }
  }

  {
    // Same pipeline than previous block, but starting from the top back right corner.
    int ijk[3] = { ijkmax[0] - 1, ijkmax[1] - 1, ijkmax[2] - 1 };

    while (ijk[0] >= ijkmin[0] && ijk[1] >= ijkmin[1] && ijk[2] >= ijkmin[2] &&
      (ghosts->GetValue(vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk)) &
        vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL))
    {
      for (int dim = 0; dim < 3; ++dim)
      {
        if (!lock[dim])
        {
          --ijk[dim];
        }
      }
    }

    for (int dim = 0; dim < 3; ++dim)
    {
      if (!lock[dim] && ijk[dim] != ijkmax[dim])
      {
        int tmp = ijk[dim];
        for (++ijk[dim]; ijk[dim] < ijkmax[dim] &&
             !(ghosts->GetValue(vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk)) &
               vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL);
             ++ijk[dim])
        {
        }
        extent[2 * dim + 1] = ijk[dim];
        ijk[dim] = tmp;
      }
      else
      {
        extent[2 * dim + 1] = gridExtent[2 * dim + 1];
      }
    }
  }

  return extent;
}

//----------------------------------------------------------------------------
void AddGhostLayerOfGridPoints(int vtkNotUsed(extentIdx),
  ::ImageDataInformation& vtkNotUsed(information),
  const ::ImageDataBlockStructure& vtkNotUsed(blockStructure))
{
  // Do nothing for image data. Points are all implicit.
}

//----------------------------------------------------------------------------
void AddGhostLayerOfGridPoints(int extentIdx, ::RectilinearGridInformation& blockInformation,
  const ::RectilinearGridBlockStructure& blockStructure)
{
  int layerThickness = blockInformation.ExtentGhostThickness[extentIdx];
  vtkSmartPointer<vtkDataArray>& coordinateGhosts = blockInformation.CoordinateGhosts[extentIdx];
  vtkDataArray* coordinates[3] = { blockStructure.XCoordinates, blockStructure.YCoordinates,
    blockStructure.ZCoordinates };
  vtkDataArray* coords = coordinates[extentIdx / 2];
  if (!coordinateGhosts)
  {
    coordinateGhosts = vtkSmartPointer<vtkDataArray>::Take(coords->NewInstance());
  }
  if (coordinateGhosts->GetNumberOfTuples() < layerThickness)
  {
    if (!(extentIdx % 2))
    {
      vtkSmartPointer<vtkDataArray> tmp =
        vtkSmartPointer<vtkDataArray>::Take(coords->NewInstance());
      tmp->InsertTuples(0, layerThickness - coordinateGhosts->GetNumberOfTuples(),
        coords->GetNumberOfTuples() - layerThickness - 1, coords);
      tmp->InsertTuples(
        tmp->GetNumberOfTuples(), coordinateGhosts->GetNumberOfTuples(), 0, coordinateGhosts);
      std::swap(tmp, coordinateGhosts);
    }
    else
    {
      coordinateGhosts->InsertTuples(coordinateGhosts->GetNumberOfTuples(),
        layerThickness - coordinateGhosts->GetNumberOfTuples(), 1, coords);
    }
  }
}

//----------------------------------------------------------------------------
void AddGhostLayerOfGridPoints(int vtkNotUsed(extentIdx),
  ::StructuredGridInformation& vtkNotUsed(blockInformation),
  const ::StructuredGridBlockStructure& vtkNotUsed(blockStructure))
{
  // Do nothing, we only have grid interfaces at this point. We will allocate the points
  // after the accumulated extent is computed.
}

//----------------------------------------------------------------------------
/**
 * This function is only used for grid inputs. It updates the extents of the output of current block
 * to account for an adjacency with a block at index `idx` inside the extent.
 * We store this extent information inside ExtentGhostThickness, which describes the ghost layer
 * thickness in each direction that we should add in the output.
 * It also updates the extent of the neighbor block so we know its extent when ghosts are added.
 */
template <class BlockT>
void AddGhostLayerToGrid(int idx, int outputGhostLevels,
  typename BlockT::BlockStructureType& blockStructure,
  typename BlockT::InformationType& blockInformation)
{
  const ::ExtentType& extent = blockStructure.ShiftedExtent;
  ::ExtentType& shiftedExtentWithNewGhosts = blockStructure.ShiftedExtentWithNewGhosts;
  ::ExtentType& receivedGhostExtent = blockStructure.ReceivedGhostExtent;

  bool upperBound = idx % 2;
  int oppositeIdx = upperBound ? idx - 1 : idx + 1;
  int localOutputGhostLevels =
    std::min(outputGhostLevels, std::abs(extent[idx] - extent[oppositeIdx]));
  blockInformation.ExtentGhostThickness[idx] =
    std::max(blockInformation.ExtentGhostThickness[idx], localOutputGhostLevels);

  receivedGhostExtent[oppositeIdx] = shiftedExtentWithNewGhosts[oppositeIdx];
  shiftedExtentWithNewGhosts[oppositeIdx] += (upperBound ? -1.0 : 1.0) * localOutputGhostLevels;
  receivedGhostExtent[idx] =
    receivedGhostExtent[oppositeIdx] + (upperBound ? 1.0 : -1.0) * localOutputGhostLevels;

  ::AddGhostLayerOfGridPoints(idx, blockInformation, blockStructure);
}

//----------------------------------------------------------------------------
/**
 * This function looks at the situation when shared dimensions with our neighbor
 * are such that we extend further that our neighbor. If so, we need to extend the new extent of
 * our neighbor as well because we have data that they will need.
 * We look a that in the 2 remaining dimensions.
 */
template <class BlockT>
void ExtendSharedInterfaceIfNeeded(int idx, int outputGhostLevels,
  typename BlockT::BlockStructureType& blockStructure,
  typename BlockT::InformationType& blockInformation)
{
  const ::ExtentType& extent = blockStructure.ShiftedExtent;
  const ::ExtentType& localExtent = blockInformation.Extent;
  ::ExtentType& shiftedExtentWithNewGhosts = blockStructure.ShiftedExtentWithNewGhosts;

  if (extent[idx] > localExtent[idx])
  {
    shiftedExtentWithNewGhosts[idx] -= outputGhostLevels;
    if (shiftedExtentWithNewGhosts[idx] < localExtent[idx])
    {
      shiftedExtentWithNewGhosts[idx] = localExtent[idx];
    }
  }
  if (extent[idx + 1] < localExtent[idx + 1])
  {
    shiftedExtentWithNewGhosts[idx + 1] += outputGhostLevels;
    if (shiftedExtentWithNewGhosts[idx + 1] > localExtent[idx + 1])
    {
      shiftedExtentWithNewGhosts[idx + 1] = localExtent[idx + 1];
    }
  }
}

//----------------------------------------------------------------------------
/**
 * This function is to be used with grids only.
 * At given position inside `blockStructures` pointed by iterator `it`, and given a computed
 * `adjacencyMask` and `overlapMask` and input ghost levels, this function updates the accumulated
 * extent shift (`outputExtentShift`) for the output grid, as well as the extent of the current
 * block's neighbor `neighborShiftedExtentWithNewGhosts`.
 */
template <class BlockT, class IteratorT>
void LinkGrid(::BlockMapType<typename BlockT::BlockStructureType>& blockStructures, IteratorT& it,
  typename BlockT::InformationType& blockInformation, ::Links& localLinks,
  unsigned char adjacencyMask, unsigned char overlapMask, int outputGhostLevels, int dim)
{
  // If there is no adjacency or overlap, then blocks are not connected
  if (adjacencyMask == 0 && overlapMask == 0)
  {
    it = blockStructures.erase(it);
    return;
  }

  int gid = it->first;
  auto& blockStructure = it->second;

  // Here we look at adjacency where faces overlap
  //   ______
  //  /__/__/|
  // |  |  | |
  // |__|__|/
  //
  if ((((dim == 3 && overlapMask == ::Overlap::YZ) || (dim == 2 && overlapMask & ::Overlap::YZ) ||
         (dim == 1 && !overlapMask)) &&
        (adjacencyMask & (::Adjacency::Left | ::Adjacency::Right))) ||
    ((((dim == 3 && overlapMask == ::Overlap::XZ) || (dim == 2 && overlapMask & ::Overlap::XZ))) &&
      (adjacencyMask & (::Adjacency::Front | ::Adjacency::Back))) ||
    ((((dim == 3 && overlapMask == ::Overlap::XY) || (dim == 2 && overlapMask & ::Overlap::XY))) &&
      (adjacencyMask & (::Adjacency::Bottom | ::Adjacency::Top))))
  {
    // idx is the index in extent of current block on which side the face overlap occurs
    int idx = -1;
    switch (adjacencyMask)
    {
      case ::Adjacency::Left:
        idx = 0;
        break;
      case ::Adjacency::Right:
        idx = 1;
        break;
      case ::Adjacency::Front:
        idx = 2;
        break;
      case ::Adjacency::Back:
        idx = 3;
        break;
      case ::Adjacency::Bottom:
        idx = 4;
        break;
      case ::Adjacency::Top:
        idx = 5;
        break;
      default:
        // Blocks are not connected, we can erase current block
        it = blockStructures.erase(it);
        if (dim != 1)
        {
          vtkLog(ERROR, "Wrong adjacency mask for 1D grid inputs ");
        }
        return;
    }

    AddGhostLayerToGrid<BlockT>(idx, outputGhostLevels, blockStructure, blockInformation);
  }
  // Here we look at adjacency where edges overlaps but no face overlap occurs
  //   ___
  //  /__/|
  // |  | |__
  // |__|/__/|
  //    |  | |
  //    |__|/
  //
  else if ((((dim == 3 && overlapMask == ::Overlap::X) || (dim == 2 && !overlapMask)) &&
             (adjacencyMask & (::Adjacency::Front | ::Adjacency::Back)) &&
             (adjacencyMask & (::Adjacency::Bottom | ::Adjacency::Top))) ||
    (((dim == 3 && overlapMask == ::Overlap::Y) || (dim == 2 && !overlapMask)) &&
      (adjacencyMask & (::Adjacency::Left | ::Adjacency::Right)) &&
      (adjacencyMask & (::Adjacency::Bottom | ::Adjacency::Top))) ||
    (((dim == 3 && overlapMask == ::Overlap::Z) || (dim == 2 && !overlapMask)) &&
      (adjacencyMask & (::Adjacency::Left | ::Adjacency::Right)) &&
      (adjacencyMask & (::Adjacency::Front | ::Adjacency::Back))))
  {
    // idx1 and idx2 are the indices in extent of current block
    // such that the intersection of the 2 faces mapped by those 2 indices is the overlapping edge.
    int idx1 = -1, idx2 = -1;
    switch (adjacencyMask)
    {
      case ::Adjacency::Front | ::Adjacency::Bottom:
        idx1 = 2;
        idx2 = 4;
        break;
      case ::Adjacency::Front | ::Adjacency::Top:
        idx1 = 2;
        idx2 = 5;
        break;
      case ::Adjacency::Back | ::Adjacency::Bottom:
        idx1 = 3;
        idx2 = 4;
        break;
      case ::Adjacency::Back | ::Adjacency::Top:
        idx1 = 3;
        idx2 = 5;
        break;
      case ::Adjacency::Left | ::Adjacency::Bottom:
        idx1 = 0;
        idx2 = 4;
        break;
      case ::Adjacency::Left | ::Adjacency::Top:
        idx1 = 0;
        idx2 = 5;
        break;
      case ::Adjacency::Right | ::Adjacency::Bottom:
        idx1 = 1;
        idx2 = 4;
        break;
      case ::Adjacency::Right | ::Adjacency::Top:
        idx1 = 1;
        idx2 = 5;
        break;
      case ::Adjacency::Left | ::Adjacency::Front:
        idx1 = 0;
        idx2 = 2;
        break;
      case ::Adjacency::Left | ::Adjacency::Back:
        idx1 = 0;
        idx2 = 3;
        break;
      case ::Adjacency::Right | ::Adjacency::Front:
        idx1 = 1;
        idx2 = 2;
        break;
      case ::Adjacency::Right | ::Adjacency::Back:
        idx1 = 1;
        idx2 = 3;
        break;
      default:
        // Blocks are not connected, we can erase current block
        it = blockStructures.erase(it);
        if (dim != 2)
        {
          vtkLog(ERROR, "Wrong adjacency mask for 2D grid inputs");
        }
        return;
    }

    AddGhostLayerToGrid<BlockT>(idx1, outputGhostLevels, blockStructure, blockInformation);
    AddGhostLayerToGrid<BlockT>(idx2, outputGhostLevels, blockStructure, blockInformation);
  }
  // Here we look at adjacency where corners touch but no edges / faces overlap
  //   ___
  //  /__/|
  // |  | |
  // |__|/__
  //    /__/|
  //   |  | |
  //   |__|/
  //
  else
  {
    // idx1, idx2 and idx3 are the indices in extent of current block
    // such that the intersection of the 3 faces mapped by those 3 indices is the concurrent corner.
    int idx1 = -1, idx2 = -1, idx3 = -1;
    switch (adjacencyMask)
    {
      case ::Adjacency::Left | ::Adjacency::Front | ::Adjacency::Bottom:
        idx1 = 0;
        idx2 = 2;
        idx3 = 4;
        break;
      case ::Adjacency::Left | ::Adjacency::Front | ::Adjacency::Top:
        idx1 = 0;
        idx2 = 2;
        idx3 = 5;
        break;
      case ::Adjacency::Left | ::Adjacency::Back | ::Adjacency::Bottom:
        idx1 = 0;
        idx2 = 3;
        idx3 = 4;
        break;
      case ::Adjacency::Left | ::Adjacency::Back | ::Adjacency::Top:
        idx1 = 0;
        idx2 = 3;
        idx3 = 5;
        break;
      case ::Adjacency::Right | ::Adjacency::Front | ::Adjacency::Bottom:
        idx1 = 1;
        idx2 = 2;
        idx3 = 4;
        break;
      case ::Adjacency::Right | ::Adjacency::Front | ::Adjacency::Top:
        idx1 = 1;
        idx2 = 2;
        idx3 = 5;
        break;
      case ::Adjacency::Right | ::Adjacency::Back | ::Adjacency::Bottom:
        idx1 = 1;
        idx2 = 3;
        idx3 = 4;
        break;
      case ::Adjacency::Right | ::Adjacency::Back | ::Adjacency::Top:
        idx1 = 1;
        idx2 = 3;
        idx3 = 5;
        break;
      default:
        // Blocks are not connected, we can erase current block
        it = blockStructures.erase(it);
        if (dim != 3)
        {
          vtkLog(ERROR, "Wrong adjacency mask for 3D grid inputs ");
        }
        return;
    }

    AddGhostLayerToGrid<BlockT>(idx1, outputGhostLevels, blockStructure, blockInformation);
    AddGhostLayerToGrid<BlockT>(idx2, outputGhostLevels, blockStructure, blockInformation);
    AddGhostLayerToGrid<BlockT>(idx3, outputGhostLevels, blockStructure, blockInformation);
  }

  if (overlapMask)
  {
    int idx1 = -1, idx2 = -1;
    switch (overlapMask)
    {
      case ::Overlap::X:
        idx1 = 0;
        break;
      case ::Overlap::Y:
        idx1 = 2;
        break;
      case ::Overlap::Z:
        idx1 = 4;
        break;
      case ::Overlap::XY:
        idx1 = 0;
        idx2 = 2;
        break;
      case ::Overlap::YZ:
        idx1 = 2;
        idx2 = 4;
        break;
      case ::Overlap::XZ:
        idx1 = 0;
        idx2 = 4;
        break;
      default:
        vtkLog(ERROR,
          "This line should never be reached. overlapMask likely equals Overlap::XYZ,"
          " which is impossible.");
        break;
    }

    ::ExtendSharedInterfaceIfNeeded<BlockT>(
      idx1, outputGhostLevels, blockStructure, blockInformation);
    if (idx2 != -1)
    {
      ::ExtendSharedInterfaceIfNeeded<BlockT>(
        idx2, outputGhostLevels, blockStructure, blockInformation);
    }
  }

  // If we reach this point, then the current neighboring block is indeed adjacent to us.
  // We add it to our link map.
  localLinks.emplace(gid);

  // We need to iterate by hand here because of the potential iterator erasure in this function.
  ++it;
}

//----------------------------------------------------------------------------
/**
 * This function computes the adjacency and overlap masks mapping the configuration between the 2
 * input extents `localExtent` and `extent`
 */
void ComputeAdjacencyAndOverlapMasks(const ::ExtentType& localExtent, const ::ExtentType& extent,
  unsigned char& adjacencyMask, unsigned char& overlapMask)
{
  // AdjacencyMask is a binary mask that is trigger if 2
  // blocks are adjacent. Dimensionnality of the grid is carried away
  // by discarding any bit that is on a degenerate dimension
  adjacencyMask = (((localExtent[0] == extent[1]) * ::Adjacency::Left) |
                    ((localExtent[1] == extent[0]) * ::Adjacency::Right) |
                    ((localExtent[2] == extent[3]) * ::Adjacency::Front) |
                    ((localExtent[3] == extent[2]) * ::Adjacency::Back) |
                    ((localExtent[4] == extent[5]) * ::Adjacency::Bottom) |
                    ((localExtent[5] == extent[4]) * ::Adjacency::Top)) &
    (((::Adjacency::Left | ::Adjacency::Right) * (localExtent[0] != localExtent[1])) |
      ((::Adjacency::Front | ::Adjacency::Back) * (localExtent[2] != localExtent[3])) |
      ((::Adjacency::Bottom | ::Adjacency::Top) * (localExtent[4] != localExtent[5])));

  overlapMask = ((localExtent[0] < extent[1] && extent[0] < localExtent[1])) |
    ((localExtent[2] < extent[3] && extent[2] < localExtent[3]) << 1) |
    ((localExtent[4] < extent[5] && extent[4] < localExtent[5]) << 2);
}

//----------------------------------------------------------------------------
bool AreExtentsAdjacent(const ::ExtentType& e1, const ::ExtentType& e2)
{
  return (e1[0] != e1[1] ? e1[0] == e2[1] || e1[1] == e2[0] : false) ||
    (e1[2] != e1[3] ? e1[2] == e2[3] || e1[3] == e2[2] : false) ||
    (e1[4] != e1[5] ? e1[4] == e2[5] || e1[5] == e2[4] : false);
}

//----------------------------------------------------------------------------
/**
 * Function to be overloaded for each supported input grid data sets.
 * This function will return true if 2 input block structures are adjacent, false otherwise.
 */
bool SynchronizeGridExtents(
  const ::ImageDataBlockStructure& localBlockStructure, ::ImageDataBlockStructure& blockStructure)
{
  // Images are spatially defined by origin, spacing, dimension, and orientation.
  // We make sure that they all connect well using those values.
  const ::VectorType& localOrigin = localBlockStructure.Origin;
  const ::VectorType& localSpacing = localBlockStructure.Spacing;
  const ::QuaternionType& localQ = localBlockStructure.OrientationQuaternion;
  const ::ExtentType& localExtent = localBlockStructure.Extent;
  int localDim = localBlockStructure.DataDimension;

  const ::ExtentType& extent = blockStructure.Extent;
  ::ExtentType& shiftedExtent = blockStructure.ShiftedExtent;
  const ::QuaternionType& q = blockStructure.OrientationQuaternion;
  const ::VectorType& spacing = blockStructure.Spacing;
  int dim = blockStructure.DataDimension;

  // We skip if dimension, spacing or quaternions don't match
  // spacing == localSpacing <=> dot(spacing, localSpacing) == norm(localSpacing)^2
  // q == localQ <=> dot(q, localQ) == 1 (both are unitary quaternions)
  if (extent[0] > extent[1] || extent[2] > extent[3] || extent[4] > extent[5] || dim != localDim ||
    !vtkMathUtilities::NearlyEqual(
      vtkMath::Dot(spacing, localSpacing), vtkMath::SquaredNorm(localSpacing)) ||
    !(std::fabs(vtkMath::Dot<double, 4>(q.GetData(), localQ.GetData()) - 1.0) < VTK_DBL_EPSILON))
  {
    return false;
  }

  // We reposition extent all together so we have a unified extent framework with the current
  // neighbor.
  const ::VectorType& origin = blockStructure.Origin;
  int originDiff[3] = { static_cast<int>(std::lround((origin[0] - localOrigin[0]) / spacing[0])),
    static_cast<int>(std::lround((origin[1] - localOrigin[1]) / spacing[1])),
    static_cast<int>(std::lround((origin[2] - localOrigin[2]) / spacing[2])) };

  shiftedExtent[0] = extent[0] + originDiff[0];
  shiftedExtent[1] = extent[1] + originDiff[0];
  shiftedExtent[2] = extent[2] + originDiff[1];
  shiftedExtent[3] = extent[3] + originDiff[1];
  shiftedExtent[4] = extent[4] + originDiff[2];
  shiftedExtent[5] = extent[5] + originDiff[2];

  return ::AreExtentsAdjacent(shiftedExtent, localExtent);
}

//============================================================================
template <bool IsIntegerT>
struct Comparator;

//============================================================================
template <>
struct Comparator<true>
{
  template <class ValueT1, class ValueT2>
  static bool Equals(const ValueT1& localVal, const ValueT2& val)
  {
    return !(localVal - val);
  }
};

//============================================================================
template <>
struct Comparator<false>
{
  template <class ValueT, bool IsFloatingPointT = std::is_floating_point<ValueT>::value>
  struct ValueToScalar;

  template <class ValueT>
  struct ValueToScalar<ValueT, true>
  {
    using Type = ValueT;
  };

  template <class ValueT>
  struct ValueToScalar<ValueT, false>
  {
    using Type = typename ValueT::value_type;
  };

  template <class ValueT1, class ValueT2>
  static bool Equals(const ValueT1& val1, const ValueT2& val2)
  {
    using Scalar = typename ValueToScalar<ValueT1>::Type;

    return std::fabs(val1 - val2) < vtkDIYGhostUtilities_detail::ComputePrecision<Scalar>(
                                      std::max(std::fabs(val1), std::fabs(val2)));
  }
};

//============================================================================
struct RectilinearGridFittingWorker
{
  RectilinearGridFittingWorker(vtkDataArray* array)
    : Array(array)
  {
  }

  template <class ArrayT>
  void operator()(ArrayT* localArray)
  {
    ArrayT* array = ArrayT::SafeDownCast(this->Array);

    auto localArrayRange = vtk::DataArrayValueRange(localArray);
    auto arrayRange = vtk::DataArrayValueRange(array);

    if (localArrayRange[localArrayRange.size() - 1] > arrayRange[arrayRange.size() - 1])
    {
      this->FitArrays(arrayRange, localArrayRange);
    }
    else
    {
      this->FitArrays(localArrayRange, arrayRange);
      std::swap(this->MinId, this->LocalMinId);
      std::swap(this->MaxId, this->LocalMaxId);
    }
  }

  template <class RangeT>
  void FitArrays(const RangeT& lowerMaxArray, const RangeT& upperMaxArray)
  {
    using ValueType = typename RangeT::ValueType;
    constexpr bool IsInteger = std::numeric_limits<ValueType>::is_integer;

    const auto& lowerMinArray = lowerMaxArray[0] > upperMaxArray[0] ? upperMaxArray : lowerMaxArray;
    const auto& upperMinArray = lowerMaxArray[0] < upperMaxArray[0] ? upperMaxArray : lowerMaxArray;
    vtkIdType id = 0;

    while (id < lowerMinArray.size() &&
      (lowerMinArray[id] < upperMinArray[0] &&
        !Comparator<IsInteger>::Equals(lowerMinArray[id], upperMinArray[0])))
    {
      ++id;
    }
    if (this->SubArraysAreEqual(lowerMinArray, upperMinArray, id))
    {
      this->LocalMinId = 0;
      this->MinId = id;
      if (lowerMaxArray[0] > upperMaxArray[0])
      {
        std::swap(this->MaxId, this->LocalMaxId);
      }
    }
  }

  template <class RangeT>
  bool SubArraysAreEqual(const RangeT& lowerArray, const RangeT& upperArray, vtkIdType lowerId)
  {
    vtkIdType upperId = 0;
    using ValueType = typename RangeT::ValueType;
    constexpr bool IsInteger = std::numeric_limits<ValueType>::is_integer;
    while (lowerId < lowerArray.size() && upperId < upperArray.size() &&
      Comparator<IsInteger>::Equals(lowerArray[lowerId], upperArray[upperId]))
    {
      ++lowerId;
      ++upperId;
    }
    if (lowerId == lowerArray.size())
    {
      this->MaxId = lowerId - 1;
      this->LocalMaxId = upperId - 1;
      this->Overlaps = true;
      return true;
    }
    return false;
  }

  vtkDataArray* Array;
  int MinId = 0, MaxId = -1, LocalMinId = 0, LocalMaxId = -1;
  bool Overlaps = false;
};

//----------------------------------------------------------------------------
/**
 * Function to be overloaded for each supported input grid data sets.
 * This function will return true if 2 input block structures are adjacent, false otherwise.
 */
bool SynchronizeGridExtents(const ::RectilinearGridBlockStructure& localBlockStructure,
  ::RectilinearGridBlockStructure& blockStructure)
{
  const ::ExtentType& extent = blockStructure.Extent;
  if (localBlockStructure.DataDimension != blockStructure.DataDimension || extent[0] > extent[1] ||
    extent[2] > extent[3] || extent[4] > extent[5])
  {
    return false;
  }
  const ::ExtentType& localExtent = localBlockStructure.Extent;

  const vtkSmartPointer<vtkDataArray>& localXCoordinates = localBlockStructure.XCoordinates;
  const vtkSmartPointer<vtkDataArray>& localYCoordinates = localBlockStructure.YCoordinates;
  const vtkSmartPointer<vtkDataArray>& localZCoordinates = localBlockStructure.ZCoordinates;

  const vtkSmartPointer<vtkDataArray>& xCoordinates = blockStructure.XCoordinates;
  const vtkSmartPointer<vtkDataArray>& yCoordinates = blockStructure.YCoordinates;
  const vtkSmartPointer<vtkDataArray>& zCoordinates = blockStructure.ZCoordinates;

  using Dispatcher = vtkArrayDispatch::Dispatch;
  RectilinearGridFittingWorker xWorker(xCoordinates), yWorker(yCoordinates), zWorker(zCoordinates);

  if (!Dispatcher::Execute(localXCoordinates, xWorker))
  {
    xWorker(localXCoordinates.GetPointer());
  }
  if (!Dispatcher::Execute(localYCoordinates, yWorker))
  {
    yWorker(localYCoordinates.GetPointer());
  }
  if (!Dispatcher::Execute(localZCoordinates, zWorker))
  {
    zWorker(localZCoordinates.GetPointer());
  }

  // The overlap between the 2 grids needs to have at least one degenerate dimension in order
  // for them to be adjacent.
  if ((!xWorker.Overlaps || !yWorker.Overlaps || !zWorker.Overlaps) &&
    (xWorker.MinId != xWorker.MaxId || yWorker.MinId != yWorker.MaxId ||
      zWorker.MinId != zWorker.MaxId))
  {
    return false;
  }

  int originDiff[3] = { extent[0] + xWorker.MinId - localExtent[0] - xWorker.LocalMinId,
    extent[2] + yWorker.MinId - localExtent[2] - yWorker.LocalMinId,
    extent[4] + zWorker.MinId - localExtent[4] - zWorker.LocalMinId };

  blockStructure.ShiftedExtent =
    ::ExtentType{ extent[0] + originDiff[0], extent[1] + originDiff[0], extent[2] + originDiff[1],
      extent[3] + originDiff[1], extent[4] + originDiff[2], extent[5] + originDiff[2] };

  return true;
}

//============================================================================
struct StructuredGridFittingWorker
{
  /**
   * Constructor storing the 6 faces of the neighboring block.
   */
  StructuredGridFittingWorker(const vtkSmartPointer<vtkPoints> points[6],
    vtkNew<vtkStaticPointLocator> locator[6], const ::ExtentType& extent,
    ::StructuredGridBlockStructure::Grid2D& grid, int dimension)
    : Points{ points[0]->GetData(), points[1]->GetData(), points[2]->GetData(),
      points[3]->GetData(), points[4]->GetData(), points[5]->GetData() }
    , Locator{ locator[0], locator[1], locator[2], locator[3], locator[4], locator[5] }
    , Grid(grid)
    , Dimension(dimension)
  {
    // We compute the extent of each external face of the neighbor block.
    for (int i = 0; i < 6; ++i)
    {
      ::ExtentType& e = this->Extent[i];
      e[i] = extent[i];
      e[i % 2 ? i - 1 : i + 1] = extent[i];
      for (int j = 0; j < 6; ++j)
      {
        if (i / 2 != j / 2)
        {
          e[j] = extent[j];
        }
      }
    }
  }

  /**
   * This method determines the local points (points from on external face of local block) are
   * connected the the points of one of the 6 faces of the block's neighbor.
   * The main subroutine `GridsFit` is asymmetrical: it needs to be called twice, first by querying
   * from the local block, finally by querying from the neighbor's block.
   *
   * If grids are connected, the overlapping extent is extracted in the form of a 2D grid.
   *
   * This method determines if grids are connected regardless of the orientation of their extent.
   * This means that given a direct frame (i, j, k) spanning the first grid, i can be mangled with
   * any dimension of the other grid. To simplify mpi communication, the convention is express the
   * indexing of the neighboring block relative to the one of the local block. For instance, if we
   * find that (i, -j) of the first grid connect with (j, k) of the second, we will multiply the
   * second dimension by -1 so that the local grid is spanned by (i, j), and the second by (j, -k).
   */
  template <class ArrayT>
  void operator()(ArrayT* localPoints)
  {
    auto localPointsRange = vtk::DataArrayTupleRange<3>(localPoints);

    for (int dim = 0; dim < 3; ++dim)
    {
      if (this->Extent[2 * dim] == this->Extent[2 * dim + 1])
      {
        continue;
      }

      for (int sideId = 2 * dim; sideId <= 2 * dim + 1; ++sideId)
      {
        ArrayT* points = vtkArrayDownCast<ArrayT>(this->Points[sideId]);
        auto pointsRange = vtk::DataArrayTupleRange<3>(points);

        if (this->GridsFit(localPointsRange, this->LocalExtent, this->LocalExtentIndex, pointsRange,
              this->Locator[sideId], this->Extent[sideId], sideId))
        {
          this->Connected = true;
        }
        else if (this->GridsFit(pointsRange, this->Extent[sideId], sideId, localPointsRange,
                   this->LocalLocator, this->LocalExtent, this->LocalExtentIndex))
        {
          this->Connected = true;
          std::swap(this->Grid, this->LocalGrid);
        }
        else
        {
          continue;
        }

        // Now, we flip the grids so the local grid uses canonical coordinates (x increasing, y
        // increasing)
        if (this->LocalGrid.StartX > this->LocalGrid.EndX)
        {
          std::swap(this->LocalGrid.StartX, this->LocalGrid.EndX);
          this->LocalGrid.XOrientation *= -1;
          std::swap(this->Grid.StartX, this->Grid.EndX);
          this->Grid.XOrientation *= -1;
        }
        if (this->LocalGrid.StartY > this->LocalGrid.EndY)
        {
          std::swap(this->LocalGrid.StartY, this->LocalGrid.EndY);
          this->LocalGrid.YOrientation *= -1;
          std::swap(this->Grid.StartY, this->Grid.EndY);
          this->Grid.YOrientation *= -1;
        }

        if (this->BestConnectionFound)
        {
          return;
        }
      }
    }
  }

  /**
   * This looks if the 4 corners of the grid composed of points from `queryPoints` are points of the
   * second grid.
   * queryExtentId and extentId are parameters that tell on which face of the block the grids lie.
   * For each corners part of the neighboring grids, a subroutine is called to see if grids fit
   * perfectly. One match is not a sufficient condition for us to stop checking if grids fit.
   * Indeed, one can catch an edge on one face, while an entire face fits elsewhere, so this method
   * might be called even if a match has been found.
   */
  template <class PointRangeT>
  bool GridsFit(const PointRangeT& queryPoints, const ::ExtentType& queryExtent, int queryExtentId,
    const PointRangeT& points, vtkAbstractPointLocator* locator, const ::ExtentType& extent,
    int extentId)
  {
    using ConstPointRef = typename PointRangeT::ConstTupleReferenceType;
    using ValueType = typename ConstPointRef::value_type;

    bool retVal = false;

    int queryXDim = (queryExtentId + 2) % 6;
    queryXDim -= queryXDim % 2;
    int queryYDim = (queryExtentId + 4) % 6;
    queryYDim -= queryYDim % 2;
    int queryijk[3];
    queryijk[queryExtentId / 2] = queryExtent[queryExtentId];

    int xCorners[2] = { queryExtent[queryXDim], queryExtent[queryXDim + 1] };
    int yCorners[2] = { queryExtent[queryYDim], queryExtent[queryYDim + 1] };
    int xNumberOfCorners = xCorners[0] == xCorners[1] ? 1 : 2;
    int yNumberOfCorners = yCorners[0] == yCorners[1] ? 1 : 2;

    constexpr int sweepDirection[2] = { 1, -1 };
    double dist2;

    for (int xCornerId = 0; xCornerId < xNumberOfCorners; ++xCornerId)
    {
      queryijk[queryXDim / 2] = xCorners[xCornerId];
      for (int yCornerId = 0; yCornerId < yNumberOfCorners; ++yCornerId)
      {
        queryijk[queryYDim / 2] = yCorners[yCornerId];
        vtkIdType queryPointId =
          vtkStructuredData::ComputePointIdForExtent(queryExtent.data(), queryijk);
        ConstPointRef queryPoint = queryPoints[queryPointId];

        double tmp[3] = { static_cast<double>(queryPoint[0]), static_cast<double>(queryPoint[1]),
          static_cast<double>(queryPoint[2]) };

        vtkIdType pointId = locator->FindClosestPointWithinRadius(
          vtkDIYGhostUtilities_detail::ComputePrecision<ValueType>(
            std::max({ std::fabs(tmp[0]), std::fabs(tmp[1]), std::fabs(tmp[2]) })),
          tmp, dist2);

        if (pointId == -1)
        {
          continue;
        }

        if (this->SweepGrids(queryPoints, queryExtentId, queryExtent, queryXDim,
              xCorners[xCornerId], xCorners[(xCornerId + 1) % 2], sweepDirection[xCornerId],
              queryYDim, yCorners[yCornerId], yCorners[(yCornerId + 1) % 2],
              sweepDirection[yCornerId], points, pointId, extentId, extent))
        {
          retVal = true;
        }
      }
    }
    return retVal;
  }

  /**
   * This method is called when one corner of the querying grid exists inside the other grid.
   * Both grids are swept in all directions. If points match until one corner is reached, then grids
   * are connected. If grids are connected, if the grid overlapping is larger than any previous
   * computed one, its extents and the id of the face are saved.
   */
  template <class PointRangeT>
  bool SweepGrids(const PointRangeT& queryPoints, int queryExtentId,
    const ::ExtentType& queryExtent, int queryXDim, int queryXBegin, int queryXEnd, int directionX,
    int queryYDim, int queryYBegin, int queryYEnd, int directionY, const PointRangeT& points,
    int pointId, int extentId, const ::ExtentType& extent)
  {
    using ConstPointRef = typename PointRangeT::ConstTupleReferenceType;
    using ValueType = typename ConstPointRef::value_type;
    constexpr bool IsInteger = std::numeric_limits<ValueType>::is_integer;
    constexpr int sweepDirection[2] = { 1, -1 };

    bool retVal = false;

    int queryijk[3], ijk[3];
    queryijk[queryExtentId / 2] = queryExtent[queryExtentId];
    vtkStructuredData::ComputePointStructuredCoordsForExtent(pointId, extent.data(), ijk);

    int xdim = ((extentId + 2) % 6);
    xdim -= xdim % 2;
    int ydim = ((extentId + 4) % 6);
    ydim -= ydim % 2;

    int xCorners[2] = { extent[xdim], extent[xdim + 1] };
    int yCorners[2] = { extent[ydim], extent[ydim + 1] };
    int xNumberOfCorners = xCorners[0] == xCorners[1] ? 1 : 2;
    int yNumberOfCorners = yCorners[0] == yCorners[1] ? 1 : 2;

    int xBegin = ijk[xdim / 2];
    int yBegin = ijk[ydim / 2];

    for (int xCornerId = 0; xCornerId < xNumberOfCorners; ++xCornerId)
    {
      for (int yCornerId = 0; yCornerId < yNumberOfCorners; ++yCornerId)
      {
        bool gridsAreFitting = true;
        int queryX, queryY = queryYBegin, x, y = yBegin;

        for (queryX = queryXBegin, x = xBegin; queryX != queryXEnd + directionX &&
             x != xCorners[(xCornerId + 1) % 2] + sweepDirection[xCornerId];
             queryX += directionX, x += sweepDirection[xCornerId])
        {
          queryijk[queryXDim / 2] = queryX;
          ijk[xdim / 2] = x;

          for (queryY = queryYBegin, y = yBegin;
               gridsAreFitting && queryY != queryYEnd + directionY &&
               y != yCorners[(yCornerId + 1) % 2] + sweepDirection[yCornerId];
               queryY += directionY, y += sweepDirection[yCornerId])
          {
            queryijk[queryYDim / 2] = queryY;
            ijk[ydim / 2] = y;

            vtkIdType queryPointId =
              vtkStructuredData::ComputePointIdForExtent(queryExtent.data(), queryijk);
            vtkIdType id = vtkStructuredData::ComputePointIdForExtent(extent.data(), ijk);

            ConstPointRef queryPoint = queryPoints[queryPointId];
            ConstPointRef point = points[id];

            if (!Comparator<IsInteger>::Equals(point[0], queryPoint[0]) ||
              !Comparator<IsInteger>::Equals(point[1], queryPoint[1]) ||
              !Comparator<IsInteger>::Equals(point[2], queryPoint[2]))
            {
              gridsAreFitting = false;
              break;
            }
          }
        }
        queryX -= directionX;
        queryY -= directionY;
        x -= sweepDirection[xCornerId];
        y -= sweepDirection[yCornerId];
        // We save grid characteristics if the new grid is larger than the previous selected one.
        // This can happen when an edge is caught, but a whole face should be caught instead
        if (gridsAreFitting &&
          ((this->LocalGrid.EndX == this->LocalGrid.StartX && queryX != queryXBegin) ||
            (this->LocalGrid.EndY == this->LocalGrid.StartY && queryY != queryYBegin) ||
            (std::abs(this->LocalGrid.EndX - this->LocalGrid.StartX) <=
                std::abs(queryX - queryXBegin) &&
              std::abs(this->LocalGrid.EndY - this->LocalGrid.StartY) <=
                std::abs(queryY - queryYBegin))))
        {
          this->LocalGrid.StartX = queryXBegin;
          this->LocalGrid.StartY = queryYBegin;
          this->LocalGrid.EndX = queryX;
          this->LocalGrid.EndY = queryY;
          this->LocalGrid.XOrientation = directionX;
          this->LocalGrid.YOrientation = directionY;
          this->LocalGrid.ExtentId = queryExtentId;

          this->Grid.StartX = xBegin;
          this->Grid.StartY = yBegin;
          this->Grid.EndX = x;
          this->Grid.EndY = y;
          this->Grid.XOrientation = sweepDirection[xCornerId];
          this->Grid.YOrientation = sweepDirection[yCornerId];
          this->Grid.ExtentId = extentId;

          if ((this->Dimension == 3 && this->Grid.StartX != this->Grid.EndX &&
                this->Grid.StartY != this->Grid.EndY) ||
            (this->Dimension == 2 &&
              (this->Grid.StartX != this->Grid.EndX || this->Grid.StartY != this->Grid.EndY)) ||
            this->Dimension == 1)
          {
            // In these instances, we know that we found the best connection.
            this->BestConnectionFound = true;
            return true;
          }

          retVal = true;
        }
      }
    }
    return retVal;
  }

  vtkDataArray* Points[6];
  vtkStaticPointLocator* Locator[6];
  int LocalExtentIndex;
  ::ExtentType LocalExtent;
  ::ExtentType Extent[6];
  vtkStaticPointLocator* LocalLocator;
  bool Connected = false;
  bool BestConnectionFound = false;
  ::StructuredGridBlockStructure::Grid2D& Grid;
  ::StructuredGridBlockStructure::Grid2D LocalGrid;
  int Dimension;
};

//----------------------------------------------------------------------------
/**
 * Function to be overloaded for each supported input grid data sets.
 * This function will return true if 2 input block structures are adjacent, false otherwise.
 */
bool SynchronizeGridExtents(::StructuredGridBlockStructure& localBlockStructure,
  ::StructuredGridBlockStructure& blockStructure)
{
  const ::ExtentType& extent = blockStructure.Extent;

  if (localBlockStructure.DataDimension != blockStructure.DataDimension || extent[0] > extent[1] ||
    extent[2] > extent[3] || extent[4] > extent[5])
  {
    return false;
  }
  const ::ExtentType& localExtent = localBlockStructure.Extent;
  const vtkSmartPointer<vtkPoints>* localPoints = localBlockStructure.OuterPointLayers;
  const vtkSmartPointer<vtkPoints>* points = blockStructure.OuterPointLayers;

  // This grid will be set by the structured grid fitting worker if the 2 blocks are connected.
  ::StructuredGridBlockStructure::Grid2D& gridInterface = blockStructure.GridInterface;

  // We need locators to query points inside grids.
  // Locators need `vtkDataSet`, so we create a `vtkPointSet` with the points of each face of the
  // neighboring block.
  vtkNew<vtkStaticPointLocator> locator[6];
  for (int id = 0; id < 6; ++id)
  {
    vtkNew<vtkPointSet> ps;
    ps->SetPoints(points[id]);
    locator[id]->SetDataSet(ps);
    locator[id]->BuildLocator();
  }

  int dimension = (localExtent[0] != localExtent[1]) + (localExtent[2] != localExtent[3]) +
    (localExtent[4] != localExtent[5]);

  using Dispatcher = vtkArrayDispatch::Dispatch;
  StructuredGridFittingWorker worker(points, locator, extent, gridInterface, dimension);

  // We look for a connection until either we tried them all, or if we found the best connection,
  // i.e. a full 2D grid has been found.
  // We iterate on each face of the local block.
  for (int dim = 0; dim < 3 && !worker.BestConnectionFound; ++dim)
  {
    if (localExtent[2 * dim] == localExtent[2 * dim + 1])
    {
      continue;
    }

    for (worker.LocalExtentIndex = 2 * dim;
         !worker.BestConnectionFound && worker.LocalExtentIndex <= 2 * dim + 1;
         ++worker.LocalExtentIndex)
    {
      vtkNew<vtkStaticPointLocator> localLocator;
      vtkNew<vtkPointSet> ps;

      ps->SetPoints(localPoints[worker.LocalExtentIndex]);
      localLocator->SetDataSet(ps);
      localLocator->BuildLocator();

      worker.LocalLocator = localLocator;
      worker.LocalExtent = localExtent;
      worker.LocalExtent[worker.LocalExtentIndex + (worker.LocalExtentIndex % 2 ? -1 : 1)] =
        localExtent[worker.LocalExtentIndex];

      vtkDataArray* localPointsArray = localPoints[worker.LocalExtentIndex]->GetData();

      if (!Dispatcher::Execute(localPointsArray, worker))
      {
        worker(localPointsArray);
      }
    }
  }

  if (!worker.Connected)
  {
    return false;
  }

  const auto& localGrid = worker.LocalGrid;
  int xdim = (localGrid.ExtentId + 2) % 6;
  xdim -= xdim % 2;
  int ydim = (localGrid.ExtentId + 4) % 6;
  ydim -= ydim % 2;

  ::ExtentType& shiftedExtent = blockStructure.ShiftedExtent;

  // We match extents to local extents.
  // We know the intersection already, so we ca just use the local grid interface extent.
  shiftedExtent[xdim] = localGrid.StartX;
  shiftedExtent[xdim + 1] = localGrid.EndX;
  shiftedExtent[ydim] = localGrid.StartY;
  shiftedExtent[ydim + 1] = localGrid.EndY;

  const auto& grid = worker.Grid;

  // Concerning the dimension orthogonal to the interface grid, we just copy the corresponding value
  // from the local extent, and we add the "depth" of the neighbor grid by looking at what is in
  // `extent`.
  int oppositeExtentId = grid.ExtentId + (grid.ExtentId % 2 ? -1 : 1);
  int deltaExtent =
    (localGrid.ExtentId % 2 ? 1 : -1) * std::abs(extent[grid.ExtentId] - extent[oppositeExtentId]);
  shiftedExtent[localGrid.ExtentId + (localGrid.ExtentId % 2 ? -1 : 1)] =
    localExtent[localGrid.ExtentId];
  shiftedExtent[localGrid.ExtentId] = localExtent[localGrid.ExtentId] + deltaExtent;

  int xxdim = (grid.ExtentId + 2) % 6;
  xxdim -= xxdim % 2;
  int yydim = (grid.ExtentId + 4) % 6;
  yydim -= yydim % 2;

  // We want to match 2 adjacent grids that could have dimension x of local grid map dimension z of
  // neighboring grid. For each dimension, 2 cases are to be taken into account:
  // - grids touch on the corner (case A)
  //   In this case, when warping neighbors extent into our referential, one of the neighbor's
  //   extent matches one of ours, and the other is shifted by the width of the neighbor
  // - grids actually overlap (case B)
  //   In this case, we can use the difference between respective StartX of each grid and reposition
  //   it w.r.t. local extent.

  // Dim X
  // case A
  if (localGrid.StartX == localGrid.EndX)
  {
    if (localGrid.StartX == localExtent[xdim])
    {
      shiftedExtent[xdim + 1] = localExtent[xdim];
      shiftedExtent[xdim] = shiftedExtent[xdim + 1] - (extent[xxdim + 1] - extent[xxdim]);
    }
    else
    {
      shiftedExtent[xdim] = localExtent[xdim + 1];
      shiftedExtent[xdim + 1] = shiftedExtent[xdim + 1] + (extent[xxdim + 1] - extent[xxdim]);
    }
  }
  // case B
  else
  {
    shiftedExtent[xdim] = localGrid.StartX - grid.StartX + extent[xdim];
    shiftedExtent[xdim + 1] = shiftedExtent[xdim] + extent[xxdim + 1] - extent[xxdim];
  }

  // Dim Y
  // case A
  if (localGrid.StartY == localGrid.EndY)
  {
    if (localGrid.StartY == localExtent[ydim])
    {
      shiftedExtent[ydim + 1] = localExtent[ydim];
      shiftedExtent[ydim] = shiftedExtent[ydim + 1] - (extent[yydim + 1] - extent[yydim]);
    }
    else
    {
      shiftedExtent[ydim] = localExtent[ydim + 1];
      shiftedExtent[ydim + 1] = shiftedExtent[ydim + 1] + (extent[yydim + 1] - extent[yydim]);
    }
  }
  // case B
  else
  {
    shiftedExtent[ydim] = localGrid.StartY - grid.StartY + extent[ydim];
    shiftedExtent[ydim + 1] = shiftedExtent[ydim] + extent[yydim + 1] - extent[yydim];
  }

  return true;
}

//----------------------------------------------------------------------------
template <class GridDataSetT>
::LinkMap ComputeLinkMapForStructuredData(
  const diy::Master& master, std::vector<GridDataSetT*>& inputs, int outputGhostLevels)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  ::LinkMap linkMap(inputs.size());

  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    // Getting block structures sent by other blocks
    BlockType* block = master.block<BlockType>(localId);
    ::BlockMapType<BlockStructureType>& blockStructures = block->BlockStructures;

    auto& input = inputs[localId];
    const ::ExtentType& localExtent = block->Information.Extent;

    // If I am myself empty, I get rid of everything and skip.
    if (localExtent[0] > localExtent[1] || localExtent[2] > localExtent[3] ||
      localExtent[4] > localExtent[5])
    {
      blockStructures.clear();
      continue;
    }

    int dim = input->GetDataDimension();

    auto& localLinks = linkMap[localId];

    BlockStructureType localBlockStructure(input, block->Information);

    for (auto it = blockStructures.begin(); it != blockStructures.end();)
    {
      BlockStructureType& blockStructure = it->second;

      // We synchronize extents, i.e. we shift the extent of current block neighbor
      // so it is described relative to current block.
      if (!::SynchronizeGridExtents(localBlockStructure, blockStructure))
      {
        // We end up here if extents cannot be fitted together
        it = blockStructures.erase(it);
        continue;
      }

      unsigned char& adjacencyMask = blockStructure.AdjacencyMask;
      unsigned char overlapMask;

      // We compute the adjacency mask and the extent.
      ::ComputeAdjacencyAndOverlapMasks(
        localExtent, blockStructure.ShiftedExtent, adjacencyMask, overlapMask);

      ::ExtentType& neighborShiftedExtentWithNewGhosts = blockStructure.ShiftedExtentWithNewGhosts;
      neighborShiftedExtentWithNewGhosts = blockStructure.ShiftedExtent;

      // We compute the adjacency mask and the extent.
      // We update our neighbor's block extent with ghost layers given spatial adjacency.
      ::LinkGrid<BlockType>(blockStructures, it, block->Information, localLinks, adjacencyMask,
        overlapMask, outputGhostLevels, dim);
    }
  }

  return linkMap;
}

//============================================================================
struct ReplaceDuplicateByHiddenWorker
{
  ReplaceDuplicateByHiddenWorker(
    vtkUnsignedCharArray* inputGhosts, vtkUnsignedCharArray* outputGhosts)
    : InputGhosts(inputGhosts)
    , OutputGhosts(outputGhosts)
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    auto inputGhostsRange = vtk::DataArrayValueRange<1>(this->InputGhosts);
    auto outputGhostsRange = vtk::DataArrayValueRange<1>(this->OutputGhosts);

    using ConstRef = typename decltype(inputGhostsRange)::ConstReferenceType;
    using Ref = typename decltype(outputGhostsRange)::ReferenceType;

    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      ConstRef inputGhost = inputGhostsRange[cellId];
      Ref outputGhost = outputGhostsRange[cellId];

      if (inputGhost & vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL)
      {
        outputGhost = vtkDataSetAttributes::CellGhostTypes::HIDDENCELL;
      }
      else
      {
        outputGhost = inputGhost;
      }
    }
  }

  vtkUnsignedCharArray* InputGhosts;
  vtkUnsignedCharArray* OutputGhosts;
};

//----------------------------------------------------------------------------
template <class PointSetT>
vtkAlgorithm* InstantiateInterfaceExtractor(PointSetT* input);

//----------------------------------------------------------------------------
template <>
vtkAlgorithm* InstantiateInterfaceExtractor<vtkUnstructuredGrid>(vtkUnstructuredGrid* input)
{
  vtkDataSetSurfaceFilter* extractor = vtkDataSetSurfaceFilter::New();
  extractor->SetAllowInterpolation(false);

  // This part is a hack to keep global point ids on the output of the surface filter.
  // It would be too messy to change its behavior, so what we do is we untag the global id
  // array so it gets copied in the output.
  vtkNew<vtkUnstructuredGrid> untaggedGIDInput;
  untaggedGIDInput->ShallowCopy(input);
  auto globalIds = vtkArrayDownCast<vtkIdTypeArray>(input->GetPointData()->GetGlobalIds());
  vtkPointData* untaggedGIDInputPD = untaggedGIDInput->GetPointData();
  untaggedGIDInputPD->SetGlobalIds(nullptr);
  untaggedGIDInputPD->AddArray(globalIds);

  if (vtkUnsignedCharArray* inputGhosts = input->GetCellGhostArray())
  {
    // We create a temporary unstructured grid in which we replace the ghost cell array.
    // Every ghost marked as duplicate is replaced by a ghost marked as hidden.
    vtkNew<vtkUnstructuredGrid> tmp;
    tmp->CopyStructure(untaggedGIDInput);

    vtkIdType numberOfCells = input->GetNumberOfCells();

    vtkCellData* cd = tmp->GetCellData();
    vtkPointData* pd = tmp->GetPointData();
    vtkFieldData* fd = tmp->GetFieldData();

    vtkCellData* inputCD = untaggedGIDInput->GetCellData();

    pd->CopyAllOn();
    pd->ShallowCopy(untaggedGIDInputPD);
    fd->ShallowCopy(untaggedGIDInput->GetFieldData());
    cd->CopyStructure(inputCD);

    for (int arrayId = 0; arrayId < cd->GetNumberOfArrays(); ++arrayId)
    {
      vtkDataArray* inputArray = inputCD->GetArray(arrayId);
      vtkDataArray* outputArray = cd->GetArray(arrayId);
      if (inputGhosts != inputArray)
      {
        outputArray->ShallowCopy(inputArray);
      }
      else
      {
        outputArray->SetNumberOfTuples(numberOfCells);
      }
    }

    ::ReplaceDuplicateByHiddenWorker worker(inputGhosts, tmp->GetCellGhostArray());

    vtkSMPTools::For(0, numberOfCells, worker);

    extractor->SetInputData(tmp);
  }
  else
  {
    extractor->SetInputData(untaggedGIDInput);
  }

  return extractor;
}

//----------------------------------------------------------------------------
template <>
vtkAlgorithm* InstantiateInterfaceExtractor<vtkPolyData>(vtkPolyData* input)
{
  vtkFeatureEdges* extractor = vtkFeatureEdges::New();
  extractor->BoundaryEdgesOn();
  extractor->FeatureEdgesOff();
  extractor->NonManifoldEdgesOff();
  extractor->PassLinesOn();
  extractor->ColoringOff();
  extractor->RemoveGhostInterfacesOff();
  extractor->SetInputData(input);

  return extractor;
}

//============================================================================
template <class ArrayT>
struct ComputeConnectivitySizeWorker
{
  ComputeConnectivitySizeWorker(ArrayT* offsets, vtkUnsignedCharArray* ghostCells)
    : Offsets(offsets)
    , GhostCells(ghostCells)
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    vtkIdType& size = this->Size.Local();
    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      if (!(this->GhostCells->GetValue(cellId) & ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA))
      {
        size += this->Offsets->GetValue(cellId + 1) - this->Offsets->GetValue(cellId);
      }
    }
  }

  void Initialize() { this->Size.Local() = 0; }

  void Reduce()
  {
    for (const vtkIdType& size : this->Size)
    {
      this->TotalSize += size;
    }
  }

  ArrayT* Offsets;
  vtkUnsignedCharArray* GhostCells;
  vtkSMPThreadLocal<vtkIdType> Size;
  vtkIdType TotalSize = 0;
};

//============================================================================
template <int MaskT>
struct ComputePolyDataConnectivitySizeWorker
{
  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  using VertArrayType = typename std::conditional<(MaskT & 1) != 0, ArrayType64, ArrayType32>::type;
  using LineArrayType = typename std::conditional<(MaskT & 2) != 0, ArrayType64, ArrayType32>::type;
  using PolyArrayType = typename std::conditional<(MaskT & 4) != 0, ArrayType64, ArrayType32>::type;
  using StripArrayType =
    typename std::conditional<(MaskT & 8) != 0, ArrayType64, ArrayType32>::type;

  ComputePolyDataConnectivitySizeWorker(vtkPolyData* input)
    : Input(input)
    , VertOffsets(vtkArrayDownCast<VertArrayType>(input->GetVerts()->GetOffsetsArray()))
    , LineOffsets(vtkArrayDownCast<LineArrayType>(input->GetLines()->GetOffsetsArray()))
    , PolyOffsets(vtkArrayDownCast<PolyArrayType>(input->GetPolys()->GetOffsetsArray()))
    , StripOffsets(vtkArrayDownCast<StripArrayType>(input->GetStrips()->GetOffsetsArray()))
    , GhostCells(input->GetCellGhostArray())
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    vtkIdType& vertsSize = this->VertsSize.Local();
    vtkIdType& linesSize = this->LinesSize.Local();
    vtkIdType& polysSize = this->PolysSize.Local();
    vtkIdType& stripsSize = this->StripsSize.Local();

    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      if (this->GhostCells->GetValue(cellId) & ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA)
      {
        continue;
      }
      switch (this->Input->GetCellType(cellId))
      {
        case VTK_EMPTY_CELL:
          break;
        case VTK_VERTEX:
        case VTK_POLY_VERTEX:
        {
          vtkIdType vertId = this->Input->GetCellIdRelativeToCellArray(cellId);
          vertsSize +=
            this->VertOffsets->GetValue(vertId + 1) - this->VertOffsets->GetValue(vertId);
          break;
        }
        case VTK_LINE:
        case VTK_POLY_LINE:
        {
          vtkIdType lineId = this->Input->GetCellIdRelativeToCellArray(cellId);
          linesSize +=
            this->LineOffsets->GetValue(lineId + 1) - this->LineOffsets->GetValue(lineId);
          break;
        }
        case VTK_TRIANGLE:
        case VTK_QUAD:
        case VTK_POLYGON:
        {
          vtkIdType polyId = this->Input->GetCellIdRelativeToCellArray(cellId);
          polysSize +=
            this->PolyOffsets->GetValue(polyId + 1) - this->PolyOffsets->GetValue(polyId);
          break;
        }
        case VTK_TRIANGLE_STRIP:
        {
          vtkIdType stripId = this->Input->GetCellIdRelativeToCellArray(cellId);
          stripsSize +=
            this->StripOffsets->GetValue(stripId + 1) - this->StripOffsets->GetValue(stripId);
          break;
        }
        default:
          vtkLog(ERROR, "Input cell at id " << cellId << " in poly data is not supported.");
          break;
      }
    }
  }

  void Initialize()
  {
    this->VertsSize.Local() = 0;
    this->LinesSize.Local() = 0;
    this->PolysSize.Local() = 0;
    this->StripsSize.Local() = 0;
  }

  void Reduce()
  {
    for (const vtkIdType& size : this->VertsSize)
    {
      this->TotalVertsSize += size;
    }
    for (const vtkIdType& size : this->LinesSize)
    {
      this->TotalLinesSize += size;
    }
    for (const vtkIdType& size : this->PolysSize)
    {
      this->TotalPolysSize += size;
    }
    for (const vtkIdType& size : this->StripsSize)
    {
      this->TotalStripsSize += size;
    }
  }

  vtkPolyData* Input;
  VertArrayType* VertOffsets;
  LineArrayType* LineOffsets;
  PolyArrayType* PolyOffsets;
  StripArrayType* StripOffsets;
  vtkUnsignedCharArray* GhostCells;

  vtkSMPThreadLocal<vtkIdType> VertsSize;
  vtkSMPThreadLocal<vtkIdType> LinesSize;
  vtkSMPThreadLocal<vtkIdType> PolysSize;
  vtkSMPThreadLocal<vtkIdType> StripsSize;

  vtkIdType TotalVertsSize = 0;
  vtkIdType TotalLinesSize = 0;
  vtkIdType TotalPolysSize = 0;
  vtkIdType TotalStripsSize = 0;
};

//----------------------------------------------------------------------------
// Helper for ComputeFacesSizeWorker and UpdateCellBufferSize
namespace
{
struct GetPolyhedronNPts
{
  // Insert full cell
  template <typename CellStateT>
  vtkIdType operator()(CellStateT& state, const vtkIdType cellId, const vtkCellArray* faces)
  {
    vtkIdType npts = 0;

    const vtkIdType beginOffset = state.GetBeginOffset(cellId);
    const vtkIdType endOffset = state.GetEndOffset(cellId);
    const vtkIdType NumberOfFaces = endOffset - beginOffset;
    const auto cellFaces = state.GetConnectivity()->GetPointer(beginOffset);

    for (vtkIdType faceNum = 0; faceNum < NumberOfFaces; ++faceNum)
    {
      npts += faces->GetCellSize(static_cast<vtkIdType>(cellFaces[faceNum]));
    }
    return npts;
  }
};
}

//============================================================================
struct ComputeFacesSizeWorker
{
  ComputeFacesSizeWorker(
    vtkCellArray* faces, vtkCellArray* faceLocations, vtkUnsignedCharArray* ghostCells)
    : Faces(faces)
    , FaceLocations(faceLocations)
    , GhostCells(ghostCells)
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    vtkIdType& size = this->Size.Local();
    vtkIdType& facenum = this->FaceNum.Local();

    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      if (!(this->GhostCells->GetValue(cellId) & ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA))
      {
        vtkIdType id = this->FaceLocations->GetCellSize(cellId);
        if (id != 0)
        {
          facenum += this->FaceLocations->GetCellSize(cellId);
          size += this->FaceLocations->Visit(GetPolyhedronNPts{}, cellId, this->Faces);
        }
      }
    }
  }

  void Initialize()
  {
    this->Size.Local() = 0;
    this->FaceNum.Local() = 0;
  }

  void Reduce()
  {
    for (const vtkIdType& size : this->Size)
    {
      this->TotalSize += size;
    }
    for (const vtkIdType& size : this->FaceNum)
    {
      this->TotalFaceNum += size;
    }
  }

  vtkCellArray* Faces;
  vtkCellArray* FaceLocations;
  vtkUnsignedCharArray* GhostCells;

  vtkSMPThreadLocal<vtkIdType> Size;
  vtkSMPThreadLocal<vtkIdType> FaceNum;
  vtkIdType TotalSize = 0;
  vtkIdType TotalFaceNum = 0;
};

#define ComputePolyDataConnectivitySizeWorkerMacro(mask)                                           \
  case mask:                                                                                       \
  {                                                                                                \
    ::ComputePolyDataConnectivitySizeWorker<mask> worker(input);                                   \
    vtkSMPTools::For(0, input->GetNumberOfCells(), worker);                                        \
    info.InputVertConnectivitySize = worker.TotalVertsSize;                                        \
    info.InputLineConnectivitySize = worker.TotalLinesSize;                                        \
    info.InputPolyConnectivitySize = worker.TotalPolysSize;                                        \
    info.InputStripConnectivitySize = worker.TotalStripsSize;                                      \
    break;                                                                                         \
  }

//----------------------------------------------------------------------------
void InitializeInformationIdsForUnstructuredData(vtkPolyData* input, ::PolyDataInformation& info)
{
  if (input->GetCellGhostArray())
  {
    vtkIdList* cellIds = info.OutputToInputCellIdRedirectionMap;
    vtkIdList* vertIds = info.OutputToInputVertCellIdRedirectionMap;
    vtkIdList* lineIds = info.OutputToInputLineCellIdRedirectionMap;
    vtkIdList* polyIds = info.OutputToInputPolyCellIdRedirectionMap;
    vtkIdList* stripIds = info.OutputToInputStripCellIdRedirectionMap;

    vertIds->Allocate(input->GetNumberOfVerts());
    lineIds->Allocate(input->GetNumberOfVerts());
    polyIds->Allocate(input->GetNumberOfVerts());
    stripIds->Allocate(input->GetNumberOfVerts());

    for (vtkIdType id = 0; id < cellIds->GetNumberOfIds(); ++id)
    {
      vtkIdType cellId = cellIds->GetId(id);
      switch (input->GetCellType(cellId))
      {
        case VTK_EMPTY_CELL:
          break;
        case VTK_VERTEX:
        case VTK_POLY_VERTEX:
          vertIds->InsertNextId(input->GetCellIdRelativeToCellArray(cellId));
          break;
        case VTK_LINE:
        case VTK_POLY_LINE:
          lineIds->InsertNextId(input->GetCellIdRelativeToCellArray(cellId));
          break;
        case VTK_TRIANGLE:
        case VTK_QUAD:
        case VTK_POLYGON:
          polyIds->InsertNextId(input->GetCellIdRelativeToCellArray(cellId));
          break;
        case VTK_TRIANGLE_STRIP:
          stripIds->InsertNextId(input->GetCellIdRelativeToCellArray(cellId));
          break;
        default:
          vtkLog(ERROR, "An input vtkPolyData holds a cell that is not supported.");
          break;
      }
    }

    info.NumberOfInputVerts = vertIds->GetNumberOfIds();
    info.NumberOfInputPolys = polyIds->GetNumberOfIds();
    info.NumberOfInputStrips = stripIds->GetNumberOfIds();
    info.NumberOfInputLines = lineIds->GetNumberOfIds();

    vtkCellArray* verts = input->GetVerts();
    vtkCellArray* lines = input->GetLines();
    vtkCellArray* polys = input->GetPolys();
    vtkCellArray* strips = input->GetStrips();

    int mask = static_cast<int>(verts->IsStorage64Bit()) | (lines->IsStorage64Bit() << 1) |
      (polys->IsStorage64Bit() << 2) | (strips->IsStorage64Bit() << 3);

    switch (mask)
    {
      ComputePolyDataConnectivitySizeWorkerMacro(0) ComputePolyDataConnectivitySizeWorkerMacro(1)
        ComputePolyDataConnectivitySizeWorkerMacro(2) ComputePolyDataConnectivitySizeWorkerMacro(3)
          ComputePolyDataConnectivitySizeWorkerMacro(4)
            ComputePolyDataConnectivitySizeWorkerMacro(5)
              ComputePolyDataConnectivitySizeWorkerMacro(6)
                ComputePolyDataConnectivitySizeWorkerMacro(7)
                  ComputePolyDataConnectivitySizeWorkerMacro(8)
                    ComputePolyDataConnectivitySizeWorkerMacro(9)
                      ComputePolyDataConnectivitySizeWorkerMacro(10)
                        ComputePolyDataConnectivitySizeWorkerMacro(11)
                          ComputePolyDataConnectivitySizeWorkerMacro(12)
                            ComputePolyDataConnectivitySizeWorkerMacro(13)
                              ComputePolyDataConnectivitySizeWorkerMacro(14)
                                ComputePolyDataConnectivitySizeWorkerMacro(15)
    }
  }
  else
  {
    info.NumberOfInputVerts = input->GetNumberOfVerts();
    info.NumberOfInputPolys = input->GetNumberOfPolys();
    info.NumberOfInputStrips = input->GetNumberOfStrips();
    info.NumberOfInputLines = input->GetNumberOfLines();

    info.InputVertConnectivitySize = input->GetVerts()->GetConnectivityArray()->GetNumberOfTuples();
    info.InputLineConnectivitySize = input->GetLines()->GetConnectivityArray()->GetNumberOfTuples();
    info.InputPolyConnectivitySize = input->GetPolys()->GetConnectivityArray()->GetNumberOfTuples();
    info.InputStripConnectivitySize =
      input->GetStrips()->GetConnectivityArray()->GetNumberOfTuples();
  }

  // These variables are used when adding points from neighboring blocks.
  // After points are added from a block b, these indices must be incremented by the number of
  // points added by this block, so we know where we left off for the following block.
  info.CurrentMaxPointId = info.NumberOfInputPoints;
  info.CurrentMaxCellId = info.NumberOfInputCells;

  info.CurrentMaxPolyId = info.NumberOfInputPolys;
  info.CurrentMaxStripId = info.NumberOfInputStrips;
  info.CurrentMaxLineId = info.NumberOfInputLines;

  info.CurrentPolyConnectivitySize = info.InputPolyConnectivitySize;
  info.CurrentStripConnectivitySize = info.InputStripConnectivitySize;
  info.CurrentLineConnectivitySize = info.InputLineConnectivitySize;
}

#undef ComputePolyDataConnectivitySizeWorkerMacro

//----------------------------------------------------------------------------
void InitializeInformationIdsForUnstructuredData(
  vtkUnstructuredGrid* input, ::UnstructuredGridInformation& info)
{
  // These variables are used when adding points from neighboring blocks.
  // After points are added from a block b, these indices must be incremented by the number of
  // points added by this block, so we know where we left off for the following block.
  info.CurrentMaxPointId = info.NumberOfInputPoints;
  info.CurrentMaxCellId = info.NumberOfInputCells;

  vtkCellArray* cells = input->GetCells();

  if (!cells)
  {
    return;
  }

  if (vtkUnsignedCharArray* ghosts = input->GetCellGhostArray())
  {
    using ArrayType32 = vtkCellArray::ArrayType32;
    using ArrayType64 = vtkCellArray::ArrayType64;

    vtkIdType numberOfCells = input->GetNumberOfCells();

    if (cells->IsStorage64Bit())
    {
      ::ComputeConnectivitySizeWorker<ArrayType64> worker(
        vtkArrayDownCast<ArrayType64>(cells->GetOffsetsArray()), ghosts);
      vtkSMPTools::For(0, input->GetNumberOfCells(), worker);

      info.InputConnectivitySize = worker.TotalSize;
    }
    else
    {
      ::ComputeConnectivitySizeWorker<ArrayType32> worker(
        vtkArrayDownCast<ArrayType32>(cells->GetOffsetsArray()), ghosts);
      vtkSMPTools::For(0, numberOfCells, worker);

      info.InputConnectivitySize = worker.TotalSize;
    }

    vtkCellArray* faceLocations = input->GetPolyhedronFaceLocations();
    vtkCellArray* faces = input->GetPolyhedronFaces();

    if (faceLocations && faceLocations->GetNumberOfCells() && faces && faces->GetNumberOfCells())
    {
      ::ComputeFacesSizeWorker worker(faces, faceLocations, ghosts);
      vtkSMPTools::For(0, numberOfCells, worker);

      info.InputFacesSize = worker.TotalSize;
      info.InputNumberOfFaces = worker.TotalFaceNum;
    }
  }
  else
  {
    info.InputConnectivitySize = cells->GetConnectivityArray()->GetNumberOfTuples();
    info.InputFacesSize = input->GetPolyhedronFaces()
      ? input->GetPolyhedronFaces()->GetConnectivityArray()->GetNumberOfTuples()
      : 0;
    info.InputNumberOfFaces = input->GetPolyhedronFaces()
      ? input->GetPolyhedronFaces()->GetOffsetsArray()->GetNumberOfTuples() - 1
      : 0;
  }

  info.CurrentConnectivitySize = info.InputConnectivitySize;
  info.CurrentFacesSize = info.InputFacesSize;
  info.CurrentMaxFaceId = info.InputNumberOfFaces;
}

//----------------------------------------------------------------------------
template <class StructuredDataSetT>
void InitializeBlocksForStructuredData(
  diy::Master& master, std::vector<StructuredDataSetT*>& inputs)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<StructuredDataSetT>::BlockType;
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    StructuredDataSetT* input = inputs[localId];
    auto& info = master.block<BlockType>(localId)->Information;
    info.Input = input;
    const int* extent = input->GetExtent();
    info.InputExtent = { extent[0], extent[1], extent[2], extent[3], extent[4], extent[5] };
  }
}

//----------------------------------------------------------------------------
template <class PointSetT>
void InitializeBlocksForUnstructuredData(diy::Master& master, std::vector<PointSetT*>& inputs)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType;
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    PointSetT* input = inputs[localId];
    BlockType* block = master.block<BlockType>(localId);
    typename BlockType::InformationType& information = block->Information;
    information.BoundingBox = vtkBoundingBox(input->GetBounds());

    information.Input = input;

    if (vtkUnsignedCharArray* ghostCells = input->GetCellGhostArray())
    {
      vtkIdType numberOfInputPoints = input->GetNumberOfPoints();
      vtkIdType numberOfInputCells = input->GetNumberOfCells();

      // We start by remapping ghost points.
      vtkSmartPointer<vtkIdList>& pointIdMap = information.OutputToInputPointIdRedirectionMap;
      pointIdMap = vtkSmartPointer<vtkIdList>::New();
      pointIdMap->Allocate(numberOfInputPoints);

      vtkSmartPointer<vtkIdList>& pointIdInverseMap =
        information.InputToOutputPointIdRedirectionMap;
      pointIdInverseMap = vtkSmartPointer<vtkIdList>::New();
      pointIdInverseMap->SetNumberOfIds(numberOfInputPoints);
      // We set -1 where input id to input id doesn't map anywhere in the output. This happens
      // for points that are only belonging exclusively to ghost cells.
      pointIdInverseMap->Fill(-1);

      vtkNew<vtkIdList> ids;
      auto ghosts = vtk::DataArrayValueRange<1>(ghostCells);

      for (vtkIdType pointId = 0; pointId < numberOfInputPoints; ++pointId)
      {
        input->GetPointCells(pointId, ids);
        for (vtkIdType id = 0; id < ids->GetNumberOfIds(); ++id)
        {
          // We are adjacent to a non-ghost cell: keep this point
          if (!(ghosts[ids->GetId(id)] & ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA))
          {
            pointIdInverseMap->SetId(pointId, pointIdMap->GetNumberOfIds());
            pointIdMap->InsertNextId(pointId);
            break;
          }
        }
      }

      information.NumberOfInputPoints = pointIdMap->GetNumberOfIds();

      vtkSmartPointer<vtkIdList>& cellIdMap = information.OutputToInputCellIdRedirectionMap;
      cellIdMap = vtkSmartPointer<vtkIdList>::New();
      cellIdMap->Allocate(numberOfInputCells);

      for (vtkIdType cellId = 0; cellId < numberOfInputCells; ++cellId)
      {
        if (!(ghosts[cellId] & ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA))
        {
          cellIdMap->InsertNextId(cellId);
        }
      }

      information.NumberOfInputCells = cellIdMap->GetNumberOfIds();
    }
    else
    {
      information.NumberOfInputPoints = input->GetNumberOfPoints();
      information.NumberOfInputCells = input->GetNumberOfCells();
    }

    // We tag points with a local id, then we extract the crust of the input.
    vtkNew<vtkIdTypeArray> pointIds;
    pointIds->SetName(LOCAL_POINT_IDS_ARRAY_NAME);
    pointIds->SetNumberOfComponents(1);
    pointIds->SetNumberOfTuples(input->GetNumberOfPoints());
    auto pointIdsRange = vtk::DataArrayValueRange<1>(pointIds);
    // FIXME Ideally, this should be done with an implicit array
    std::iota(pointIdsRange.begin(), pointIdsRange.end(), 0);

    vtkNew<PointSetT> inputWithLocalPointIds;
    inputWithLocalPointIds->ShallowCopy(input);
    inputWithLocalPointIds->GetPointData()->AddArray(pointIds);

    information.InterfaceExtractor = vtkSmartPointer<vtkAlgorithm>::Take(
      ::InstantiateInterfaceExtractor<PointSetT>(inputWithLocalPointIds));

    vtkAlgorithm* interfaceFilter = information.InterfaceExtractor;
    interfaceFilter->Update();

    vtkPointSet* surface = vtkPointSet::SafeDownCast(interfaceFilter->GetOutputDataObject(0));

    information.InterfacePoints =
      surface->GetNumberOfPoints() ? surface->GetPoints()->GetData() : nullptr;
    information.InterfacePointIds = vtkArrayDownCast<vtkIdTypeArray>(
      surface->GetPointData()->GetAbstractArray(LOCAL_POINT_IDS_ARRAY_NAME));

    auto inputGlobalPointIds =
      vtkArrayDownCast<vtkIdTypeArray>(input->GetPointData()->GetGlobalIds());

    information.InterfaceGlobalPointIds = inputGlobalPointIds
      ? vtkArrayDownCast<vtkIdTypeArray>(
          surface->GetPointData()->GetAbstractArray(inputGlobalPointIds->GetName()))
      : nullptr;

    ::InitializeInformationIdsForUnstructuredData(input, information);
  }
}

//============================================================================
/**
 * This functor extracts point ids of the source that match points in the target.
 * 2 outputs are produced:
 * - The matching point ids in the source that are sorted in the same order as points appear in the
 *   source, in MatchingSourcePointIds
 * - Those same point ids, but sorted in the same order as points appear in the target, in
 *   RemappedMatchingReceivedPointIdsSortedLikeTarget. If the input had ghosts and points need to be
 *   remapped from input to output, the remapping is already done in this array, i.e. one can query
 *   points in the output, but not in the input using this array.
 */
struct MatchingPointExtractor
{
  MatchingPointExtractor(vtkIdTypeArray* sourcePointIds, vtkPointSet* surface,
    vtkDataArray* sourcePoints, vtkIdTypeArray* sourceGlobalPointIds, vtkIdList* pointIdMap)
    : SourcePointIds(sourcePointIds)
    , SourcePoints(sourcePoints)
    , InputToOutputPointIdMap(pointIdMap)
  {
    if (sourceGlobalPointIds)
    {
      auto gidRange = vtk::DataArrayValueRange<1>(sourceGlobalPointIds);
      using ConstRef = typename decltype(gidRange)::ConstReferenceType;

      for (ConstRef gid : gidRange)
      {
        this->SourceGlobalPointIds.insert({ gid, this->SourceGlobalPointIds.size() });
      }
    }
    else
    {
      // We only use the locator if global point ids are not present.
      this->KdTree->BuildLocatorFromPoints(surface->GetPoints());
    }
  }

  template <class PointArrayT>
  void operator()(PointArrayT* points, vtkIdTypeArray* globalPointIds)
  {
    std::vector<vtkIdType> inverseMap;
    auto sourcePointIdsRange = vtk::DataArrayValueRange<1>(this->SourcePointIds);

    if (globalPointIds)
    {
      auto gidRange = vtk::DataArrayValueRange<1>(globalPointIds);

      inverseMap.reserve(gidRange.size());
      this->MatchingSourcePointIds->Allocate(gidRange.size());

      using ConstRef = typename decltype(gidRange)::ConstReferenceType;

      for (ConstRef gid : gidRange)
      {
        auto it = this->SourceGlobalPointIds.find(gid);
        if (it != this->SourceGlobalPointIds.end())
        {
          vtkIdType& matchingPointId = it->second;
          this->MatchingSourcePointIds->InsertNextId(sourcePointIdsRange[matchingPointId]);
          inverseMap.push_back(matchingPointId);
        }
      }
    }
    else
    {
      auto pointsRange = vtk::DataArrayTupleRange<3>(points);

      inverseMap.reserve(pointsRange.size());
      this->MatchingSourcePointIds->Allocate(pointsRange.size());

      using ConstPointRef = typename decltype(pointsRange)::ConstTupleReferenceType;
      using ValueType = typename ConstPointRef::value_type;
      double p[3];
      double dist2;

      for (ConstPointRef point : pointsRange)
      {
        vtkMath::Assign(point, p);
        vtkIdType closestPointId = this->KdTree->FindClosestPointWithinRadius(
          vtkDIYGhostUtilities_detail::ComputePrecision<ValueType>(
            std::max({ std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2]) })),

          p, dist2);

        if (closestPointId == -1)
        {
          continue;
        }

        this->MatchingSourcePointIds->InsertNextId(sourcePointIdsRange[closestPointId]);
        inverseMap.push_back(closestPointId);
      }
    }

    this->RemappedMatchingReceivedPointIdsSortedLikeTarget->Allocate(inverseMap.size());
    std::sort(inverseMap.begin(), inverseMap.end());

    if (this->InputToOutputPointIdMap)
    {
      for (const vtkIdType& id : inverseMap)
      {
        this->RemappedMatchingReceivedPointIdsSortedLikeTarget->InsertNextId(
          this->InputToOutputPointIdMap->GetId(sourcePointIdsRange[id]));
      }
    }
    else
    {
      for (const vtkIdType& id : inverseMap)
      {
        this->RemappedMatchingReceivedPointIdsSortedLikeTarget->InsertNextId(
          sourcePointIdsRange[id]);
      }
    }
  }

  // Inputs
  vtkIdTypeArray* SourcePointIds;
  vtkNew<vtkKdTree> KdTree;
  vtkDataArray* SourcePoints;
  std::unordered_map<vtkIdType, vtkIdType> SourceGlobalPointIds;
  vtkIdList* InputToOutputPointIdMap;

  // Outputs
  vtkIdList* MatchingSourcePointIds;
  vtkIdList* RemappedMatchingReceivedPointIdsSortedLikeTarget;
};

//----------------------------------------------------------------------------
template <class InputArrayT, class OutputArrayT>
void FillConnectivityAndOffsetsArrays(vtkCellArray* inputCells, vtkCellArray* outputCells,
  const std::map<vtkIdType, vtkIdType>& seedPointIdsToSendWithIndex,
  const std::map<vtkIdType, vtkIdType>& pointIdsToSendWithIndex, vtkIdList* cellIdsToSend)
{
  vtkIdType currentConnectivitySize = 0;
  InputArrayT* inputOffsets = vtkArrayDownCast<InputArrayT>(inputCells->GetOffsetsArray());
  InputArrayT* inputConnectivity =
    vtkArrayDownCast<InputArrayT>(inputCells->GetConnectivityArray());
  OutputArrayT* outputOffsets = vtkArrayDownCast<OutputArrayT>(outputCells->GetOffsetsArray());
  OutputArrayT* outputConnectivity =
    vtkArrayDownCast<OutputArrayT>(outputCells->GetConnectivityArray());

  auto connectivityRange = vtk::DataArrayValueRange<1>(outputConnectivity);

  vtkIdType outputId = 0;

  for (vtkIdType id = 0; id < cellIdsToSend->GetNumberOfIds(); ++id)
  {
    vtkIdType cellId = cellIdsToSend->GetId(id);
    vtkIdType inputOffset = inputOffsets->GetValue(cellId);
    outputOffsets->SetValue(outputId, currentConnectivitySize);

    vtkIdType nextOffset =
      currentConnectivitySize + inputOffsets->GetValue(cellId + 1) - inputOffset;

    vtkIdType counter = 0;
    for (vtkIdType offset = outputOffsets->GetValue(outputId); offset < nextOffset;
         ++offset, ++counter)
    {
      vtkIdType pointId = inputConnectivity->GetValue(inputOffset + counter);
      auto it = pointIdsToSendWithIndex.find(pointId);
      // We will find a valid it of the point of id pointId is not on the interface between us
      // and the current connected block
      if (it != pointIdsToSendWithIndex.end())
      {
        connectivityRange[offset] = it->second;
      }
      else
      {
        // We put a negative id here to tell the block who will receive this
        // that this point is part of the interfacing points: the neighboring block already owns a
        // copy of this point.
        connectivityRange[offset] = -seedPointIdsToSendWithIndex.at(pointId);
      }
    }

    currentConnectivitySize = nextOffset;
    ++outputId;
  }

  // If there has been no offset added, it means that no cells are to send, so we should not
  // add the last theoretical offset.
  if (cellIdsToSend->GetNumberOfIds())
  {
    outputOffsets->SetValue(cellIdsToSend->GetNumberOfIds(), connectivityRange.size());
  }
}

//============================================================================
template <class InputArrayT, class OutputArrayT, class PointSetT>
struct FillUnstructuredDataTopologyBufferFunctor;

//============================================================================
template <class InputArrayT, class OutputArrayT>
struct FillUnstructuredDataTopologyBufferFunctor<InputArrayT, OutputArrayT, vtkUnstructuredGrid>
{
  using BlockStructureType = ::UnstructuredGridBlockStructure;

  /**
   * This function will fill the buffers describing the geometry to send to a connected block.
   * Inputs:
   * - seedPointIdsToSendWithIndex: Points interfacing the neighboring block. These are being used
   * to tell the neighboring block which points in the geometry buffer being sent are already
   * present there (the block already has a copy because those are the points that interface the 2
   * blocks). We tag them with a negative sign and the position of this point in the buffer we
   * already sent to the block when exchanging interfaces to see who's connected to who. The
   * neighboring block will use this index to retrieve which point we are talking about (this is
   * retrieved with RemappedMatchingReceivedPointIdsSortedLikeTarget in MatchingPointExtractor).
   * - pointIdsToSendWithIndex: Every point ids, besides the one interfacing the current connected
   *   block, that we need to send, with their index in the point buffer we will send.
   */
  static void Fill(const std::map<vtkIdType, vtkIdType>& seedPointIdsToSendWithIndex,
    const std::map<vtkIdType, vtkIdType>& pointIdsToSendWithIndex,
    BlockStructureType& blockStructure, vtkUnstructuredGrid* input)
  {
    auto& buffer = blockStructure.SendBuffer;

    vtkCellArray* cellArray = buffer.CellArray;
    OutputArrayT* connectivity = vtkArrayDownCast<OutputArrayT>(cellArray->GetConnectivityArray());
    OutputArrayT* offsets = vtkArrayDownCast<OutputArrayT>(cellArray->GetOffsetsArray());
    vtkNew<vtkUnsignedCharArray> types;
    buffer.Types = types;

    vtkIdList* cellIdsToSend = blockStructure.CellIdsToSend;
    vtkIdType numberOfCellsToSend = cellIdsToSend->GetNumberOfIds();

    connectivity->SetNumberOfValues(blockStructure.ConnectivitySize);
    offsets->SetNumberOfValues(numberOfCellsToSend + 1);
    types->SetNumberOfValues(numberOfCellsToSend);

    vtkCellArray* inputCellArray = input->GetCells();
    vtkIdType outputId = 0;

    vtkCellArray* inputFaces = input->GetPolyhedronFaces();
    vtkCellArray* inputFaceLocations = input->GetPolyhedronFaceLocations();

    // faces and faceLocations deal with VTK_POLYHEDRON. If there are VTK_POLYHEDRON cells in the
    // input, we instantiate those arrays for our buffers.
    if (inputFaces && inputFaces->GetNumberOfCells())
    {
      buffer.Faces = vtkSmartPointer<vtkCellArray>::New();
      buffer.Faces->AllocateExact(blockStructure.FacesNum, blockStructure.FacesSize);
      buffer.FaceLocations = vtkSmartPointer<vtkCellArray>::New();
      buffer.FaceLocations->AllocateExact(numberOfCellsToSend, blockStructure.FacesNum);
    }

    vtkCellArray* faces = buffer.Faces;
    vtkCellArray* faceLocations = buffer.FaceLocations;

    vtkIdType currentFacesId = 0;

    ::FillConnectivityAndOffsetsArrays<InputArrayT, OutputArrayT>(inputCellArray, cellArray,
      seedPointIdsToSendWithIndex, pointIdsToSendWithIndex, cellIdsToSend);

    for (vtkIdType i = 0; i < numberOfCellsToSend; ++i)
    {
      vtkIdType cellId = cellIdsToSend->GetId(i);
      int cellType = input->GetCellType(cellId);

      if (cellType == VTK_POLYHEDRON)
      {
        vtkIdType numberOfFaces = 0;
        vtkIdType const* faceIds;
        vtkNew<vtkIdList> faceTmp;
        vtkNew<vtkIdList> ptsTmp;
        inputFaceLocations->GetCellAtId(cellId, numberOfFaces, faceIds, faceTmp);
        faceLocations->InsertNextCell(numberOfFaces);
        for (vtkIdType faceId = 0; faceId < numberOfFaces; ++faceId)
        {
          vtkIdType numberOfPoints;
          vtkIdType const* facePointIds;
          inputFaces->GetCellAtId(faceIds[faceId], numberOfPoints, facePointIds, ptsTmp);
          faceLocations->InsertCellPoint(currentFacesId++);
          faces->InsertNextCell(numberOfPoints);
          for (vtkIdType facePointId = 0; facePointId < numberOfPoints; ++facePointId)
          {
            vtkIdType pointId = facePointIds[facePointId];
            auto it = pointIdsToSendWithIndex.find(pointId);
            // We will find a valid it of the point of id pointId is not on the interface between us
            // and the current connected block
            if (it != pointIdsToSendWithIndex.end())
            {
              faces->InsertCellPoint(it->second);
            }
            else
            {
              // We put a negative id here to tell the block who will receive this
              // that this point is part of the interfacing points: the neighboring block already
              // owns a copy of this point.
              faces->InsertCellPoint(-seedPointIdsToSendWithIndex.at(pointId));
            }
          }
        }
      }
      else if (faceLocations)
      {
        faceLocations->InsertNextCell(0);
      }
      types->SetValue(outputId++, cellType);
    }
  }
};

//============================================================================
template <class InputArrayT, class OutputArrayT>
struct FillUnstructuredDataTopologyBufferFunctor<InputArrayT, OutputArrayT, vtkPolyData>
{
  /**
   * See version of FillUnstructuredDataTopologyBuffer for vtkUnstructuredGrid for explanation
   */
  static void Fill(const std::map<vtkIdType, vtkIdType>& seedPointIdsToSendWithIndex,
    const std::map<vtkIdType, vtkIdType>& pointIdsToSendWithIndex, vtkCellArray* inputCells,
    vtkCellArray* cells, vtkIdList* cellIdsToSend, vtkIdType connectivitySize)
  {
    OutputArrayT* connectivity = vtkArrayDownCast<OutputArrayT>(cells->GetConnectivityArray());
    OutputArrayT* offsets = vtkArrayDownCast<OutputArrayT>(cells->GetOffsetsArray());

    connectivity->SetNumberOfValues(connectivitySize);

    vtkIdType numberOfCellsToSend = cellIdsToSend->GetNumberOfIds();
    offsets->SetNumberOfValues(numberOfCellsToSend ? numberOfCellsToSend + 1 : 0);

    ::FillConnectivityAndOffsetsArrays<InputArrayT, OutputArrayT>(
      inputCells, cells, seedPointIdsToSendWithIndex, pointIdsToSendWithIndex, cellIdsToSend);
  }
};

//----------------------------------------------------------------------------
void CopyCellIdsToSendIntoBlockStructure(
  const std::set<vtkIdType>& cellIdsToSend, ::UnstructuredDataBlockStructure& blockStructure)
{
  blockStructure.CellIdsToSend->SetNumberOfIds(cellIdsToSend.size());
  vtkSMPTools::Transform(cellIdsToSend.cbegin(), cellIdsToSend.cend(),
    blockStructure.CellIdsToSend->begin(), [](vtkIdType cellId) -> vtkIdType { return cellId; });
}

//----------------------------------------------------------------------------
template <class PointSetT>
void CopyCellIdsToSendIntoBlockStructure(PointSetT* input, const std::set<vtkIdType>& cellIdsToSend,
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType ::BlockStructureType&
    blockStructure);

//----------------------------------------------------------------------------
template <>
void CopyCellIdsToSendIntoBlockStructure(vtkUnstructuredGrid* vtkNotUsed(input),
  const std::set<vtkIdType>& cellIdsToSend, ::UnstructuredGridBlockStructure& blockStructure)
{
  ::CopyCellIdsToSendIntoBlockStructure(cellIdsToSend, blockStructure);
}

//----------------------------------------------------------------------------
template <>
void CopyCellIdsToSendIntoBlockStructure(vtkPolyData* input,
  const std::set<vtkIdType>& cellIdsToSend, ::PolyDataBlockStructure& blockStructure)
{
  ::CopyCellIdsToSendIntoBlockStructure(cellIdsToSend, blockStructure);

  vtkIdList* polyIdsToSend = blockStructure.PolyIdsToSend;
  vtkIdList* stripIdsToSend = blockStructure.StripIdsToSend;
  vtkIdList* lineIdsToSend = blockStructure.LineIdsToSend;

  polyIdsToSend->SetNumberOfIds(blockStructure.NumberOfPolysToSend);
  stripIdsToSend->SetNumberOfIds(blockStructure.NumberOfStripsToSend);
  lineIdsToSend->SetNumberOfIds(blockStructure.NumberOfLinesToSend);

  vtkIdType polyId = -1, stripId = -1, lineId = -1;

  for (const vtkIdType& cellId : cellIdsToSend)
  {
    switch (input->GetCellType(cellId))
    {
      case VTK_EMPTY_CELL:
        break;
      case VTK_VERTEX:
      case VTK_POLY_VERTEX:
        break;
      case VTK_LINE:
      case VTK_POLY_LINE:
        lineIdsToSend->SetId(++lineId, input->GetCellIdRelativeToCellArray(cellId));
        break;
      case VTK_TRIANGLE_STRIP:
        stripIdsToSend->SetId(++stripId, input->GetCellIdRelativeToCellArray(cellId));
        break;
      case VTK_TRIANGLE:
      case VTK_QUAD:
      case VTK_POLYGON:
        polyIdsToSend->SetId(++polyId, input->GetCellIdRelativeToCellArray(cellId));
        break;
      default:
        vtkLog(ERROR, "An input vtkPolyData holds a cell that is not supported.");
        break;
    }
  }
}

//----------------------------------------------------------------------------
template <class PointSetT>
void UpdateCellBufferSize(vtkIdType cellIdToSend,
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType::InformationType& info,
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType ::BlockStructureType&
    blockStructure);

//----------------------------------------------------------------------------
template <>
void UpdateCellBufferSize<vtkUnstructuredGrid>(vtkIdType cellIdToSend,
  ::UnstructuredGridInformation& info, ::UnstructuredGridBlockStructure& blockStructure)
{
  blockStructure.ConnectivitySize += info.Input->GetCells()->GetCellSize(cellIdToSend);

  vtkCellArray* faces = info.Faces;
  vtkCellArray* faceLocations = info.FaceLocations;
  if (faces && faceLocations && faceLocations->GetCellSize(cellIdToSend) != 0) // i.e. is polyhedron
  {
    vtkIdType& facesSize = blockStructure.FacesSize;
    vtkIdType& facesNum = blockStructure.FacesNum;
    vtkIdType numberOfFaces = 0;
    //
    numberOfFaces = faceLocations->GetCellSize(cellIdToSend);
    facesNum += numberOfFaces;
    facesSize += faceLocations->Visit(GetPolyhedronNPts{}, cellIdToSend, faces);
  }
}

//----------------------------------------------------------------------------
template <>
void UpdateCellBufferSize<vtkPolyData>(
  vtkIdType cellIdToSend, ::PolyDataInformation& info, ::PolyDataBlockStructure& blockStructure)
{
  switch (info.Input->GetCellType(cellIdToSend))
  {
    case VTK_EMPTY_CELL:
      break;
    case VTK_VERTEX:
    case VTK_POLY_VERTEX:
      break;
    case VTK_LINE:
    case VTK_POLY_LINE:
    {
      ++blockStructure.NumberOfLinesToSend;
      vtkPolyData* input = info.Input;
      blockStructure.LineConnectivitySize +=
        input->GetLines()->GetCellSize(input->GetCellIdRelativeToCellArray(cellIdToSend));
      break;
    }
    case VTK_TRIANGLE_STRIP:
    {
      ++blockStructure.NumberOfStripsToSend;
      vtkPolyData* input = info.Input;
      blockStructure.StripConnectivitySize +=
        input->GetStrips()->GetCellSize(input->GetCellIdRelativeToCellArray(cellIdToSend));
      break;
    }
    case VTK_TRIANGLE:
    case VTK_QUAD:
    case VTK_POLYGON:
    {
      ++blockStructure.NumberOfPolysToSend;
      vtkPolyData* input = info.Input;
      blockStructure.PolyConnectivitySize +=
        input->GetPolys()->GetCellSize(input->GetCellIdRelativeToCellArray(cellIdToSend));
      break;
    }
    default:
      vtkLog(ERROR, "An input vtkPolyData holds a cell that is not supported.");
      break;
  }
}

//----------------------------------------------------------------------------
void FillUnstructuredDataTopologyBuffer(const std::map<vtkIdType, vtkIdType>& seedPointIdsWithIndex,
  const std::map<vtkIdType, vtkIdType>& pointIdsToSendWithIndex,
  ::UnstructuredGridBlockStructure& blockStructure, vtkUnstructuredGrid* input,
  vtkIdType maxPointId)
{
  auto& buffer = blockStructure.SendBuffer;

  vtkCellArray* cellArray = buffer.CellArray;

  // We're being careful to account for different storage options in cell arrays
#ifdef VTK_USE_64BIT_IDS
  if (!(maxPointId >> 31) || !(blockStructure.ConnectivitySize >> 31))
  {
    cellArray->ConvertTo32BitStorage();
  }
#else
  (void)maxPointId;
#endif

  int mask =
    (cellArray->IsStorage64Bit() << 1) | static_cast<int>(input->GetCells()->IsStorage64Bit());

  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  switch (mask)
  {
    case 0:
      ::FillUnstructuredDataTopologyBufferFunctor<ArrayType32, ArrayType32,
        vtkUnstructuredGrid>::Fill(seedPointIdsWithIndex, pointIdsToSendWithIndex, blockStructure,
        input);
      break;
    case 1:
      ::FillUnstructuredDataTopologyBufferFunctor<ArrayType64, ArrayType32,
        vtkUnstructuredGrid>::Fill(seedPointIdsWithIndex, pointIdsToSendWithIndex, blockStructure,
        input);
      break;
    case 2:
      ::FillUnstructuredDataTopologyBufferFunctor<ArrayType32, ArrayType64,
        vtkUnstructuredGrid>::Fill(seedPointIdsWithIndex, pointIdsToSendWithIndex, blockStructure,
        input);
      break;
    case 3:
      ::FillUnstructuredDataTopologyBufferFunctor<ArrayType64, ArrayType64,
        vtkUnstructuredGrid>::Fill(seedPointIdsWithIndex, pointIdsToSendWithIndex, blockStructure,
        input);
      break;
  }
}

//----------------------------------------------------------------------------
void FillUnstructuredDataTopologyBuffer(const std::map<vtkIdType, vtkIdType>& seedPointIdsWithIndex,
  const std::map<vtkIdType, vtkIdType>& pointIdsToSendWithIndex,
  ::PolyDataBlockStructure& blockStructure, vtkPolyData* input, vtkIdType maxPointId)
{
  auto& buffer = blockStructure.SendBuffer;

  vtkCellArray* cellArrays[] = { buffer.Polys, buffer.Strips, buffer.Lines };
  vtkCellArray* inputCellArrays[] = { input->GetPolys(), input->GetStrips(), input->GetLines() };
  vtkIdType connectivitySize[] = { blockStructure.PolyConnectivitySize,
    blockStructure.StripConnectivitySize, blockStructure.LineConnectivitySize };
  vtkIdList* cellIdsToSend[] = { blockStructure.PolyIdsToSend, blockStructure.StripIdsToSend,
    blockStructure.LineIdsToSend };

  // We're being careful to account for different storage options in cell arrays
#ifdef VTK_USE_64BIT_IDS
  bool convertTo32Bits = !(maxPointId >> 31) || !(connectivitySize[0] >> 31) ||
    !(connectivitySize[1] >> 31) || !(connectivitySize[2] >> 31);
#else
  (void)maxPointId;
#endif

  for (int i = 0; i < 3; ++i)
  {
    vtkCellArray* cells = cellArrays[i];
    vtkCellArray* inputCells = inputCellArrays[i];

#ifdef VTK_USE_64BIT_IDS
    if (convertTo32Bits)
    {
      cells->ConvertTo32BitStorage();
    }
#endif

    int mask = (cells->IsStorage64Bit() << 1) | static_cast<int>(inputCells->IsStorage64Bit());

    using ArrayType32 = vtkCellArray::ArrayType32;
    using ArrayType64 = vtkCellArray::ArrayType64;

    switch (mask)
    {
      case 0:
        ::FillUnstructuredDataTopologyBufferFunctor<ArrayType32, ArrayType32, vtkPolyData>::Fill(
          seedPointIdsWithIndex, pointIdsToSendWithIndex, inputCells, cells, cellIdsToSend[i],
          connectivitySize[i]);
        break;
      case 1:
        ::FillUnstructuredDataTopologyBufferFunctor<ArrayType64, ArrayType32, vtkPolyData>::Fill(
          seedPointIdsWithIndex, pointIdsToSendWithIndex, inputCells, cells, cellIdsToSend[i],
          connectivitySize[i]);
        break;
      case 2:
        ::FillUnstructuredDataTopologyBufferFunctor<ArrayType32, ArrayType64, vtkPolyData>::Fill(
          seedPointIdsWithIndex, pointIdsToSendWithIndex, inputCells, cells, cellIdsToSend[i],
          connectivitySize[i]);
        break;
      case 3:
        ::FillUnstructuredDataTopologyBufferFunctor<ArrayType64, ArrayType64, vtkPolyData>::Fill(
          seedPointIdsWithIndex, pointIdsToSendWithIndex, inputCells, cells, cellIdsToSend[i],
          connectivitySize[i]);
        break;
    }
  }
}

//----------------------------------------------------------------------------
/**
 * Given seed point ids mapped with their index inside the given list, which should basically be the
 * ids of the points interfacing with the current connected block, this function computes, looking
 * at the connectivity of the input data set, which other points are to be sent to the connected
 * block, as well as which cells. It then fills buffers describing the geometry of the cells that we
 * need to send.
 */
template <class PointSetT>
void BuildTopologyBufferToSend(vtkIdList* seedPointIds,
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType ::InformationType& info,
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType ::BlockStructureType&
    blockStructure,
  int outputGhostLevels)
{
  vtkIdType maxPointId = 0;

  PointSetT* input = info.Input;

  std::set<vtkIdType> pointIdsToSend;
  std::set<vtkIdType> cellIdsToSend;
  vtkNew<vtkIdList> ids;

  for (vtkIdType pointId = 0; pointId < seedPointIds->GetNumberOfIds(); ++pointId)
  {
    pointIdsToSend.insert(seedPointIds->GetId(pointId));
  }

  std::set<vtkIdType> cellIdsToSendAtLastLevel;
  std::set<vtkIdType> pointIdsToSendAtLastLevel;

  pointIdsToSendAtLastLevel.insert(pointIdsToSend.cbegin(), pointIdsToSend.cend());

  vtkUnsignedCharArray* ghostCellArray = input->GetCellGhostArray();

  // At each level, we look at the last chunk of point its that we added (starting with
  // seed points that are on the interface between us and the neighboring block).
  for (int ghostLevel = 0; ghostLevel < outputGhostLevels; ++ghostLevel)
  {
    std::set<vtkIdType> cellIdsToSendAtThisLevel;
    std::set<vtkIdType> pointIdsToSendAtThisLevel;

    // For each point in this chunk of points, we look at every cells that use this point.
    // If the found cell has already been added as a cell to send, we skip. If not, we add it as a
    // cell to send.
    for (const vtkIdType& pointId : pointIdsToSendAtLastLevel)
    {
      input->GetPointCells(pointId, ids);
      for (vtkIdType id = 0; id < ids->GetNumberOfIds(); ++id)
      {
        vtkIdType cellIdToSend = ids->GetId(id);
        if ((!ghostCellArray ||
              !(ghostCellArray->GetValue(cellIdToSend) &
                ::GHOST_CELL_TO_PEEL_IN_UNSTRUCTURED_DATA)) &&
          !cellIdsToSend.count(cellIdToSend))
        {
          cellIdsToSendAtThisLevel.insert(cellIdToSend);
          cellIdsToSend.insert(cellIdToSend);

          ::UpdateCellBufferSize<PointSetT>(cellIdToSend, info, blockStructure);
        }
      }
    }

    // For each cells that we want to send at this level, we look at all points composing them, and
    // we add any point that has never been processed in the previous scope into the new chunk of
    // points.
    for (const vtkIdType& cellId : cellIdsToSendAtThisLevel)
    {
      input->GetCellPoints(cellId, ids);
      for (vtkIdType id = 0; id < ids->GetNumberOfIds(); ++id)
      {
        vtkIdType pointIdToSend = ids->GetId(id);
        if (!pointIdsToSend.count(pointIdToSend))
        {
          maxPointId = std::max(pointIdToSend, maxPointId);
          pointIdsToSendAtThisLevel.insert(pointIdToSend);
          pointIdsToSend.insert(pointIdToSend);
        }
      }
    }

    std::swap(cellIdsToSendAtThisLevel, cellIdsToSendAtLastLevel);
    std::swap(pointIdsToSendAtThisLevel, pointIdsToSendAtLastLevel);
  }

  // FIXME
  // We want to create an index for each point we want to send.
  // This will help up locate those points in the sending buffer. We do that because we are not
  // going to send again the interfacing points. Our neighbor is already aware of those points.
  // We index the interfacing points, we get rid of them in the buffer that we constructed in the
  // last scope, which owns a copy of them. At the end, the union of the 2 std::map we are
  // constructed has no overlaps, and describes every points that play a role in ghost exchanging.
  std::map<vtkIdType, vtkIdType> seedPointIdsWithIndex;
  {
    vtkIdType tag = 0;
    // We remove those seed points from the union of all point ids to send.
    for (auto it = seedPointIds->begin(); it != seedPointIds->end(); ++it)
    {
      pointIdsToSend.erase(*it);
      seedPointIdsWithIndex[*it] = ++tag;
    }
  }

  std::map<vtkIdType, vtkIdType> pointIdsToSendWithIndex;
  {
    vtkIdType id = 0;
    for (const vtkIdType& pointId : pointIdsToSend)
    {
      pointIdsToSendWithIndex[pointId] = id++;
    }
  }

  blockStructure.PointIdsToSend->SetNumberOfIds(pointIdsToSend.size());

  // We fill our internal buffer of point ids to send (skipping those on the interface)
  vtkSMPTools::Transform(pointIdsToSend.cbegin(), pointIdsToSend.cend(),
    blockStructure.PointIdsToSend->begin(), [](vtkIdType pointId) -> vtkIdType { return pointId; });

  ::CopyCellIdsToSendIntoBlockStructure(input, cellIdsToSend, blockStructure);

  ::FillUnstructuredDataTopologyBuffer(
    seedPointIdsWithIndex, pointIdsToSendWithIndex, blockStructure, input, maxPointId);
}

//----------------------------------------------------------------------------
template <class PointSetT>
::LinkMap ComputeLinkMapForUnstructuredData(
  const diy::Master& master, std::vector<PointSetT*>& inputs, int outputGhostLevels)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;
  using BlockInformationType = typename BlockType::InformationType;

  using Dispatcher = vtkArrayDispatch::Dispatch;

  ::LinkMap linkMap(inputs.size());

  // For each local point id to be sent to connected blocks, this multimap
  // stores which block id this point is to be sent to, as well as its position in the buffer being
  // sent to its corresponding block.
  std::vector<std::multimap<vtkIdType, std::pair<int, vtkIdType>>>
    localPointIdsToSendBufferMultimaps(inputs.size());

  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    BlockType* block = master.block<BlockType>(localId);
    ::BlockMapType<BlockStructureType>& blockStructures = block->BlockStructures;
    BlockInformationType& info = block->Information;

    if (!info.InterfacePoints)
    {
      blockStructures.clear();
      continue;
    }

    vtkIdTypeArray* globalPointIds = info.InterfaceGlobalPointIds;
    ::Links& localLinks = linkMap[localId];

    ::MatchingPointExtractor matchingPointExtractor(info.InterfacePointIds,
      vtkPointSet::SafeDownCast(info.InterfaceExtractor->GetOutputDataObject(0)),
      info.InterfacePoints, globalPointIds, info.InputToOutputPointIdRedirectionMap);

    for (auto it = blockStructures.begin(); it != blockStructures.end();)
    {
      BlockStructureType& blockStructure = it->second;
      vtkIdList* matchingReceivedPointIds = blockStructure.MatchingReceivedPointIds;
      matchingPointExtractor.MatchingSourcePointIds = matchingReceivedPointIds;
      matchingPointExtractor.RemappedMatchingReceivedPointIdsSortedLikeTarget =
        blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget;
      vtkDataArray* interfacingPointsArray = blockStructure.InterfacingPoints->GetData();
      vtkIdTypeArray* interfacingGlobalPointIds = blockStructure.InterfacingGlobalPointIds;

      if ((interfacingGlobalPointIds == nullptr) !=
        matchingPointExtractor.SourceGlobalPointIds.empty())
      {
        vtkLog(ERROR,
          "Inconsistency in the presence of global point ids across partitions. "
          "The pipeline will fail at generating ghost cells");
        return LinkMap();
      }

      if (!Dispatcher::Execute(
            interfacingPointsArray, matchingPointExtractor, interfacingGlobalPointIds))
      {
        matchingPointExtractor(interfacingPointsArray, interfacingGlobalPointIds);
      }

      // Blocks are connected if there is at least one point that is in both blocks.
      // If there are none, we delete the block in BlockStructures.
      if (matchingReceivedPointIds->GetNumberOfIds())
      {
        localLinks.emplace(it->first);

        ::BuildTopologyBufferToSend<PointSetT>(
          matchingReceivedPointIds, info, blockStructure, outputGhostLevels);

        vtkIdList* pointIdsToSend = blockStructure.PointIdsToSend;
        for (vtkIdType id = 0; id < pointIdsToSend->GetNumberOfIds(); ++id)
        {
          localPointIdsToSendBufferMultimaps[localId].insert(
            { pointIdsToSend->GetId(id), { it->first, id } });
        }
        ++it;
      }
      else
      {
        it = blockStructures.erase(it);
      }
    }
  }

  // In this part, we look at points that are duplicated among every blocks.
  // In the previous step, we looked at what points / cells we needed to send. It is possible that
  // multiple blocks own a copy of the same point and that those blocks need to exchange this point
  // information to some common block neighbor. When such events happen, the receiving block will
  // instantiate multiple copies of the same point if nothing is done against it.
  // We can detect those points by looking at which points on our interface do we send to multiple
  // blocks. An interfacing point for one block A can be a non-interfacing point for a block B, and
  // be sent both by us and A to B.
  //
  // So here, we list each points for which it could happen and store it in SharedPointIds.
  // The receiving block will then be able to look at those and deal with this information.
  // We only need to send the index of duplicate points.
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    BlockType* block = master.block<BlockType>(localId);
    auto& blockStructures = block->BlockStructures;

    const auto& localPointIdsToSendBufferMultimap = localPointIdsToSendBufferMultimaps[localId];

    vtkIdType previousPointId = -1;
    vtkIdType previousLocalId = -1;
    vtkIdType previousPointIdInSendBuffer = -1;
    for (auto it = localPointIdsToSendBufferMultimap.cbegin();
         it != localPointIdsToSendBufferMultimap.cend(); ++it)
    {
      vtkIdType pointId = it->first;

      if (pointId == previousPointId)
      {
        // Do not forget to store the previous point as it is a duplicate.
        blockStructures.at(previousLocalId)
          .SharedPointIds->InsertNextValue(previousPointIdInSendBuffer);
      }

      // Look for other duplicates and store the one we just found
      while (pointId == previousPointId && it != localPointIdsToSendBufferMultimap.cend())
      {
        auto& pair = it->second;
        int localIdTmp = pair.first;
        vtkIdType pointIdInSendBuffer = pair.second;
        blockStructures.at(localIdTmp).SharedPointIds->InsertNextValue(pointIdInSendBuffer);
        if (++it == localPointIdsToSendBufferMultimap.cend())
        {
          break;
        }
        pointId = it->first;
      }
      if (it == localPointIdsToSendBufferMultimap.cend())
      {
        break;
      }

      previousPointId = pointId;
      previousLocalId = it->second.first;
      previousPointIdInSendBuffer = it->second.second;
    }
  }

  return linkMap;
}

//----------------------------------------------------------------------------
/**
 * Given 2 input extents `localExtent` and `extent`, this function returns the list of ids in `grid`
 * such that the cells lie in the intersection of the 2 input extents.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeInterfaceCellIdsForStructuredData(
  const ::ExtentType& localExtent, const ::ExtentType& extent, GridDataSetT* grid)
{
  int imin, imax, jmin, jmax, kmin, kmax;
  // We shift imax, jmax and kmax in case of degenerate dimension.
  imin = std::max(extent[0], localExtent[0]);
  imax = std::min(extent[1], localExtent[1]) + (localExtent[0] == localExtent[1]);
  jmin = std::max(extent[2], localExtent[2]);
  jmax = std::min(extent[3], localExtent[3]) + (localExtent[2] == localExtent[3]);
  kmin = std::max(extent[4], localExtent[4]);
  kmax = std::min(extent[5], localExtent[5]) + (localExtent[4] == localExtent[5]);

  const int* gridExtent = grid->GetExtent();

  vtkNew<vtkIdList> ids;
  ids->SetNumberOfIds((imax - imin) * (jmax - jmin) * (kmax - kmin));
  vtkIdType count = 0;
  int ijk[3];
  for (int k = kmin; k < kmax; ++k)
  {
    ijk[2] = k;
    for (int j = jmin; j < jmax; ++j)
    {
      ijk[1] = j;
      for (int i = imin; i < imax; ++i, ++count)
      {
        ijk[0] = i;
        ids->SetId(count, vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk));
      }
    }
  }
  return ids;
}

//----------------------------------------------------------------------------
/**
 * This function returns the ids in input `grid` of the cells such that `grid`'s extent overlaps the
 * block of global id gid's extent when ghosts are added.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeInputInterfaceCellIdsForStructuredData(
  const typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType* block, int gid,
  GridDataSetT* grid)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  const BlockStructureType& blockStructure = block->BlockStructures.at(gid);
  const ::ExtentType& extent = blockStructure.ShiftedExtentWithNewGhosts;
  const ::ExtentType& localExtent = block->Information.Extent;

  return ::ComputeInterfaceCellIdsForStructuredData(localExtent, extent, grid);
}

//----------------------------------------------------------------------------
/**
 * This function returns the ids in output `grid` of the cells such that `grid`'s extent overlaps
 * the block of global id gid's extent when ghosts are added.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeOutputInterfaceCellIdsForStructuredData(
  typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType::BlockStructureType&
    blockStructure,
  GridDataSetT* grid)
{
  const ::ExtentType& extent = blockStructure.ShiftedExtent;
  int* gridExtent = grid->GetExtent();
  ::ExtentType localExtent{ gridExtent[0], gridExtent[1], gridExtent[2], gridExtent[3],
    gridExtent[4], gridExtent[5] };

  return ::ComputeInterfaceCellIdsForStructuredData(localExtent, extent, grid);
}

//----------------------------------------------------------------------------
/**
 * Given 2 input extents `localExtent` and `extent`, this function returns the list of ids in `grid`
 * such that the points lie in the intersection of the 2 input extents.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeInterfacePointIdsForStructuredData(unsigned char adjacencyMask,
  const ::ExtentType& localExtent, const ::ExtentType& extent, GridDataSetT* grid, bool crop)
{
  int imin, imax, jmin, jmax, kmin, kmax;
  imin = std::max(extent[0], localExtent[0]);
  imax = std::min(extent[1], localExtent[1]);
  jmin = std::max(extent[2], localExtent[2]);
  jmax = std::min(extent[3], localExtent[3]);
  kmin = std::max(extent[4], localExtent[4]);
  kmax = std::min(extent[5], localExtent[5]);

  constexpr unsigned char LR = ::Adjacency::Right | ::Adjacency::Left;
  constexpr unsigned char BF = ::Adjacency::Back | ::Adjacency::Front;
  constexpr unsigned char TB = ::Adjacency::Top | ::Adjacency::Bottom;

  // When this method is called from the receiving block, they put in the negative of the bit mask
  // that was used by the sending block. This means that pairs 00 become 11. We're resetting them to
  // 00 here.
  if ((adjacencyMask & LR) == LR)
  {
    adjacencyMask &= ~LR;
  }
  if ((adjacencyMask & BF) == BF)
  {
    adjacencyMask &= ~BF;
  }
  if ((adjacencyMask & TB) == TB)
  {
    adjacencyMask &= ~TB;
  }

  int cropimin = imin;
  int cropimax = imax;
  int cropjmin = jmin;
  int cropjmax = jmax;
  int cropkmin = kmin;
  int cropkmax = kmax;

  if (crop)
  {
    // This case sets ownership of points
    // If the function was called asking to remove the interface (i.e. points between the
    // partitions that are mangled), we remove them by setting an extent in which
    // points will be skipped.
    // The owner of the point is set to be the block of lowest id owning a copy of the point.
    // We actually share more points than we would need to in theory, because we are sending
    // some points that are on our surface, which could be sent by other processes as well.
    // We need to send those points so if there is a different point data across partitions,
    // the owner of the point sets the values in the other partitions. This can be useful
    // if the user wants to keep track of which partition owns the non ghost copy of the point.
    // See paraview/paraview#21347
    //
    // Little illustration with 4 partitions each owning one pixel:
    //  __p__
    // | 0| 1|
    // |__|__|
    // | 3| 2|
    // |__|__|
    //
    // Let us assume we're generating one layer of ghosts.
    // p is located by partition 0 and 1 at the beginning of the
    // pipeline. 0 will be the owner of the point (the partition having a non ghost copy of p).
    // We are actually sendint p twice to partition 2 and 3.
    // Later on in the pipeline, the first time p is encountered in a buffer, the point is set.
    // When p appears again in a buffer (that would be sent by partition 1), it is skipped, setting
    // p to the value of partition 0.
    //
    // When removing the interface, 1 gets the whole interface with 0 removed, so 0 doesn't receive
    // the points from 1 at the interface. On the contrary, 0 sends his to 1.
    // 2 doesn't send its corner point (a single point) to 0, but 0 sends his to 2.
    // This ablation is done using the cropimin etc. variables.
    if (adjacencyMask & ::Adjacency::Right)
    {
      cropimin = cropimax = imax;
    }
    else if (adjacencyMask & ::Adjacency::Left)
    {
      cropimin = cropimax = imin;
    }
    if (adjacencyMask & ::Adjacency::Back)
    {
      cropjmin = cropjmax = jmax;
    }
    else if (adjacencyMask & ::Adjacency::Front)
    {
      cropjmin = cropjmax = jmin;
    }
    if (adjacencyMask & ::Adjacency::Top)
    {
      cropkmin = cropkmax = kmax;
    }
    else if (adjacencyMask & ::Adjacency::Bottom)
    {
      cropkmin = cropkmax = kmin;
    }
  }

  const int* gridExtent = grid->GetExtent();
  vtkNew<vtkIdList> ids;

  ids->SetNumberOfIds((imax - imin + 1) * (jmax - jmin + 1) * (kmax - kmin + 1) -
    (crop ? (cropimax - cropimin + 1) * (cropjmax - cropjmin + 1) * (cropkmax - cropkmin + 1) : 0));
  vtkIdType count = -1;
  int ijk[3];
  for (ijk[2] = kmin; ijk[2] <= kmax; ++ijk[2])
  {
    for (ijk[1] = jmin; ijk[1] <= jmax; ++ijk[1])
    {
      for (ijk[0] = imin; ijk[0] <= imax; ++ijk[0])
      {
        if (crop && ijk[0] >= cropimin && ijk[0] <= cropimax && ijk[1] >= cropjmin &&
          ijk[1] <= cropjmax && ijk[2] >= cropkmin && ijk[2] <= cropkmax)
        {
          continue;
        }
        ids->SetId(++count, vtkStructuredData::ComputePointIdForExtent(gridExtent, ijk));
      }
    }
  }

  return ids;
}

//----------------------------------------------------------------------------
/**
 * This function returns the ids in input `grid` of the points such that `grid`'s extent overlaps
 * the block of global id gid's extent when ghosts are added.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeInputInterfacePointIdsForStructuredData(
  const typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType* block, int gid,
  GridDataSetT* grid, bool crop)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  const BlockStructureType& blockStructure = block->BlockStructures.at(gid);
  const unsigned char& adjacencyMask = blockStructure.AdjacencyMask;
  const ::ExtentType& extent = blockStructure.ShiftedExtentWithNewGhosts;
  const ::ExtentType& localExtent = block->Information.Extent;

  return ::ComputeInterfacePointIdsForStructuredData(
    adjacencyMask, localExtent, extent, grid, crop);
}

//----------------------------------------------------------------------------
/**
 * This function returns the ids in output `grid` of the points such that `grid`'s extent overlaps
 * the block corresponding to current blockStruture's extent when ghosts are added.
 */
template <class GridDataSetT>
vtkSmartPointer<vtkIdList> ComputeOutputInterfacePointIdsForStructuredData(
  typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType::BlockStructureType&
    blockStructure,
  GridDataSetT* grid, bool crop)
{
  const unsigned char& adjacencyMask = blockStructure.AdjacencyMask;
  const ::ExtentType& extent = blockStructure.ShiftedExtent;
  int* gridExtent = grid->GetExtent();
  ::ExtentType localExtent{ gridExtent[0], gridExtent[1], gridExtent[2], gridExtent[3],
    gridExtent[4], gridExtent[5] };

  // We apply a bitwise NOT operation on adjacencyMask to have the same adjacency mask as
  // in the Input version of this function. It produces an axial symmetry on each dimension
  // having an adjacency.
  return ::ComputeInterfacePointIdsForStructuredData(
    ~adjacencyMask, localExtent, extent, grid, crop);
}

//----------------------------------------------------------------------------
void UpdateOutputGridPoints(
  vtkImageData* vtkNotUsed(output), ::ImageDataInformation& vtkNotUsed(blockInformation))
{
  // Points are implicit in an vtkImageData. We do nothing.
}

//----------------------------------------------------------------------------
void AppendGhostPointsForRectilinearGrid(vtkSmartPointer<vtkDataArray>& coordinates,
  vtkSmartPointer<vtkDataArray>& preCoordinates, vtkSmartPointer<vtkDataArray>& postCoordinates)
{
  if (preCoordinates)
  {
    std::swap(preCoordinates, coordinates);
    coordinates->InsertTuples(coordinates->GetNumberOfTuples(), preCoordinates->GetNumberOfTuples(),
      0, preCoordinates.GetPointer());
  }
  if (postCoordinates)
  {
    coordinates->InsertTuples(coordinates->GetNumberOfTuples(),
      postCoordinates->GetNumberOfTuples(), 0, postCoordinates.GetPointer());
  }
}

//----------------------------------------------------------------------------
void UpdateOutputGridPoints(
  vtkRectilinearGrid* output, ::RectilinearGridInformation& blockInformation)
{
  auto& coordinateGhosts = blockInformation.CoordinateGhosts;

  vtkSmartPointer<vtkDataArray> xCoordinates = blockInformation.XCoordinates;
  vtkSmartPointer<vtkDataArray>& preXCoordinates = coordinateGhosts[0];
  AppendGhostPointsForRectilinearGrid(xCoordinates, preXCoordinates, coordinateGhosts[1]);
  output->SetXCoordinates(xCoordinates);

  vtkSmartPointer<vtkDataArray>& yCoordinates = blockInformation.YCoordinates;
  vtkSmartPointer<vtkDataArray>& preYCoordinates = coordinateGhosts[2];
  AppendGhostPointsForRectilinearGrid(yCoordinates, preYCoordinates, coordinateGhosts[3]);
  output->SetYCoordinates(yCoordinates);

  vtkSmartPointer<vtkDataArray>& zCoordinates = blockInformation.ZCoordinates;
  vtkSmartPointer<vtkDataArray>& preZCoordinates = coordinateGhosts[4];
  AppendGhostPointsForRectilinearGrid(zCoordinates, preZCoordinates, coordinateGhosts[5]);
  output->SetZCoordinates(zCoordinates);
}

//----------------------------------------------------------------------------
void UpdateOutputGridPoints(
  vtkStructuredGrid* output, ::StructuredGridInformation& blockInformation)
{
  // We create a new instance because at this point input and output share the same point arrays.
  // This is done in vtkStructuredGrid::CopyStructure.
  vtkNew<vtkPoints> points;
  vtkPoints* inputPoints = blockInformation.InputPoints;
  const ::ExtentType& inputExtent = blockInformation.Extent;
  const int* extent = output->GetExtent();

  if (inputPoints)
  {
    points->SetDataType(inputPoints->GetDataType());
  }
  points->SetNumberOfPoints(
    (extent[1] - extent[0] + 1) * (extent[3] - extent[2] + 1) * (extent[5] - extent[4] + 1));

  int ijk[3];

  for (int k = inputExtent[4]; k <= inputExtent[5]; ++k)
  {
    ijk[2] = k;
    for (int j = inputExtent[2]; j <= inputExtent[3]; ++j)
    {
      ijk[1] = j;
      for (int i = inputExtent[0]; i <= inputExtent[1]; ++i)
      {
        ijk[0] = i;
        double* point = inputPoints->GetPoint(
          vtkStructuredData::ComputePointIdForExtent(inputExtent.data(), ijk));
        points->SetPoint(vtkStructuredData::ComputePointIdForExtent(extent, ijk), point);
      }
    }
  }
  output->SetPoints(points);
}

//----------------------------------------------------------------------------
template <class GridDataSetT>
void UpdateOutputGridStructure(GridDataSetT* output,
  typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType::InformationType&
    blockInformation)
{
  const ::ExtentType& ghostThickness = blockInformation.ExtentGhostThickness;
  ::ExtentType outputExtent = blockInformation.Extent;
  // We update the extent of the current output and add ghost layers.
  outputExtent[0] -= ghostThickness[0];
  outputExtent[1] += ghostThickness[1];
  outputExtent[2] -= ghostThickness[2];
  outputExtent[3] += ghostThickness[3];
  outputExtent[4] -= ghostThickness[4];
  outputExtent[5] += ghostThickness[5];
  output->SetExtent(outputExtent.data());

  ::UpdateOutputGridPoints(output, blockInformation);
}

//----------------------------------------------------------------------------
void CloneDataObject(vtkDataObject* input, vtkDataObject* clone)
{
  clone->GetFieldData()->ShallowCopy(input->GetFieldData());
}

//----------------------------------------------------------------------------
/**
 * Clone a `grid` into a `clone`. clone should have wider extents than grid.
 * This function does a deep copy of every scalar fields.
 */
template <class GridDataSetT>
void CloneGrid(GridDataSetT* grid, GridDataSetT* clone, const ExtentType& extent)
{
  ::CloneDataObject(grid, clone);

  vtkCellData* cloneCellData = clone->GetCellData();
  vtkCellData* gridCellData = grid->GetCellData();
  cloneCellData->CopyAllOn();
  cloneCellData->CopyAllocate(gridCellData, clone->GetNumberOfCells());
  cloneCellData->SetNumberOfTuples(clone->GetNumberOfCells());

  const int* cloneExtent = clone->GetExtent();
  const int* gridExtent = grid->GetExtent();

  // We use `std::max` here to work for grids of dimension 2 and 1.
  // This gives "thickness" to the degenerate dimension
  int imin = extent[0];
  int imax = std::max(extent[1], extent[0] + 1);
  int jmin = extent[2];
  int jmax = std::max(extent[3], extent[2] + 1);
  int kmin = extent[4];
  int kmax = std::max(extent[5], extent[4] + 1);

  int ijk[3];

  if (cloneCellData->GetNumberOfTuples())
  {
    for (ijk[2] = kmin; ijk[2] < kmax; ++ijk[2])
    {
      for (ijk[1] = jmin; ijk[1] < jmax; ++ijk[1])
      {
        for (ijk[0] = imin; ijk[0] < imax; ++ijk[0])
        {
          cloneCellData->SetTuple(vtkStructuredData::ComputeCellIdForExtent(cloneExtent, ijk),
            vtkStructuredData::ComputeCellIdForExtent(gridExtent, ijk), gridCellData);
        }
      }
    }
  }

  // We need to reset the newly allocated ghost cells to 0 if there was a ghost array in the input
  if (vtkUnsignedCharArray* ghostCells = cloneCellData->GetGhostArray())
  {
    auto ghostRange = vtk::DataArrayValueRange<1>(ghostCells);
    for (ijk[2] = cloneExtent[4]; ijk[2] < cloneExtent[5]; ++ijk[2])
    {
      for (ijk[1] = cloneExtent[2]; ijk[1] < cloneExtent[3]; ++ijk[1])
      {
        for (ijk[0] = cloneExtent[0]; ijk[0] < cloneExtent[1]; ++ijk[0])
        {
          if (ijk[0] < imin || ijk[0] >= imax || ijk[1] < jmin || ijk[1] >= jmax || ijk[2] < kmin ||
            ijk[2] >= kmax)
          {
            ghostRange[vtkStructuredData::ComputeCellIdForExtent(cloneExtent, ijk)] = 0;
          }
        }
      }
    }
  }

  vtkPointData* clonePointData = clone->GetPointData();
  vtkPointData* gridPointData = grid->GetPointData();
  clonePointData->CopyAllOn();
  clonePointData->CopyAllocate(gridPointData, clone->GetNumberOfPoints());
  clonePointData->SetNumberOfTuples(clone->GetNumberOfPoints());

  imax = extent[1];
  jmax = extent[3];
  kmax = extent[5];

  if (clonePointData->GetNumberOfTuples())
  {
    for (ijk[2] = kmin; ijk[2] <= kmax; ++ijk[2])
    {
      for (ijk[1] = jmin; ijk[1] <= jmax; ++ijk[1])
      {
        for (ijk[0] = imin; ijk[0] <= imax; ++ijk[0])
        {
          clonePointData->SetTuple(vtkStructuredData::ComputePointIdForExtent(cloneExtent, ijk),
            vtkStructuredData::ComputePointIdForExtent(gridExtent, ijk), gridPointData);
        }
      }
    }
  }

  // We need to reset the newly allocated ghost points to 0 if there was a ghost array in the input
  if (vtkUnsignedCharArray* ghostPoints = clonePointData->GetGhostArray())
  {
    auto ghostRange = vtk::DataArrayValueRange<1>(ghostPoints);
    for (ijk[2] = cloneExtent[4]; ijk[2] <= cloneExtent[5]; ++ijk[2])
    {
      for (ijk[1] = cloneExtent[2]; ijk[1] <= cloneExtent[3]; ++ijk[1])
      {
        for (ijk[0] = cloneExtent[0]; ijk[0] <= cloneExtent[1]; ++ijk[0])
        {
          if (ijk[0] < imin || ijk[0] > imax || ijk[1] < jmin || ijk[1] > jmax || ijk[2] < kmin ||
            ijk[2] > kmax)
          {
            ghostRange[vtkStructuredData::ComputePointIdForExtent(cloneExtent, ijk)] = 0;
          }
        }
      }
    }
  }
}

//----------------------------------------------------------------------------
void CloneCellData(vtkPointSet* ps, vtkPointSet* clone, ::UnstructuredDataInformation& info)
{
  vtkCellData* cloneCellData = clone->GetCellData();
  vtkCellData* psCellData = ps->GetCellData();
  cloneCellData->CopyAllOn();
  cloneCellData->CopyAllocate(psCellData, clone->GetNumberOfCells());
  cloneCellData->SetNumberOfTuples(clone->GetNumberOfCells());

  if (vtkIdList* redirectionMap = info.OutputToInputCellIdRedirectionMap)
  {
    for (int arrayId = 0; arrayId < cloneCellData->GetNumberOfArrays(); ++arrayId)
    {
      psCellData->GetAbstractArray(arrayId)->GetTuples(
        redirectionMap, cloneCellData->GetAbstractArray(arrayId));
    }
  }
  else
  {
    for (int arrayId = 0; arrayId < cloneCellData->GetNumberOfArrays(); ++arrayId)
    {
      vtkAbstractArray* sourceArray = psCellData->GetAbstractArray(arrayId);
      sourceArray->GetTuples(
        0, sourceArray->GetNumberOfTuples() - 1, cloneCellData->GetAbstractArray(arrayId));
    }
  }
  // We need to initialize newly allocated ghosts to zero
  if (vtkUnsignedCharArray* ghostCells = cloneCellData->GetGhostArray())
  {
    auto ghostRange = vtk::DataArrayValueRange<1>(ghostCells);
    std::fill(ghostRange.begin() + info.NumberOfInputCells, ghostRange.end(), 0);
  }
}

//----------------------------------------------------------------------------
void ClonePointData(vtkPointSet* ps, vtkPointSet* clone, ::UnstructuredDataInformation& info)
{
  vtkPointData* clonePointData = clone->GetPointData();
  vtkPointData* psPointData = ps->GetPointData();
  clonePointData->CopyAllOn();
  clonePointData->CopyAllocate(psPointData, clone->GetNumberOfPoints());
  clonePointData->SetNumberOfTuples(clone->GetNumberOfPoints());

  if (vtkIdList* redirectionMap = info.OutputToInputPointIdRedirectionMap)
  {
    for (int arrayId = 0; arrayId < clonePointData->GetNumberOfArrays(); ++arrayId)
    {
      psPointData->GetAbstractArray(arrayId)->GetTuples(
        redirectionMap, clonePointData->GetAbstractArray(arrayId));
    }
  }
  else
  {
    for (int arrayId = 0; arrayId < clonePointData->GetNumberOfArrays(); ++arrayId)
    {
      vtkAbstractArray* sourceArray = psPointData->GetAbstractArray(arrayId);
      sourceArray->GetTuples(
        0, sourceArray->GetNumberOfTuples() - 1, clonePointData->GetAbstractArray(arrayId));
    }
  }
  // We need to initialize newly allocated ghosts to zero
  if (vtkUnsignedCharArray* ghostPoints = clonePointData->GetGhostArray())
  {
    auto ghostRange = vtk::DataArrayValueRange<1>(ghostPoints);
    std::fill(ghostRange.begin() + info.NumberOfInputPoints, ghostRange.end(), 0);
  }
}

//----------------------------------------------------------------------------
void ClonePoints(vtkPointSet* ps, vtkPointSet* clone, ::UnstructuredDataInformation& info)
{
  if (vtkIdList* redirectionMap = info.OutputToInputPointIdRedirectionMap)
  {
    ps->GetPoints()->GetData()->GetTuples(redirectionMap, clone->GetPoints()->GetData());
  }
  else
  {
    vtkPoints* sourcePoints = ps->GetPoints();
    sourcePoints->GetData()->GetTuples(
      0, sourcePoints->GetNumberOfPoints() - 1, clone->GetPoints()->GetData());
  }
}

//============================================================================
template <class ArrayT>
struct ArrayBinaryTagger
{
  using ValueType = typename ArrayT::ValueType;

  ArrayBinaryTagger(ArrayT* array, ValueType value)
    : Array(array)
    , Value(value)
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    for (vtkIdType id = startId; id < endId; ++id)
    {
      this->Array->SetValue(id, this->Array->GetValue(id) | this->Value);
    }
  }

  ArrayT* Array;
  ValueType Value;
};

//----------------------------------------------------------------------------
template <class InputArrayT, class OutputArrayT>
void DeepCopyCellsImpl(vtkCellArray* inputCells, vtkCellArray* outputCells,
  vtkIdList* cellRedirectionMap, vtkIdList* pointRedirectionMap)
{
  InputArrayT* inputConnectivity =
    vtkArrayDownCast<InputArrayT>(inputCells->GetConnectivityArray());
  InputArrayT* inputOffsets = vtkArrayDownCast<InputArrayT>(inputCells->GetOffsetsArray());
  OutputArrayT* outputConnectivity =
    vtkArrayDownCast<OutputArrayT>(outputCells->GetConnectivityArray());
  OutputArrayT* outputOffsets = vtkArrayDownCast<OutputArrayT>(outputCells->GetOffsetsArray());

  auto inputConnectivityRange = vtk::DataArrayValueRange<1>(inputConnectivity);
  auto inputOffsetsRange = vtk::DataArrayValueRange<1>(inputOffsets);
  auto outputConnectivityRange = vtk::DataArrayValueRange<1>(outputConnectivity);
  auto outputOffsetsRange = vtk::DataArrayValueRange<1>(outputOffsets);

  outputOffsetsRange[0] = 0;

  for (vtkIdType outputCellId = 0; outputCellId < cellRedirectionMap->GetNumberOfIds();
       ++outputCellId)
  {
    vtkIdType inputCellId = cellRedirectionMap->GetId(outputCellId);
    vtkIdType inputOffset = inputOffsetsRange[inputCellId];
    vtkIdType cellSize = inputOffsetsRange[inputCellId + 1] - inputOffset;
    vtkIdType outputOffset = outputOffsetsRange[outputCellId + 1] =
      outputOffsetsRange[outputCellId] + cellSize;
    outputOffset -= cellSize;

    for (vtkIdType pointId = 0; pointId < cellSize; ++pointId)
    {
      outputConnectivityRange[outputOffset + pointId] =
        pointRedirectionMap->GetId(inputConnectivityRange[inputOffset + pointId]);
    }
  }
}

//----------------------------------------------------------------------------
void DeepCopyCells(vtkCellArray* inputCells, vtkCellArray* outputCells,
  vtkIdList* cellRedirectionMap, vtkIdList* pointRedirectionMap)
{
  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;
  int mask = static_cast<int>(inputCells->IsStorage64Bit()) | (outputCells->IsStorage64Bit() << 1);

  switch (mask)
  {
    case 0:
      ::DeepCopyCellsImpl<ArrayType32, ArrayType32>(
        inputCells, outputCells, cellRedirectionMap, pointRedirectionMap);
      break;
    case 1:
      ::DeepCopyCellsImpl<ArrayType64, ArrayType32>(
        inputCells, outputCells, cellRedirectionMap, pointRedirectionMap);
      break;
    case 2:
      ::DeepCopyCellsImpl<ArrayType32, ArrayType64>(
        inputCells, outputCells, cellRedirectionMap, pointRedirectionMap);
      break;
    case 3:
      ::DeepCopyCellsImpl<ArrayType64, ArrayType64>(
        inputCells, outputCells, cellRedirectionMap, pointRedirectionMap);
      break;
  }
}

//----------------------------------------------------------------------------
template <class InputArrayT, class OutputArrayT>
void DeepCopyPolyhedronImpl(vtkCellArray* inputFaceLocations, vtkCellArray* inputFaces,
  vtkCellArray* outputFaceLocations, vtkCellArray* outputFaces, vtkIdList* cellRedirectionMap,
  vtkIdList* pointRedirectionMap)
{
  InputArrayT* inputConnectivity =
    vtkArrayDownCast<InputArrayT>(inputFaceLocations->GetConnectivityArray());
  InputArrayT* inputOffsets = vtkArrayDownCast<InputArrayT>(inputFaceLocations->GetOffsetsArray());
  InputArrayT* inputFacesConnectivity =
    vtkArrayDownCast<InputArrayT>(inputFaces->GetConnectivityArray());
  InputArrayT* inputFacesOffsets = vtkArrayDownCast<InputArrayT>(inputFaces->GetOffsetsArray());

  OutputArrayT* outputConnectivity =
    vtkArrayDownCast<OutputArrayT>(outputFaceLocations->GetConnectivityArray());
  OutputArrayT* outputOffsets =
    vtkArrayDownCast<OutputArrayT>(outputFaceLocations->GetOffsetsArray());
  OutputArrayT* outputFacesConnectivity =
    vtkArrayDownCast<OutputArrayT>(outputFaces->GetConnectivityArray());
  OutputArrayT* outputFacesOffsets = vtkArrayDownCast<OutputArrayT>(outputFaces->GetOffsetsArray());

  auto inputConnectivityRange = vtk::DataArrayValueRange<1>(inputConnectivity);
  auto inputOffsetsRange = vtk::DataArrayValueRange<1>(inputOffsets);
  auto inputFacesRange = vtk::DataArrayValueRange<1>(inputFacesOffsets);
  auto inputFacesConnectivityRange = vtk::DataArrayValueRange<1>(inputFacesConnectivity);

  auto outputConnectivityRange = vtk::DataArrayValueRange<1>(outputConnectivity);
  auto outputOffsetsRange = vtk::DataArrayValueRange<1>(outputOffsets);
  auto outputFacesRange = vtk::DataArrayValueRange<1>(outputFacesOffsets);
  auto outputFacesConnectivityRange = vtk::DataArrayValueRange<1>(outputFacesConnectivity);

  vtkIdType outputFacesId = 0;
  outputOffsetsRange[0] = 0;
  outputFacesRange[0] = 0;
  for (vtkIdType outputCellId = 0; outputCellId < cellRedirectionMap->GetNumberOfIds();
       ++outputCellId)
  {
    vtkIdType inputCellId = cellRedirectionMap->GetId(outputCellId);
    vtkIdType inputOffset = inputOffsetsRange[inputCellId];
    vtkIdType nextInputOffset = inputOffsetsRange[inputCellId + 1];
    vtkIdType cellSize = nextInputOffset - inputOffset;
    outputOffsetsRange[outputCellId + 1] = outputOffsetsRange[outputCellId] + cellSize;
    vtkIdType outputOffset = outputOffsetsRange[outputCellId];
    // Prepare cloneFaceLocations
    for (vtkIdType faceId = 0; faceId < cellSize; ++faceId)
    {
      outputConnectivityRange[outputOffset + faceId] = outputFacesId++;
    }
    // copy polyhedron faces
    for (vtkIdType faceSrcId = inputOffset, faceDstId = outputConnectivityRange[outputOffset];
         faceSrcId < nextInputOffset; ++faceSrcId, ++faceDstId)
    {
      vtkIdType faceSrcLoc = inputConnectivityRange[faceSrcId];
      vtkIdType faceSrcOffset = inputFacesRange[faceSrcLoc];
      vtkIdType nextSrcFaceOffset = inputFacesRange[faceSrcLoc + 1];
      outputFacesRange[faceDstId + 1] =
        outputFacesRange[faceDstId] + (nextSrcFaceOffset - faceSrcOffset);
      for (vtkIdType pointSrcId = faceSrcOffset, pointDestId = outputFacesRange[faceDstId];
           pointSrcId < nextSrcFaceOffset; ++pointSrcId, ++pointDestId)
      {
        outputFacesConnectivityRange[pointDestId] =
          pointRedirectionMap->GetId(inputFacesConnectivityRange[pointSrcId]);
      }
    }
  }
}
//----------------------------------------------------------------------------
void DeepCopyPolyhedrons(
  vtkUnstructuredGrid* ug, vtkUnstructuredGrid* clone, ::UnstructuredGridInformation& info)
{

  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  vtkCellArray* ugFaceLocations = ug->GetPolyhedronFaceLocations();
  vtkCellArray* ugFaces = ug->GetPolyhedronFaces();
  vtkCellArray* cloneFaceLocations = clone->GetPolyhedronFaceLocations();
  vtkCellArray* cloneFaces = clone->GetPolyhedronFaces();

  vtkIdList* cellRedirectionMap = info.OutputToInputCellIdRedirectionMap;
  vtkIdList* pointRedirectionMap = info.OutputToInputPointIdRedirectionMap;

  // Protection against mismatch storage
  if (ugFaceLocations->IsStorage64Bit() != ugFaces->IsStorage64Bit())
  {
    ugFaceLocations->ConvertTo64BitStorage();
    ugFaces->ConvertTo64BitStorage();
  }
  if (cloneFaceLocations->IsStorage64Bit() != cloneFaces->IsStorage64Bit())
  {
    cloneFaceLocations->ConvertTo64BitStorage();
    cloneFaces->ConvertTo64BitStorage();
  }

  // compute storage mask
  int mask = static_cast<int>(ugFaceLocations->IsStorage64Bit()) |
    (static_cast<int>(cloneFaceLocations->IsStorage64Bit()) << 1);

  switch (mask)
  {
    case 0:
      ::DeepCopyPolyhedronImpl<ArrayType32, ArrayType32>(ugFaceLocations, ugFaces,
        cloneFaceLocations, cloneFaces, cellRedirectionMap, pointRedirectionMap);
      break;
    case 1:
      ::DeepCopyPolyhedronImpl<ArrayType64, ArrayType32>(ugFaceLocations, ugFaces,
        cloneFaceLocations, cloneFaces, cellRedirectionMap, pointRedirectionMap);
      break;
    case 2:
      ::DeepCopyPolyhedronImpl<ArrayType32, ArrayType64>(ugFaceLocations, ugFaces,
        cloneFaceLocations, cloneFaces, cellRedirectionMap, pointRedirectionMap);
      break;
    case 3:
      ::DeepCopyPolyhedronImpl<ArrayType64, ArrayType64>(ugFaceLocations, ugFaces,
        cloneFaceLocations, cloneFaces, cellRedirectionMap, pointRedirectionMap);
      break;
  }
}

//----------------------------------------------------------------------------
/**
 * We're doing a homebrewed shallow copy because we do not want to share any pointer with the input,
 * which is the case for unstructured grid cell connectivity information.
 */
void CloneUnstructuredGrid(
  vtkUnstructuredGrid* ug, vtkUnstructuredGrid* clone, ::UnstructuredGridInformation& info)
{
  ::CloneDataObject(ug, clone);
  ::ClonePointData(ug, clone, info);
  ::ClonePoints(ug, clone, info);
  ::CloneCellData(ug, clone, info);

  if (!ug->GetPolyhedronFaces() && clone->GetPolyhedronFaces() &&
    clone->GetPolyhedronFaceLocations())
  {
    // initialize empty polyhedron when none are present but they are required for output.
    using ArrayType32 = vtkCellArray::ArrayType32;
    using ArrayType64 = vtkCellArray::ArrayType64;

    if (clone->GetPolyhedronFaceLocations()->IsStorage64Bit())
    {
      auto faceLocRange =
        vtkArrayDownCast<ArrayType64>(clone->GetPolyhedronFaceLocations()->GetOffsetsArray());
      for (vtkIdType pos = 0; pos < ug->GetNumberOfCells() + 1; ++pos)
      {
        faceLocRange->SetValue(pos, 0);
      }
    }
    else
    {
      auto faceLocRange =
        vtkArrayDownCast<ArrayType32>(clone->GetPolyhedronFaceLocations()->GetOffsetsArray());
      for (vtkIdType pos = 0; pos < ug->GetNumberOfCells() + 1; ++pos)
      {
        faceLocRange->SetValue(pos, 0);
      }
    }

    if (clone->GetPolyhedronFaces()->IsStorage64Bit())
    {
      auto faceLocRange =
        vtkArrayDownCast<ArrayType64>(clone->GetPolyhedronFaces()->GetOffsetsArray());
      for (vtkIdType pos = 0;
           pos < clone->GetPolyhedronFaces()->GetOffsetsArray()->GetNumberOfTuples(); ++pos)
      {
        faceLocRange->SetValue(pos, 0);
      }
    }
    else
    {
      auto faceLocRange =
        vtkArrayDownCast<ArrayType32>(clone->GetPolyhedronFaces()->GetOffsetsArray());
      for (vtkIdType pos = 0;
           pos < clone->GetPolyhedronFaces()->GetOffsetsArray()->GetNumberOfTuples(); ++pos)
      {
        faceLocRange->SetValue(pos, 0);
      }
    }
  }

  if (vtkIdList* redirectionMap = info.OutputToInputCellIdRedirectionMap)
  {
    ::DeepCopyCells(
      ug->GetCells(), clone->GetCells(), redirectionMap, info.InputToOutputPointIdRedirectionMap);
    ug->GetCellTypesArray()->GetTuples(redirectionMap, clone->GetCellTypesArray());

    vtkCellArray* ugFaceLocations = ug->GetPolyhedronFaceLocations();
    if (clone->GetPolyhedronFaceLocations() && ugFaceLocations &&
      ugFaceLocations->GetNumberOfCells())
    {
      ::DeepCopyPolyhedrons(ug, clone, info);
    }
  }
  else
  {
    vtkCellArray* ugCellArray = ug->GetCells();
    vtkCellArray* cloneCellArray = clone->GetCells();
    vtkDataArray* ugConnectivity = ugCellArray->GetConnectivityArray();
    vtkDataArray* ugOffsets = ugCellArray->GetOffsetsArray();

    ugConnectivity->GetTuples(
      0, ugConnectivity->GetNumberOfTuples() - 1, cloneCellArray->GetConnectivityArray());
    ugOffsets->GetTuples(0, ugOffsets->GetNumberOfTuples() - 1, cloneCellArray->GetOffsetsArray());
    ug->GetCellTypesArray()->GetTuples(0, ug->GetNumberOfCells() - 1, clone->GetCellTypesArray());

    vtkCellArray* ugFaces = ug->GetPolyhedronFaces();

    if (clone->GetPolyhedronFaces() && ugFaces && ugFaces->GetNumberOfCells())
    {
      vtkDataArray* ugFaceConnectivity = ug->GetPolyhedronFaces()->GetConnectivityArray();
      vtkDataArray* ugFaceOffsets = ug->GetPolyhedronFaces()->GetOffsetsArray();
      vtkDataArray* ugPolyConnectivity = ug->GetPolyhedronFaceLocations()->GetConnectivityArray();
      vtkDataArray* ugPolyOffsets = ug->GetPolyhedronFaceLocations()->GetOffsetsArray();
      ugFaceConnectivity->GetTuples(0, ugFaceConnectivity->GetNumberOfTuples() - 1,
        clone->GetPolyhedronFaces()->GetConnectivityArray());
      ugFaceOffsets->GetTuples(
        0, ugFaceOffsets->GetNumberOfTuples() - 1, clone->GetPolyhedronFaces()->GetOffsetsArray());
      ugPolyConnectivity->GetTuples(0, ugPolyConnectivity->GetNumberOfTuples() - 1,
        clone->GetPolyhedronFaceLocations()->GetConnectivityArray());
      ugPolyOffsets->GetTuples(0, ugPolyOffsets->GetNumberOfTuples() - 1,
        clone->GetPolyhedronFaceLocations()->GetOffsetsArray());
    }
  }
}

//----------------------------------------------------------------------------
void ClonePolyData(vtkPolyData* pd, vtkPolyData* clone, ::PolyDataInformation& info)
{
  ::CloneDataObject(pd, clone);
  ::ClonePointData(pd, clone, info);
  ::ClonePoints(pd, clone, info);

  vtkIdType cloneNumberOfVerts = clone->GetNumberOfVerts();
  vtkIdType cloneNumberOfLines = clone->GetNumberOfLines();
  vtkIdType cloneNumberOfPolys = clone->GetNumberOfPolys();

  vtkIdType cloneLinesOffset = cloneNumberOfVerts;
  vtkIdType pdLinesOffset = info.NumberOfInputVerts;

  vtkIdType clonePolysOffset = cloneNumberOfLines + cloneLinesOffset;
  vtkIdType pdPolysOffset = info.NumberOfInputLines + pdLinesOffset;

  vtkIdType cloneStripsOffset = cloneNumberOfPolys + clonePolysOffset;
  vtkIdType pdStripsOffset = info.NumberOfInputPolys + pdPolysOffset;

  // We cannot use CloneCellData here because the cell data gets all stirred up in a vtkPolyData
  vtkCellData* cloneCellData = clone->GetCellData();
  vtkCellData* pdCellData = pd->GetCellData();
  cloneCellData->CopyAllOn();
  cloneCellData->CopyAllocate(pdCellData, clone->GetNumberOfCells());
  cloneCellData->SetNumberOfTuples(clone->GetNumberOfCells());

  if (vtkIdList* pointIds = info.InputToOutputPointIdRedirectionMap)
  {
    vtkIdList* vertIds = info.OutputToInputVertCellIdRedirectionMap;
    if (vertIds->GetNumberOfIds())
    {
      ::DeepCopyCells(pd->GetVerts(), clone->GetVerts(), vertIds, pointIds);
    }

    vtkIdList* lineIds = info.OutputToInputLineCellIdRedirectionMap;
    if (lineIds->GetNumberOfIds())
    {
      ::DeepCopyCells(pd->GetLines(), clone->GetLines(), lineIds, pointIds);
    }

    vtkIdList* polyIds = info.OutputToInputPolyCellIdRedirectionMap;
    if (polyIds->GetNumberOfIds())
    {
      ::DeepCopyCells(pd->GetPolys(), clone->GetPolys(), polyIds, pointIds);
    }

    vtkIdList* stripIds = info.OutputToInputStripCellIdRedirectionMap;
    if (stripIds->GetNumberOfIds())
    {
      ::DeepCopyCells(pd->GetStrips(), clone->GetStrips(), stripIds, pointIds);
    }

    vtkNew<vtkIdList> iotaVert;
    iotaVert->SetNumberOfIds(info.NumberOfInputVerts);
    std::iota(iotaVert->begin(), iotaVert->end(), 0);

    vtkNew<vtkIdList> iotaLine;
    iotaLine->SetNumberOfIds(info.NumberOfInputLines);
    std::iota(iotaLine->begin(), iotaLine->end(), cloneLinesOffset);

    vtkNew<vtkIdList> iotaPoly;
    iotaPoly->SetNumberOfIds(info.NumberOfInputPolys);
    std::iota(iotaPoly->begin(), iotaPoly->end(), clonePolysOffset);

    vtkNew<vtkIdList> iotaStrip;
    iotaStrip->SetNumberOfIds(info.NumberOfInputStrips);
    std::iota(iotaStrip->begin(), iotaStrip->end(), cloneStripsOffset);

    vtkNew<vtkIdList> iotaCell;
    iotaCell->SetNumberOfIds(info.NumberOfInputCells);
    std::iota(iotaCell->begin(), iotaCell->begin() + info.NumberOfInputVerts, 0);
    std::iota(iotaCell->begin() + pdLinesOffset,
      iotaCell->begin() + pdLinesOffset + info.NumberOfInputLines, cloneLinesOffset);
    std::iota(iotaCell->begin() + pdPolysOffset,
      iotaCell->begin() + pdPolysOffset + info.NumberOfInputPolys, clonePolysOffset);
    std::iota(iotaCell->begin() + pdStripsOffset,
      iotaCell->begin() + pdStripsOffset + info.NumberOfInputStrips, cloneStripsOffset);

    vtkIdList* cellIds = info.OutputToInputCellIdRedirectionMap;

    for (int arrayId = 0; arrayId < pdCellData->GetNumberOfArrays(); ++arrayId)
    {
      vtkAbstractArray* sourceArray = pdCellData->GetAbstractArray(arrayId);
      cloneCellData->GetAbstractArray(arrayId)->InsertTuples(iotaCell, cellIds, sourceArray);
    }
  }
  else
  {
    vtkCellArray* cloneVerts = clone->GetVerts();
    vtkCellArray* cloneLines = clone->GetLines();
    vtkCellArray* clonePolys = clone->GetPolys();
    vtkCellArray* cloneStrips = clone->GetStrips();

    vtkCellArray* pdPolys = pd->GetPolys();
    vtkDataArray* pdPolyConnectivity = pdPolys->GetConnectivityArray();
    vtkDataArray* pdPolyOffsets = pdPolys->GetOffsetsArray();

    vtkCellArray* pdStrips = pd->GetStrips();
    vtkDataArray* pdStripConnectivity = pdStrips->GetConnectivityArray();
    vtkDataArray* pdStripOffsets = pdStrips->GetOffsetsArray();

    vtkCellArray* pdLines = pd->GetLines();
    vtkDataArray* pdLineConnectivity = pdLines->GetConnectivityArray();
    vtkDataArray* pdLineOffsets = pdLines->GetOffsetsArray();

    clonePolys->GetConnectivityArray()->InsertTuples(
      0, pdPolyConnectivity->GetNumberOfTuples(), 0, pdPolyConnectivity);
    clonePolys->GetOffsetsArray()->InsertTuples(
      0, pdPolyOffsets->GetNumberOfTuples(), 0, pdPolyOffsets);

    cloneStrips->GetConnectivityArray()->InsertTuples(
      0, pdStripConnectivity->GetNumberOfTuples(), 0, pdStripConnectivity);
    cloneStrips->GetOffsetsArray()->InsertTuples(
      0, pdStripOffsets->GetNumberOfTuples(), 0, pdStripOffsets);

    cloneLines->GetConnectivityArray()->InsertTuples(
      0, pdLineConnectivity->GetNumberOfTuples(), 0, pdLineConnectivity);
    cloneLines->GetOffsetsArray()->InsertTuples(
      0, pdLineOffsets->GetNumberOfTuples(), 0, pdLineOffsets);

    cloneVerts->ShallowCopy(pd->GetVerts());

    for (int arrayId = 0; arrayId < cloneCellData->GetNumberOfArrays(); ++arrayId)
    {
      vtkAbstractArray* source = pdCellData->GetAbstractArray(arrayId);
      vtkAbstractArray* target = cloneCellData->GetAbstractArray(arrayId);

      target->InsertTuples(0, info.NumberOfInputVerts, 0, source);
      target->InsertTuples(cloneLinesOffset, info.NumberOfInputLines, pdLinesOffset, source);
      target->InsertTuples(clonePolysOffset, info.NumberOfInputPolys, pdPolysOffset, source);
      target->InsertTuples(cloneStripsOffset, info.NumberOfInputStrips, pdStripsOffset, source);
    }
  }
}

//----------------------------------------------------------------------------
void EnqueuePointData(const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId,
  vtkDataSet* input, vtkIdList* pointIds)
{
  vtkNew<vtkFieldData> pointData;
  vtkPointData* inputPointData = input->GetPointData();
  pointData->CopyStructure(inputPointData);
  pointData->SetNumberOfTuples(pointIds->GetNumberOfIds());

  for (int arrayId = 0; arrayId < pointData->GetNumberOfArrays(); ++arrayId)
  {
    inputPointData->GetAbstractArray(arrayId)->GetTuples(
      pointIds, pointData->GetAbstractArray(arrayId));
  }

  cp.enqueue<vtkFieldData*>(blockId, pointData);
}

//----------------------------------------------------------------------------
void EnqueueCellData(const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId,
  vtkDataSet* input, vtkIdList* cellIds)
{
  vtkNew<vtkFieldData> cellData;
  vtkCellData* inputCellData = input->GetCellData();
  cellData->CopyStructure(inputCellData);
  cellData->SetNumberOfTuples(cellIds->GetNumberOfIds());

  for (int arrayId = 0; arrayId < cellData->GetNumberOfArrays(); ++arrayId)
  {
    inputCellData->GetAbstractArray(arrayId)->GetTuples(
      cellIds, cellData->GetAbstractArray(arrayId));
  }

  cp.enqueue<vtkFieldData*>(blockId, cellData);
}

//----------------------------------------------------------------------------
template <class ArrayT>
void EnqueueDataArray(
  const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId, ArrayT* array)
{
  cp.enqueue<vtkDataArray*>(blockId, array);
}

//----------------------------------------------------------------------------
template <class ArrayT>
void EnqueueDataArray(
  const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId, ArrayT* array, vtkIdList* ids)
{
  if (!array)
  {
    cp.enqueue<vtkDataArray*>(blockId, nullptr);
    return;
  }

  vtkSmartPointer<ArrayT> subArray = vtkSmartPointer<ArrayT>::Take(array->NewInstance());
  subArray->SetNumberOfComponents(array->GetNumberOfComponents());
  subArray->SetNumberOfTuples(ids->GetNumberOfIds());
  array->GetTuples(ids, subArray);
  cp.enqueue<vtkDataArray*>(blockId, subArray);
}

//----------------------------------------------------------------------------
void EnqueuePoints(const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId,
  vtkPointSet* input, vtkIdList* pointIds)
{
  ::EnqueueDataArray(cp, blockId, input->GetPoints()->GetData(), pointIds);
}

//----------------------------------------------------------------------------
template <class DataSetT>
void EnqueueCellsForUnstructuredData(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId,
  typename ::DataSetTypeToBlockTypeConverter<
    DataSetT>::BlockType::BlockStructureType::TopologyBufferType& buffer);

//----------------------------------------------------------------------------
template <>
void EnqueueCellsForUnstructuredData<vtkUnstructuredGrid>(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, ::UnstructuredGridBlockStructure::TopologyBufferType& buffer)
{
  cp.enqueue<vtkDataArray*>(blockId, buffer.Types);
  cp.enqueue<vtkDataArray*>(blockId, buffer.CellArray->GetOffsetsArray());
  cp.enqueue<vtkDataArray*>(blockId, buffer.CellArray->GetConnectivityArray());
  if (buffer.Faces && buffer.FaceLocations)
  {
    cp.enqueue<vtkDataArray*>(blockId, buffer.Faces->GetOffsetsArray());
    cp.enqueue<vtkDataArray*>(blockId, buffer.Faces->GetConnectivityArray());
    cp.enqueue<vtkDataArray*>(blockId, buffer.FaceLocations->GetOffsetsArray());
    cp.enqueue<vtkDataArray*>(blockId, buffer.FaceLocations->GetConnectivityArray());
  }
  else
  {
    cp.enqueue<vtkDataArray*>(blockId, vtkSmartPointer<vtkIdTypeArray>(nullptr));
    cp.enqueue<vtkDataArray*>(blockId, vtkSmartPointer<vtkIdTypeArray>(nullptr));
    cp.enqueue<vtkDataArray*>(blockId, vtkSmartPointer<vtkIdTypeArray>(nullptr));
    cp.enqueue<vtkDataArray*>(blockId, vtkSmartPointer<vtkIdTypeArray>(nullptr));
  }
}

//----------------------------------------------------------------------------
template <>
void EnqueueCellsForUnstructuredData<vtkPolyData>(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, ::PolyDataBlockStructure::TopologyBufferType& buffer)
{
  vtkCellArray* polys = buffer.Polys;
  vtkCellArray* strips = buffer.Strips;
  vtkCellArray* lines = buffer.Lines;

  cp.enqueue<vtkDataArray*>(blockId, polys->GetOffsetsArray());
  cp.enqueue<vtkDataArray*>(blockId, polys->GetConnectivityArray());

  cp.enqueue<vtkDataArray*>(blockId, strips->GetOffsetsArray());
  cp.enqueue<vtkDataArray*>(blockId, strips->GetConnectivityArray());

  cp.enqueue<vtkDataArray*>(blockId, lines->GetOffsetsArray());
  cp.enqueue<vtkDataArray*>(blockId, lines->GetConnectivityArray());
}

//----------------------------------------------------------------------------
void EnqueuePointsIfNeeded(
  const diy::Master::ProxyWithLink&, const diy::BlockID&, vtkImageData*, vtkIdList*)
{
}

//----------------------------------------------------------------------------
void EnqueuePointsIfNeeded(
  const diy::Master::ProxyWithLink&, const diy::BlockID&, vtkRectilinearGrid*, vtkIdList*)
{
}

//----------------------------------------------------------------------------
void EnqueuePointsIfNeeded(const diy::Master::ProxyWithLink& cp, const diy::BlockID& blockId,
  vtkStructuredGrid* input, vtkIdList* pointIds)
{
  ::EnqueuePoints(cp, blockId, input, pointIds);
}

//----------------------------------------------------------------------------
template <class DataSetT>
void EnqueueGhostsForStructuredData(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, DataSetT* input,
  typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType* block)
{
  vtkSmartPointer<vtkIdList> cellIds =
    ::ComputeInputInterfaceCellIdsForStructuredData(block, blockId.gid, input);
  ::EnqueueCellData(cp, blockId, input, cellIds);

  vtkSmartPointer<vtkIdList> pointIds = ::ComputeInputInterfacePointIdsForStructuredData(
    block, blockId.gid, input, cp.gid() > blockId.gid /* crop */);
  ::EnqueuePointData(cp, blockId, input, pointIds);

  ::EnqueuePointsIfNeeded(cp, blockId, input, pointIds);
}

//----------------------------------------------------------------------------
template <class DataSetT>
void EnqueueGhostsForUnstructuredData(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, DataSetT* input,
  typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType* block)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  BlockStructureType& blockStructure = block->BlockStructures.at(blockId.gid);

  ::EnqueueCellData(cp, blockId, input, blockStructure.CellIdsToSend);
  ::EnqueueCellsForUnstructuredData<DataSetT>(cp, blockId, blockStructure.SendBuffer);

  auto globalPointIds = vtkArrayDownCast<vtkIdTypeArray>(input->GetPointData()->GetGlobalIds());

  vtkIdList* pointIds = blockStructure.PointIdsToSend;
  ::EnqueuePointData(cp, blockId, input, pointIds);
  ::EnqueuePoints(cp, blockId, input, pointIds);
  ::EnqueueDataArray(cp, blockId, globalPointIds, pointIds);

  // We enqueue interfacing points to blocks of lower id than us.
  // This sets point ownership.
  if (cp.gid() < blockId.gid)
  {
    vtkIdList* interfacePointIds = blockStructure.MatchingReceivedPointIds;
    ::EnqueuePointData(cp, blockId, input, interfacePointIds);
  }

  ::EnqueueDataArray(cp, blockId, blockStructure.SharedPointIds.GetPointer());
}

//----------------------------------------------------------------------------
void DequeueFieldData(
  const diy::Master::ProxyWithLink& cp, int gid, vtkSmartPointer<vtkFieldData>& fd)
{
  vtkFieldData* tmp = nullptr;
  cp.dequeue<vtkFieldData*>(gid, tmp);
  fd = vtkSmartPointer<vtkFieldData>::Take(tmp);
}

//----------------------------------------------------------------------------
template <class BlockStructureT>
void DequeueCellsForUnstructuredData(
  const diy::Master::ProxyWithLink& cp, int gid, BlockStructureT& blockStructure);

//----------------------------------------------------------------------------
template <>
void DequeueCellsForUnstructuredData<::UnstructuredGridBlockStructure>(
  const diy::Master::ProxyWithLink& cp, int gid, ::UnstructuredGridBlockStructure& blockStructure)
{
  ::UnstructuredGridBlockStructure::TopologyBufferType& buffer = blockStructure.ReceiveBuffer;

  vtkDataArray* types = nullptr;
  vtkDataArray* offsets = nullptr;
  vtkDataArray* connectivity = nullptr;
  vtkDataArray* faces_offsets = nullptr;
  vtkDataArray* faces = nullptr;
  vtkDataArray* faceLocations_offsets = nullptr;
  vtkDataArray* faceLocations = nullptr;

  cp.dequeue<vtkDataArray*>(gid, types);
  cp.dequeue<vtkDataArray*>(gid, offsets);
  cp.dequeue<vtkDataArray*>(gid, connectivity);
  cp.dequeue<vtkDataArray*>(gid, faces_offsets);
  cp.dequeue<vtkDataArray*>(gid, faces);
  cp.dequeue<vtkDataArray*>(gid, faceLocations_offsets);
  cp.dequeue<vtkDataArray*>(gid, faceLocations);

  buffer.Types =
    vtkSmartPointer<vtkUnsignedCharArray>::Take(vtkArrayDownCast<vtkUnsignedCharArray>(types));

  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  if (faces && faces_offsets)
  {
    buffer.Faces = vtkSmartPointer<vtkCellArray>::New();
    if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(faces_offsets))
    {
      buffer.Faces->SetData(offsets32, vtkArrayDownCast<ArrayType32>(faces));
    }
    else
    {
      buffer.Faces->SetData(
        vtkArrayDownCast<ArrayType64>(faces_offsets), vtkArrayDownCast<ArrayType64>(faces));
    }
    faces_offsets->FastDelete();
    faces->FastDelete();
  }

  if (faceLocations && faceLocations_offsets)
  {
    buffer.FaceLocations = vtkSmartPointer<vtkCellArray>::New();
    if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(faceLocations_offsets))
    {
      buffer.FaceLocations->SetData(offsets32, vtkArrayDownCast<ArrayType32>(faceLocations));
    }
    else
    {
      buffer.FaceLocations->SetData(vtkArrayDownCast<ArrayType64>(faceLocations_offsets),
        vtkArrayDownCast<ArrayType64>(faceLocations));
    }
    faceLocations_offsets->FastDelete();
    faceLocations->FastDelete();
  }

  if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(offsets))
  {
    buffer.CellArray->SetData(offsets32, vtkArrayDownCast<ArrayType32>(connectivity));
  }
  else
  {
    buffer.CellArray->SetData(
      vtkArrayDownCast<ArrayType64>(offsets), vtkArrayDownCast<ArrayType64>(connectivity));
  }

  offsets->FastDelete();
  connectivity->FastDelete();
}

//----------------------------------------------------------------------------
template <>
void DequeueCellsForUnstructuredData<::PolyDataBlockStructure>(
  const diy::Master::ProxyWithLink& cp, int gid, ::PolyDataBlockStructure& blockStructure)
{
  ::PolyDataBlockStructure::TopologyBufferType& buffer = blockStructure.ReceiveBuffer;

  vtkDataArray* polyOffsets = nullptr;
  vtkDataArray* polyConnectivity = nullptr;
  vtkDataArray* stripOffsets = nullptr;
  vtkDataArray* stripConnectivity = nullptr;
  vtkDataArray* lineOffsets = nullptr;
  vtkDataArray* lineConnectivity = nullptr;

  cp.dequeue<vtkDataArray*>(gid, polyOffsets);
  cp.dequeue<vtkDataArray*>(gid, polyConnectivity);
  cp.dequeue<vtkDataArray*>(gid, stripOffsets);
  cp.dequeue<vtkDataArray*>(gid, stripConnectivity);
  cp.dequeue<vtkDataArray*>(gid, lineOffsets);
  cp.dequeue<vtkDataArray*>(gid, lineConnectivity);

  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(polyOffsets))
  {
    buffer.Polys->SetData(offsets32, vtkArrayDownCast<ArrayType32>(polyConnectivity));
  }
  else
  {
    buffer.Polys->SetData(
      vtkArrayDownCast<ArrayType64>(polyOffsets), vtkArrayDownCast<ArrayType64>(polyConnectivity));
  }

  if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(stripOffsets))
  {
    buffer.Strips->SetData(offsets32, vtkArrayDownCast<ArrayType32>(stripConnectivity));
  }
  else
  {
    buffer.Strips->SetData(vtkArrayDownCast<ArrayType64>(stripOffsets),
      vtkArrayDownCast<ArrayType64>(stripConnectivity));
  }

  if (ArrayType32* offsets32 = vtkArrayDownCast<ArrayType32>(lineOffsets))
  {
    buffer.Lines->SetData(offsets32, vtkArrayDownCast<ArrayType32>(lineConnectivity));
  }
  else
  {
    buffer.Lines->SetData(
      vtkArrayDownCast<ArrayType64>(lineOffsets), vtkArrayDownCast<ArrayType64>(lineConnectivity));
  }

  polyOffsets->FastDelete();
  polyConnectivity->FastDelete();
  stripOffsets->FastDelete();
  stripConnectivity->FastDelete();
  lineOffsets->FastDelete();
  lineConnectivity->FastDelete();
}

//----------------------------------------------------------------------------
void DequeuePoints(const diy::Master::ProxyWithLink& cp, int gid, vtkPoints* points)
{
  vtkDataArray* tmp = nullptr;
  cp.dequeue<vtkDataArray*>(gid, tmp);
  if (tmp)
  {
    points->SetData(tmp);
    tmp->FastDelete();
  }
}

//----------------------------------------------------------------------------
template <class ArrayT>
void DequeueDataArray(const diy::Master::ProxyWithLink& cp, int gid, vtkSmartPointer<ArrayT>& array)
{
  vtkDataArray* inArray = nullptr;
  cp.dequeue<vtkDataArray*>(gid, inArray);
  array = vtkSmartPointer<vtkIdTypeArray>::Take(vtkArrayDownCast<ArrayT>(inArray));
}

//----------------------------------------------------------------------------
template <class BlockStructureT>
void DequeueGhostsForStructuredData(
  const diy::Master::ProxyWithLink& cp, int gid, BlockStructureT& blockStructure)
{
  ::DequeueFieldData(cp, gid, blockStructure.GhostCellData);
  ::DequeueFieldData(cp, gid, blockStructure.GhostPointData);
}

//----------------------------------------------------------------------------
template <class BlockStructureT>
void DequeueGhostsForUnstructuredData(
  const diy::Master::ProxyWithLink& cp, int gid, BlockStructureT& blockStructure)
{
  ::DequeueFieldData(cp, gid, blockStructure.GhostCellData);
  ::DequeueCellsForUnstructuredData(cp, gid, blockStructure);

  ::DequeueFieldData(cp, gid, blockStructure.GhostPointData);
  ::DequeuePoints(cp, gid, blockStructure.GhostPoints);
  ::DequeueDataArray(cp, gid, blockStructure.GhostGlobalPointIds);

  // We dequeue interfacing points that are only sent by blocks of higher gid than us
  if (cp.gid() > gid)
  {
    ::DequeueFieldData(cp, gid, blockStructure.InterfacingPointData);
  }

  ::DequeueDataArray(cp, gid, blockStructure.ReceivedSharedPointIds);
}

//----------------------------------------------------------------------------
template <class GridDataSetT>
void DeepCopyInputAndAllocateGhostsForStructuredData(
  typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType* block, GridDataSetT* input,
  GridDataSetT* output)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType;
  using BlockInformationType = typename BlockType::InformationType;

  if (!::IsExtentValid(input->GetExtent()))
  {
    output->ShallowCopy(input);
    return;
  }

  BlockInformationType& info = block->Information;
  ::UpdateOutputGridStructure(output, info);

  ::CloneGrid(input, output, info.Extent);
}

//============================================================================
/**
 * This functor appends the cell buffers (connectivity + offset + polyhedron faces) to add the
 * geometry that has been sent by one block neighbor.
 *
 * Noteworthy parameters:
 * - matchingReceivedPointIds: This lists the ids of our external surface that match the interface
 *   of a neighboring block. We need those points to connect the interfacing cells of this block.
 * - redirectionMapForDuplicatePointIds: Maps to our output points, the points that have been sent
 *   by the current block neighbor and that have already been added to our point list by another
 *   connected block.
 * - pointIdOffsetIntervals: This map maps output point id to the number of points of lower id that
 *   are duplicate in source points. This allows to keep track of where the target point id should
 *   be in the target arrays given a source point id: just subtract the lower bound of this map.
 * - PointIdOffset: This is the number of points already present in our output points before adding
 *   the ghosts from this neighboring block.
 * - CellIdOffset: This is the number of cells already present in our output cells before adding the
 *   ghosts from this neighboring block.
 * - ConnectivityOffset: This is the current size of the connectivity array, before adding ghosts
 *   from their neighboring block.
 */
template <class ArrayT>
struct CellArrayInserter
{
  CellArrayInserter(vtkCellArray* srcCells, vtkCellArray* dstCells,
    vtkIdList* matchingReceivedPointIds,
    const std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds,
    const std::map<vtkIdType, vtkIdType>& pointIdOffsetIntervals, vtkIdType numberOfPointsInDest,
    vtkIdType numberOfCellsInDest, vtkIdType connectivitySizeInDest)
    : SourceCells(srcCells)
    , DestCells(dstCells)
    , MatchingReceivedPointIds(matchingReceivedPointIds)
    , RedirectionMapForDuplicatePointIds(redirectionMapForDuplicatePointIds)
    , PointIdOffsetIntervals(pointIdOffsetIntervals)
    , PointIdOffset(numberOfPointsInDest)
    , CellIdOffset(numberOfCellsInDest)
    , ConnectivityOffset(connectivitySizeInDest)
  {
    ArrayT* offsetsDest = vtkArrayDownCast<ArrayT>(this->DestCells->GetOffsetsArray());
    ArrayT* offsetsSource = vtkArrayDownCast<ArrayT>(this->SourceCells->GetOffsetsArray());

    // Last location of offsets will never be set in the loop, as it has
    // numberOfCells + 1 values.
    offsetsDest->SetValue(this->CellIdOffset + this->SourceCells->GetNumberOfCells(),
      offsetsDest->GetValue(this->CellIdOffset) +
        offsetsSource->GetValue(this->SourceCells->GetNumberOfCells()));
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    ArrayT* offsetsSource = vtkArrayDownCast<ArrayT>(this->SourceCells->GetOffsetsArray());
    ArrayT* connectivitySource =
      vtkArrayDownCast<ArrayT>(this->SourceCells->GetConnectivityArray());
    ArrayT* offsetsDest = vtkArrayDownCast<ArrayT>(this->DestCells->GetOffsetsArray());
    ArrayT* connectivityDest = vtkArrayDownCast<ArrayT>(this->DestCells->GetConnectivityArray());

    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      vtkIdType offset = offsetsSource->GetValue(cellId);
      vtkIdType nextOffset = offsetsSource->GetValue(cellId + 1);
      offsetsDest->SetValue(this->CellIdOffset + cellId, this->ConnectivityOffset + offset);

      for (vtkIdType id = offset; id < nextOffset; ++id)
      {
        vtkIdType pointId = connectivitySource->GetValue(id);
        if (pointId >= 0)
        {
          if (this->RedirectionMapForDuplicatePointIds.empty())
          {
            // If we do not have duplicate points, we just add the received point naively.
            connectivityDest->SetValue(
              this->ConnectivityOffset + id, this->PointIdOffset + pointId);
          }
          else
          {
            // If we do have duplicates, we look if the current point id is a duplicate or not
            auto it = this->RedirectionMapForDuplicatePointIds.find(pointId);
            if (it == this->RedirectionMapForDuplicatePointIds.end())
            {
              // Here, pointId is not a duplicate, so we can add the received point almost
              // normally. We just have to watch out for the induced offset that previous duplicate
              // points might have caused.
              connectivityDest->SetValue(this->ConnectivityOffset + id,
                this->PointIdOffset + pointId -
                  this->PointIdOffsetIntervals.lower_bound(pointId)->second);
            }
            else
            {
              // If pointId is a duplicate, we already own a copy of this point, and its index
              // is stored in the iterator we just fetched.
              connectivityDest->SetValue(this->ConnectivityOffset + id, it->second);
            }
          }
        }
        else if (-pointId - 1 < this->MatchingReceivedPointIds->GetNumberOfIds())
        {
          // In this case, we already own a copy of this point. It is on the interfacing surface
          // between us and the block who sent us those ids. We have to retrieve where this point
          // is located.
          // We tagged those points by giving them a negative id.
          connectivityDest->SetValue(
            this->ConnectivityOffset + id, this->MatchingReceivedPointIds->GetId(-pointId - 1));
        }
        else
        {
          vtkLog(ERROR,
            "Wrong output geometry... Ghosts should not be trusted."
              << " This is likely due to asymmetry between data shared between the partitions.");
          connectivityDest->SetValue(this->ConnectivityOffset + id, 0);
        }
      }
    }
  }

  vtkCellArray* SourceCells;
  vtkCellArray* DestCells;
  vtkIdList* MatchingReceivedPointIds;
  const std::map<vtkIdType, vtkIdType>& RedirectionMapForDuplicatePointIds;
  const std::map<vtkIdType, vtkIdType>& PointIdOffsetIntervals;
  vtkIdType PointIdOffset;
  vtkIdType CellIdOffset;
  vtkIdType ConnectivityOffset;
};

//----------------------------------------------------------------------------
template <class ArrayT>
void InsertCells(vtkCellArray* srcCells, vtkCellArray* dstCells,
  vtkIdList* matchingReceivedPointIds,
  const std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds,
  const std::map<vtkIdType, vtkIdType>& pointIdOffsetIntervals, vtkIdType numberOfPointsInDest,
  vtkIdType numberOfCellsInDest, vtkIdType connectivitySizeInDest)
{
  ::CellArrayInserter<ArrayT> inserter(srcCells, dstCells, matchingReceivedPointIds,
    redirectionMapForDuplicatePointIds, pointIdOffsetIntervals, numberOfPointsInDest,
    numberOfCellsInDest, connectivitySizeInDest);
  vtkSMPTools::For(0, srcCells->GetNumberOfCells(), inserter);
}

//----------------------------------------------------------------------------
void InsertCells(vtkCellArray* srcCells, vtkCellArray* dstCells,
  vtkIdList* matchingReceivedPointIds,
  const std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds,
  const std::map<vtkIdType, vtkIdType>& pointIdOffsetIntervals, vtkIdType numberOfPointsInDest,
  vtkIdType numberOfCellsInDest, vtkIdType connectivitySizeInDest)
{
  using ArrayType32 = vtkCellArray::ArrayType32;
  using ArrayType64 = vtkCellArray::ArrayType64;

  if (!srcCells->GetNumberOfCells())
  {
    return;
  }

  if (srcCells->IsStorage64Bit())
  {
    ::InsertCells<ArrayType64>(srcCells, dstCells, matchingReceivedPointIds,
      redirectionMapForDuplicatePointIds, pointIdOffsetIntervals, numberOfPointsInDest,
      numberOfCellsInDest, connectivitySizeInDest);
  }
  else
  {
    ::InsertCells<ArrayType32>(srcCells, dstCells, matchingReceivedPointIds,
      redirectionMapForDuplicatePointIds, pointIdOffsetIntervals, numberOfPointsInDest,
      numberOfCellsInDest, connectivitySizeInDest);
  }
}

//============================================================================
template <class ArrayT>
struct PolyhedronsInserter
{
  PolyhedronsInserter(vtkCellArray* srcFaceLocations, vtkCellArray* srcFaces,
    vtkCellArray* dstFaceLocations, vtkCellArray* dstFaces, vtkIdList* matchingReceivedPointIds,
    const std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds,
    const std::map<vtkIdType, vtkIdType>& pointIdOffsetIntervals, vtkIdType pointIdOffset,
    vtkIdType cellIdOffset, vtkIdType facesIdOffset, vtkIdType facesOffset)
    : SourceFaceLocations(srcFaceLocations)
    , SourceFaces(srcFaces)
    , DestFaceLocations(dstFaceLocations)
    , DestFaces(dstFaces)
    , MatchingReceivedPointIds(matchingReceivedPointIds)
    , RedirectionMapForDuplicatePointIds(redirectionMapForDuplicatePointIds)
    , PointIdOffsetIntervals(pointIdOffsetIntervals)
    , PointIdOffset(pointIdOffset)
    , CellIdOffset(cellIdOffset)
    , FacesIdOffset(facesIdOffset)
    , FacesOffset(facesOffset)
  {
    ArrayT* offsetsFacesDest = vtkArrayDownCast<ArrayT>(this->DestFaces->GetOffsetsArray());
    ArrayT* offsetsFacesSource = vtkArrayDownCast<ArrayT>(this->SourceFaces->GetOffsetsArray());
    ArrayT* offsetsFaceLocDest =
      vtkArrayDownCast<ArrayT>(this->DestFaceLocations->GetOffsetsArray());
    ArrayT* offsetsFaceLocSource =
      vtkArrayDownCast<ArrayT>(this->SourceFaceLocations->GetOffsetsArray());

    // Last location of offsets will never be set in the loop, as it has
    // numberOfCells + 1 values.
    offsetsFaceLocDest->SetValue(this->CellIdOffset + this->SourceFaceLocations->GetNumberOfCells(),
      offsetsFaceLocDest->GetValue(this->CellIdOffset) +
        offsetsFaceLocSource->GetValue(this->SourceFaceLocations->GetNumberOfCells()));
    offsetsFacesDest->SetValue(this->FacesIdOffset + this->SourceFaces->GetNumberOfCells(),
      offsetsFacesDest->GetValue(this->FacesIdOffset) +
        offsetsFacesSource->GetValue(this->SourceFaces->GetNumberOfCells()));
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    ArrayT* srcOffsets = vtkArrayDownCast<ArrayT>(this->SourceFaceLocations->GetOffsetsArray());
    ArrayT* srcConnectivity =
      vtkArrayDownCast<ArrayT>(this->SourceFaceLocations->GetConnectivityArray());
    ArrayT* srcFacesOffsets = vtkArrayDownCast<ArrayT>(this->SourceFaces->GetOffsetsArray());
    ArrayT* srcFacesConnectivity =
      vtkArrayDownCast<ArrayT>(this->SourceFaces->GetConnectivityArray());
    //
    ArrayT* destOffsets = vtkArrayDownCast<ArrayT>(this->DestFaceLocations->GetOffsetsArray());
    ArrayT* destConnectivity =
      vtkArrayDownCast<ArrayT>(this->DestFaceLocations->GetConnectivityArray());
    ArrayT* destFacesOffsets = vtkArrayDownCast<ArrayT>(this->DestFaces->GetOffsetsArray());
    ArrayT* destFacesConnectivity =
      vtkArrayDownCast<ArrayT>(this->DestFaces->GetConnectivityArray());

    for (vtkIdType cellId = startId; cellId < endId; ++cellId)
    {
      vtkIdType offset = srcOffsets->GetValue(cellId);
      vtkIdType nextOffset = srcOffsets->GetValue(cellId + 1);
      destOffsets->SetValue(this->CellIdOffset + cellId, this->FacesIdOffset + offset);

      // We enter the following if statement if current cell is a VTK_POLYHEDRON
      if ((nextOffset - offset) > 0)
      {
        for (vtkIdType id = offset; id < nextOffset; ++id)
        {
          vtkIdType faceId = srcConnectivity->GetValue(id);
          //
          vtkIdType faceOffset = srcFacesOffsets->GetValue(faceId);
          vtkIdType nextFaceOffset = srcFacesOffsets->GetValue(faceId + 1);
          //
          destConnectivity->SetValue(this->FacesIdOffset + id, this->FacesIdOffset + id);
          destFacesOffsets->SetValue(this->FacesIdOffset + id, this->FacesOffset + faceOffset);
          //
          for (vtkIdType locPt = faceOffset; locPt < nextFaceOffset; ++locPt)
          {
            vtkIdType pointId = srcFacesConnectivity->GetValue(locPt);
            if (pointId >= 0)
            {
              if (this->RedirectionMapForDuplicatePointIds.empty())
              {
                destFacesConnectivity->SetValue(
                  this->FacesOffset + locPt, this->PointIdOffset + pointId);
              }
              else
              {
                auto it = this->RedirectionMapForDuplicatePointIds.find(pointId);
                if (it == this->RedirectionMapForDuplicatePointIds.end())
                {
                  destFacesConnectivity->SetValue(this->FacesOffset + locPt,
                    this->PointIdOffset + pointId -
                      this->PointIdOffsetIntervals.lower_bound(pointId)->second);
                }
                else
                {
                  destFacesConnectivity->SetValue(this->FacesOffset + locPt, it->second);
                }
              }
            }
            else if (-pointId - 1 < this->MatchingReceivedPointIds->GetNumberOfIds())
            {
              // In this case, we already own a copy of this point. It is on the interfacing surface
              // between us and the block who sent us those ids. We have to retrieve where this
              // point is located. We tagged those points by giving them a negative id.
              destFacesConnectivity->SetValue(
                this->FacesOffset + locPt, this->MatchingReceivedPointIds->GetId(-pointId - 1));
            }
            else
            {
              vtkLog(ERROR,
                "Wrong output geometry... Ghosts should not be trusted."
                  << " This is likely due to asymmetry between data shared between the "
                     "partitions.");
              destFacesConnectivity->SetValue(this->FacesOffset + locPt, 0);
            }
          }
        }
      }
    }
  }

  vtkCellArray* SourceFaceLocations;
  vtkCellArray* SourceFaces;
  vtkCellArray* DestFaceLocations;
  vtkCellArray* DestFaces;
  vtkIdList* MatchingReceivedPointIds;
  const std::map<vtkIdType, vtkIdType>& RedirectionMapForDuplicatePointIds;
  const std::map<vtkIdType, vtkIdType>& PointIdOffsetIntervals;
  vtkIdType PointIdOffset;
  vtkIdType CellIdOffset;
  vtkIdType FacesIdOffset;
  vtkIdType FacesOffset;
};

//============================================================================
/**
 * This worker is used to checked if 2 points are the same, using the underlying type of the point.
 */
struct QueryPointWorker
{
  QueryPointWorker(vtkAbstractPointLocator* locator)
    : Locator(locator)
  {
  }

  template <class ArrayT, class ValueT = typename ArrayT::ValueType>
  void operator()(ArrayT* vtkNotUsed(array), double p[3])
  {
    this->TargetPointId = this->Locator->FindClosestPointWithinRadius(
      vtkDIYGhostUtilities_detail::ComputePrecision<ValueT>(
        std::max({ std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2]) })),
      p, this->Dist2);
  }

  vtkAbstractPointLocator* Locator;
  vtkIdType TargetPointId;
  double Dist2;
};

//----------------------------------------------------------------------------
void DeepCopyInputAndAllocateGhosts(
  ::UnstructuredGridBlock* block, vtkUnstructuredGrid* input, vtkUnstructuredGrid* output)
{
  using BlockType = ::UnstructuredGridBlock;
  using BlockStructureType = typename BlockType::BlockStructureType;
  using BlockInformationType = typename BlockType::InformationType;

  BlockInformationType& info = block->Information;

  vtkIdType numberOfPoints = info.NumberOfInputPoints;
  vtkIdType numberOfCells = info.NumberOfInputCells;

  vtkIdType connectivitySize = info.InputConnectivitySize;
  vtkIdType facesSize = info.InputFacesSize;
  vtkIdType facesNum = info.InputNumberOfFaces;

  for (auto& pair : block->BlockStructures)
  {
    BlockStructureType& blockStructure = pair.second;
    if (blockStructure.GhostPoints)
    {
      numberOfPoints += blockStructure.GhostPoints->GetNumberOfPoints() -
        static_cast<vtkIdType>(blockStructure.RedirectionMapForDuplicatePointIds.size());
    }
    numberOfCells += blockStructure.ReceiveBuffer.Types->GetNumberOfValues();
    connectivitySize +=
      blockStructure.ReceiveBuffer.CellArray->GetConnectivityArray()->GetNumberOfValues();
    vtkCellArray* faces = blockStructure.ReceiveBuffer.Faces;
    facesSize += faces ? faces->GetConnectivityArray()->GetNumberOfValues() : 0;
    facesNum += faces ? faces->GetOffsetsArray()->GetNumberOfValues() - 1 : 0;
  }

  vtkPoints* inputPoints = input->GetPoints();
  vtkNew<vtkPoints> outputPoints;
  if (inputPoints)
  {
    outputPoints->SetDataType(inputPoints->GetDataType());
  }
  outputPoints->SetNumberOfPoints(numberOfPoints);
  output->SetPoints(outputPoints);

  vtkNew<vtkCellArray> outputCellArray;

  vtkNew<vtkUnsignedCharArray> types;
  types->SetNumberOfValues(numberOfCells);

  vtkSmartPointer<vtkCellArray> outputFaces = nullptr;
  vtkSmartPointer<vtkCellArray> outputFaceLocations = nullptr;

  if (facesSize)
  {
    outputFaces = vtkSmartPointer<vtkCellArray>::New();
    outputFaces->AllocateExact(facesNum, facesSize);
    outputFaces->GetConnectivityArray()->SetNumberOfTuples(facesSize);
    outputFaces->GetOffsetsArray()->SetNumberOfTuples(facesNum + 1);
    outputFaceLocations = vtkSmartPointer<vtkCellArray>::New();
    outputFaceLocations->AllocateExact(numberOfCells, facesNum);
    outputFaceLocations->GetConnectivityArray()->SetNumberOfTuples(facesNum);
    outputFaceLocations->GetOffsetsArray()->SetNumberOfTuples(numberOfCells + 1);
  }

// We're being careful to account for different storage options in cell arrays
#ifdef VTK_USE_64BIT_IDS
  if (!(numberOfPoints >> 31) || !(connectivitySize >> 31))
  {
    outputCellArray->ConvertTo32BitStorage();
  }
#endif

  outputCellArray->GetConnectivityArray()->SetNumberOfTuples(connectivitySize);
  outputCellArray->GetOffsetsArray()->SetNumberOfTuples(numberOfCells + 1);

  output->SetPolyhedralCells(types, outputCellArray, outputFaceLocations, outputFaces);

  ::CloneUnstructuredGrid(input, output, info);
}

//----------------------------------------------------------------------------
void DeepCopyInputAndAllocateGhosts(::PolyDataBlock* block, vtkPolyData* input, vtkPolyData* output)
{
  using BlockType = ::PolyDataBlock;
  using BlockStructureType = typename BlockType::BlockStructureType;
  using BlockInformationType = typename BlockType::InformationType;

  BlockInformationType& info = block->Information;

  vtkIdType numberOfPoints = info.NumberOfInputPoints;

  vtkIdType polyConnectivitySize = info.InputPolyConnectivitySize;
  vtkIdType stripConnectivitySize = info.InputStripConnectivitySize;
  vtkIdType lineConnectivitySize = info.InputLineConnectivitySize;

  vtkIdType polyOffsetsSize = info.NumberOfInputPolys;
  vtkIdType stripOffsetsSize = info.NumberOfInputStrips;
  vtkIdType lineOffsetsSize = info.NumberOfInputLines;

  for (auto& pair : block->BlockStructures)
  {
    BlockStructureType& blockStructure = pair.second;
    if (blockStructure.GhostPoints)
    {
      numberOfPoints += blockStructure.GhostPoints->GetNumberOfPoints() -
        static_cast<vtkIdType>(blockStructure.RedirectionMapForDuplicatePointIds.size());
    }

    auto& buffer = blockStructure.ReceiveBuffer;

    vtkCellArray* polys = buffer.Polys;
    vtkCellArray* strips = buffer.Strips;
    vtkCellArray* lines = buffer.Lines;

    vtkIdType numberOfPolyOffsets = polys->GetOffsetsArray()->GetNumberOfValues();
    vtkIdType numberOfStripOffsets = strips->GetOffsetsArray()->GetNumberOfValues();
    vtkIdType numberOfLineOffsets = lines->GetOffsetsArray()->GetNumberOfValues();

    vtkIdType numberOfPolys = numberOfPolyOffsets ? numberOfPolyOffsets - 1 : 0;
    vtkIdType numberOfStrips = numberOfStripOffsets ? numberOfStripOffsets - 1 : 0;
    vtkIdType numberOfLines = numberOfLineOffsets ? numberOfLineOffsets - 1 : 0;

    polyOffsetsSize += numberOfPolys;
    stripOffsetsSize += numberOfStrips;
    lineOffsetsSize += numberOfLines;

    polyConnectivitySize += polys->GetConnectivityArray()->GetNumberOfValues();
    stripConnectivitySize += strips->GetConnectivityArray()->GetNumberOfValues();
    lineConnectivitySize += lines->GetConnectivityArray()->GetNumberOfValues();
  }

  // Offsets array have exactly one more element than there are cells, if there are cells
  polyOffsetsSize += (polyOffsetsSize != 0);
  stripOffsetsSize += (stripOffsetsSize != 0);
  lineOffsetsSize += (lineOffsetsSize != 0);

  vtkPoints* inputPoints = input->GetPoints();
  vtkNew<vtkPoints> outputPoints;
  if (inputPoints)
  {
    outputPoints->SetDataType(inputPoints->GetDataType());
  }
  outputPoints->SetNumberOfPoints(numberOfPoints);
  output->SetPoints(outputPoints);

  vtkNew<vtkCellArray> outputPolys, outputStrips, outputLines;

// We're being careful to account for different storage options in cell arrays
#ifdef VTK_USE_64BIT_IDS
  if (!(numberOfPoints >> 31) || !(polyConnectivitySize >> 31) || !(stripConnectivitySize >> 31) ||
    !(lineConnectivitySize >> 31))
  {
    outputPolys->ConvertTo32BitStorage();
    outputStrips->ConvertTo32BitStorage();
    outputLines->ConvertTo32BitStorage();
  }
#endif

  outputPolys->GetConnectivityArray()->SetNumberOfTuples(polyConnectivitySize);
  outputPolys->GetOffsetsArray()->SetNumberOfTuples(polyOffsetsSize);

  outputStrips->GetConnectivityArray()->SetNumberOfTuples(stripConnectivitySize);
  outputStrips->GetOffsetsArray()->SetNumberOfTuples(stripOffsetsSize);

  outputLines->GetConnectivityArray()->SetNumberOfTuples(lineConnectivitySize);
  outputLines->GetOffsetsArray()->SetNumberOfTuples(lineOffsetsSize);

  if (polyOffsetsSize)
  {
    output->SetPolys(outputPolys);
  }
  if (stripOffsetsSize)
  {
    output->SetStrips(outputStrips);
  }
  if (lineOffsetsSize)
  {
    output->SetLines(outputLines);
  }

  ::ClonePolyData(input, output, info);
}

//----------------------------------------------------------------------------
template <class PointSetT>
void DeepCopyInputAndAllocateGhostsForUnstructuredData(
  typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType* block, PointSetT* input,
  PointSetT* output)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<PointSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;
  using BlockInformation = typename BlockType::InformationType;

  ::BlockMapType<BlockStructureType>& blockStructures = block->BlockStructures;
  BlockInformation& info = block->Information;

  if (!info.InterfacePoints)
  {
    output->ShallowCopy(input);
    return;
  }

  vtkIdType pointIdOffset = info.NumberOfInputPoints;

  vtkIdType numberOfReceivedSharedPoints = 0;
  for (auto& pair : blockStructures)
  {
    if (pair.second.ReceivedSharedPointIds)
    {
      numberOfReceivedSharedPoints += pair.second.ReceivedSharedPointIds->GetNumberOfValues();
    }
  }

  // This pointIdRedirection is used to redirect duplicate points that have been sent by multiple
  // blocks to their location in our local output points.
  std::vector<vtkIdType> pointIdRedirection;
  pointIdRedirection.reserve(numberOfReceivedSharedPoints);

  // We look at tagged duplicate points sent by our neighbors and see if they match previously
  // added points.
  // If they do, we store their current position in the output point array so we can redirect
  // cell connectivity to the correct point.
  //
  // We do all of that when we allocate because we want to know the exact number of points in the
  // output at this stage. Since this information can be useful in the future, we store relevant
  // information.

  if (info.InterfaceGlobalPointIds)
  {
    // This is the case when we use global ids instead of point positions
    std::unordered_map<vtkIdType, vtkIdType> pointIdLocator;

    for (auto& pair : blockStructures)
    {
      BlockStructureType& blockStructure = pair.second;
      if (!blockStructure.GhostGlobalPointIds || !blockStructure.ReceivedSharedPointIds)
      {
        continue;
      }
      auto globalIds = vtk::DataArrayValueRange<1>(blockStructure.GhostGlobalPointIds);
      std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds =
        blockStructure.RedirectionMapForDuplicatePointIds;

      auto sharedPointIds = vtk::DataArrayValueRange<1>(blockStructure.ReceivedSharedPointIds);
      using ConstRef = typename decltype(sharedPointIds)::ConstReferenceType;

      vtkIdType numberOfMatchingPoints = 0;

      for (ConstRef pointId : sharedPointIds)
      {
        ConstRef globalId = globalIds[pointId];

        if (pointIdLocator.empty())
        {
          pointIdLocator.emplace(globalId, 0);
          pointIdRedirection.push_back(pointIdOffset + pointId);
          continue;
        }

        auto it = pointIdLocator.find(globalId);
        if (it != pointIdLocator.end())
        {
          ++numberOfMatchingPoints;
          redirectionMapForDuplicatePointIds.emplace(pointId, pointIdRedirection[it->second]);
        }
        else
        {
          pointIdRedirection.push_back(pointIdOffset + pointId - numberOfMatchingPoints);
          pointIdLocator.emplace(globalId, pointIdLocator.size());
        }
      }
      pointIdOffset += globalIds.size() - numberOfMatchingPoints;
    }
  }
  else
  {
    // This is the case when we use point positions to match points.

    vtkNew<vtkIncrementalOctreePointLocator> pointLocator;
    vtkNew<vtkPoints> points;
    points->SetDataType(info.InterfacePoints->GetDataType());
    constexpr double inf = std::numeric_limits<double>::infinity();
    double bounds[6] = { inf, -inf, inf, -inf, inf, -inf };

    for (auto& pair : blockStructures)
    {
      BlockStructureType& blockStructure = pair.second;
      double* tmp = blockStructure.GhostPoints->GetBounds();
      bounds[0] = std::min(bounds[0], tmp[0]);
      bounds[1] = std::max(bounds[1], tmp[1]);
      bounds[2] = std::min(bounds[2], tmp[2]);
      bounds[3] = std::max(bounds[3], tmp[3]);
      bounds[4] = std::min(bounds[4], tmp[4]);
      bounds[5] = std::max(bounds[5], tmp[5]);
    }

    pointLocator->InitPointInsertion(points, bounds);

    ::QueryPointWorker queryPointWorker(pointLocator);

    for (auto& pair : blockStructures)
    {
      BlockStructureType& blockStructure = pair.second;
      vtkPoints* receivedPoints = blockStructure.GhostPoints;
      std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds =
        blockStructure.RedirectionMapForDuplicatePointIds;
      if (!blockStructure.ReceivedSharedPointIds || !receivedPoints)
      {
        continue;
      }
      auto sharedPointIds = vtk::DataArrayValueRange<1>(blockStructure.ReceivedSharedPointIds);
      using ConstRef = typename decltype(sharedPointIds)::ConstReferenceType;
      vtkIdType numberOfMatchingPoints = 0;
      for (ConstRef pointId : sharedPointIds)
      {
        double* p = receivedPoints->GetPoint(pointId);

        if (!points->GetNumberOfPoints())
        {
          pointLocator->InsertNextPoint(p);
          pointIdRedirection.push_back(pointIdOffset + pointId);
          continue;
        }

        using Dispatcher = vtkArrayDispatch::Dispatch;
        vtkDataArray* receivedPointsArray = receivedPoints->GetData();
        if (!Dispatcher::Execute(receivedPoints->GetData(), queryPointWorker, p))
        {
          queryPointWorker.template operator()<vtkDataArray, double>(receivedPointsArray, p);
        }

        if (queryPointWorker.TargetPointId != -1)
        {
          ++numberOfMatchingPoints;
          redirectionMapForDuplicatePointIds.insert(
            { pointId, pointIdRedirection[queryPointWorker.TargetPointId] });
        }
        else
        {
          pointIdRedirection.push_back(pointIdOffset + pointId - numberOfMatchingPoints);
          pointLocator->InsertNextPoint(p);
        }
      }
      pointIdOffset += receivedPoints->GetNumberOfPoints() - numberOfMatchingPoints;
    }
  }

  ::DeepCopyInputAndAllocateGhosts(block, input, output);
}

//----------------------------------------------------------------------------
/**
 * This function fills hidden ghosts in allocated ghost layers for grid data sets.
 * This step is essential to perform before filling duplicate because there might be junctions with
 * allocated ghosts but no grid to get data from. This can happen when adjacent faces are of
 * different size.
 */
template <class GridDataSetT>
void FillHiddenGhostsForStructuredData(
  const diy::Master& master, std::vector<GridDataSetT*>& outputs)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<GridDataSetT>::BlockType;

  static constexpr unsigned char CELL_GHOST_VALUE =
    vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL |
    vtkDataSetAttributes::CellGhostTypes::HIDDENCELL;
  static constexpr unsigned char POINT_GHOST_VALUE =
    vtkDataSetAttributes::PointGhostTypes::DUPLICATEPOINT |
    vtkDataSetAttributes::PointGhostTypes::HIDDENPOINT;

  for (int localId = 0; localId < static_cast<int>(outputs.size()); ++localId)
  {
    auto& output = outputs[localId];
    BlockType* block = master.block<BlockType>(localId);

    vtkUnsignedCharArray* ghostCellArray = block->GhostCellArray;
    vtkUnsignedCharArray* ghostPointArray = block->GhostPointArray;

    ::ExtentType localExtent;
    output->GetExtent(localExtent.data());

    const ::ExtentType& localExtentWithNoGhosts = block->Information.Extent;

    int isDimensionDegenerate[3] = { localExtent[0] == localExtent[1],
      localExtent[2] == localExtent[3], localExtent[4] == localExtent[5] };

    // We are careful and take into account when dimensions are degenerate:
    // we do not want to fill a degenerate dimension with ghosts.
    //
    // On each dimension, we have to fill each end of each segment on points and cells.
    // This is repeated for each dimension.
    if (!isDimensionDegenerate[0])
    {
      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtent[0],
        localExtentWithNoGhosts[0], localExtent[2], localExtent[3] + isDimensionDegenerate[1],
        localExtent[4], localExtent[5] + isDimensionDegenerate[2], CELL_GHOST_VALUE);

      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtentWithNoGhosts[1],
        localExtent[1], localExtent[2], localExtent[3] + isDimensionDegenerate[1], localExtent[4],
        localExtent[5] + isDimensionDegenerate[2], CELL_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output, localExtent[0],
        localExtentWithNoGhosts[0] - 1, localExtent[2], localExtent[3], localExtent[4],
        localExtent[5], POINT_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output,
        localExtentWithNoGhosts[1] + 1, localExtent[1], localExtent[2], localExtent[3],
        localExtent[4], localExtent[5], POINT_GHOST_VALUE);
    }
    if (!isDimensionDegenerate[1])
    {
      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtent[0],
        localExtent[1] + isDimensionDegenerate[0], localExtent[2], localExtentWithNoGhosts[2],
        localExtent[4], localExtent[5] + isDimensionDegenerate[2], CELL_GHOST_VALUE);

      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtent[0],
        localExtent[1] + isDimensionDegenerate[0], localExtentWithNoGhosts[3], localExtent[3],
        localExtent[4], localExtent[5] + isDimensionDegenerate[2], CELL_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output, localExtent[0],
        localExtent[1], localExtent[2], localExtentWithNoGhosts[2] - 1, localExtent[4],
        localExtent[5], POINT_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output, localExtent[0],
        localExtent[1], localExtentWithNoGhosts[3] + 1, localExtent[3], localExtent[4],
        localExtent[5], POINT_GHOST_VALUE);
    }
    if (!isDimensionDegenerate[2])
    {
      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtent[0],
        localExtent[1] + isDimensionDegenerate[0], localExtent[2],
        localExtent[3] + isDimensionDegenerate[1], localExtent[4], localExtentWithNoGhosts[4],
        CELL_GHOST_VALUE);

      ::TreatVirginGhostCellsForStructuredData(ghostCellArray, output, localExtent[0],
        localExtent[1] + isDimensionDegenerate[0], localExtent[2],
        localExtent[3] + isDimensionDegenerate[1], localExtentWithNoGhosts[5], localExtent[5],
        CELL_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output, localExtent[0],
        localExtent[1], localExtent[2], localExtent[3], localExtent[4],
        localExtentWithNoGhosts[4] - 1, POINT_GHOST_VALUE);

      ::TreatVirginGhostPointsForStructuredData(ghostPointArray, output, localExtent[0],
        localExtent[1], localExtent[2], localExtent[3], localExtentWithNoGhosts[5] + 1,
        localExtent[5], POINT_GHOST_VALUE);
    }
  }
}

//----------------------------------------------------------------------------
void FillReceivedGhostFieldData(
  vtkFieldData* sourceFD, vtkFieldData* destFD, vtkIdList* sourceIds, vtkIdList* destIds)
{
  if (!sourceFD || !sourceFD->GetNumberOfTuples())
  {
    return;
  }

  for (int arrayId = 0; arrayId < destFD->GetNumberOfArrays(); ++arrayId)
  {
    vtkAbstractArray* destArray = destFD->GetAbstractArray(arrayId);
    if (vtkAbstractArray* sourceArray = sourceFD->GetAbstractArray(destArray->GetName()))
    {
      destArray->InsertTuples(destIds, sourceIds, sourceArray);
    }
  }
}

//----------------------------------------------------------------------------
void FillReceivedGhostFieldDataForStructuredData(
  vtkFieldData* sourceFD, vtkFieldData* destFD, vtkIdList* ids)
{
  if (!sourceFD || !sourceFD->GetNumberOfTuples())
  {
    return;
  }

  vtkNew<vtkIdList> sourceIds;
  sourceIds->SetNumberOfIds(sourceFD->GetNumberOfTuples());
  std::iota(sourceIds->begin(), sourceIds->end(), 0);

  FillReceivedGhostFieldData(sourceFD, destFD, sourceIds, ids);
}

//----------------------------------------------------------------------------
void FillDuplicatePointGhostArrayForStructuredData(
  vtkUnsignedCharArray* ghostArray, vtkIdList* pointIds)
{
  for (vtkIdType i = 0; i < pointIds->GetNumberOfIds(); ++i)
  {
    vtkIdType pointId = pointIds->GetId(i);
    ghostArray->SetValue(pointId,
      ghostArray->GetValue(pointId) | vtkDataSetAttributes::PointGhostTypes::DUPLICATEPOINT);
  }
}

//----------------------------------------------------------------------------
void FillDuplicateCellGhostArrayForStructuredData(
  vtkUnsignedCharArray* ghostArray, vtkIdList* cellIds)
{
  for (vtkIdType i = 0; i < cellIds->GetNumberOfIds(); ++i)
  {
    vtkIdType cellId = cellIds->GetId(i);
    ghostArray->SetValue(
      cellId, ghostArray->GetValue(cellId) | vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL);
  }
}

//----------------------------------------------------------------------------
template <class BlockStructureT>
void FillDuplicatePointGhostArrayForUnstructureData(vtkUnsignedCharArray* ghostArray, int myGid,
  int gid, BlockStructureT& blockStructure, vtkIdType currentMaxPointId,
  vtkIdType numberOfAddedPoints)
{
  // We set our interfacing points with other blocks to be ghosts if the global id
  // of the corresponding block is lower than our global id.
  if (myGid > gid)
  {
    vtkIdList* pointIds = blockStructure.MatchingReceivedPointIds;
    for (vtkIdType id = 0; id < pointIds->GetNumberOfIds(); ++id)
    {
      vtkIdType pointId = pointIds->GetId(id);
      ghostArray->SetValue(pointId,
        ghostArray->GetValue(pointId) | vtkDataSetAttributes::PointGhostTypes::DUPLICATEPOINT);
    }
  }

  ::ArrayBinaryTagger<vtkUnsignedCharArray> filler(
    ghostArray, vtkDataSetAttributes::PointGhostTypes::DUPLICATEPOINT);

  vtkSMPTools::For(currentMaxPointId, currentMaxPointId + numberOfAddedPoints, filler);
}

//----------------------------------------------------------------------------
void FillDuplicateCellGhostArrayForUnstructureData(
  vtkUnsignedCharArray* ghostArray, vtkIdType currentMaxCellId, vtkIdType numberOfAddedCells)
{
  ::ArrayBinaryTagger<vtkUnsignedCharArray> filler(
    ghostArray, vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL);

  vtkSMPTools::For(currentMaxCellId, currentMaxCellId + numberOfAddedCells, filler);
}

//----------------------------------------------------------------------------
void FillReceivedGhostFieldData(vtkFieldData* sourceFD, vtkFieldData* destFD,
  vtkIdType currentNumberOfElements, vtkIdType numberOfAddedElements, vtkIdType sourceOffset = 0)
{
  if (!sourceFD)
  {
    return;
  }

  for (int arrayId = 0; arrayId < destFD->GetNumberOfArrays(); ++arrayId)
  {
    vtkAbstractArray* destArray = destFD->GetAbstractArray(arrayId);
    if (vtkAbstractArray* sourceArray = sourceFD->GetAbstractArray(destArray->GetName()))
    {
      destArray->InsertTuples(
        currentNumberOfElements, numberOfAddedElements, sourceOffset, sourceArray);
    }
  }
}

//----------------------------------------------------------------------------
template <class BlockT>
void RemoveDuplicatePointIds(
  BlockT* block, vtkIdList* pointIds, vtkIdList* pointIdsWithNoDuplicate, vtkIdList* remapping)
{
  vtkUnsignedCharArray* ghostPoints = block->GhostPointArray;
  pointIdsWithNoDuplicate->Allocate(pointIds->GetNumberOfIds());
  remapping->Allocate(pointIds->GetNumberOfIds());

  for (vtkIdType id = 0; id < pointIds->GetNumberOfIds(); ++id)
  {
    vtkIdType pointId = pointIds->GetId(id);
    // This ghost was not already set with a duplicate bit
    if (!(ghostPoints->GetValue(pointId) & vtkDataSetAttributes::DUPLICATEPOINT))
    {
      pointIdsWithNoDuplicate->InsertNextId(pointId);
      remapping->InsertNextId(id);
    }
  }
}

//----------------------------------------------------------------------------
void FillReceivedGhostPointsForStructuredDataIfNeeded(
  ::ImageDataBlockStructure&, vtkImageData*, vtkIdList*, vtkIdList*)
{
}

//----------------------------------------------------------------------------
void FillReceivedGhostPointsForStructuredDataIfNeeded(
  ::RectilinearGridBlockStructure&, vtkRectilinearGrid*, vtkIdList*, vtkIdList*)
{
}

//----------------------------------------------------------------------------
void FillReceivedGhostPointsForStructuredDataIfNeeded(
  ::StructuredGridBlockStructure& blockStructure, vtkStructuredGrid* output, vtkIdList* remapping,
  vtkIdList* pointIds)
{
  if (blockStructure.GhostPoints)
  {
    output->GetPoints()->GetData()->InsertTuples(
      pointIds, remapping, blockStructure.GhostPoints->GetData());
  }
}

//----------------------------------------------------------------------------
template <class StructuredDataSetT>
void FillReceivedGhostsForStructuredData(
  typename ::DataSetTypeToBlockTypeConverter<StructuredDataSetT>::BlockType* block, int myGid,
  int gid, StructuredDataSetT* output, int outputGhostLevels)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<StructuredDataSetT>::BlockType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  BlockStructureType& blockStructure = block->BlockStructures.at(gid);

  vtkSmartPointer<vtkIdList> pointIds = ::ComputeOutputInterfacePointIdsForStructuredData(
    blockStructure, output, myGid < gid /* crop */);

  // We're skipping points that were already set by another block. This ensures that the block of
  // lowest id stays the owner of any point that exists in multiple blocks.
  // We skip duplicate points by looking at the ghost array. A ghost already set to DUPLICATEPOINT
  // has already been set in the past.
  vtkNew<vtkIdList> pointIdsWithNoDuplicate, remapping;
  ::RemoveDuplicatePointIds(block, pointIds, pointIdsWithNoDuplicate, remapping);

  ::FillReceivedGhostFieldData(
    blockStructure.GhostPointData, output->GetPointData(), remapping, pointIdsWithNoDuplicate);

  ::FillReceivedGhostPointsForStructuredDataIfNeeded(
    blockStructure, output, remapping, pointIdsWithNoDuplicate);

  // It is important to this function after filling received ghosts.
  // If not, we might write over hidden ghosts from other blocks
  ::FillDuplicatePointGhostArrayForStructuredData(block->GhostPointArray, pointIdsWithNoDuplicate);

  if (outputGhostLevels)
  {
    vtkSmartPointer<vtkIdList> cellIds =
      ::ComputeOutputInterfaceCellIdsForStructuredData(blockStructure, output);
    ::FillReceivedGhostFieldDataForStructuredData(
      blockStructure.GhostCellData, output->GetCellData(), cellIds);

    // It is important to this function after filling received ghosts.
    // If not, we might write over hidden ghosts from other blocks
    ::FillDuplicateCellGhostArrayForStructuredData(block->GhostCellArray, cellIds);
  }
}

//----------------------------------------------------------------------------
void FillReceivedGhosts(
  ::ImageDataBlock* block, int myGid, int gid, vtkImageData* output, int outputGhostLevels)
{
  ::FillReceivedGhostsForStructuredData(block, myGid, gid, output, outputGhostLevels);
}

//----------------------------------------------------------------------------
void FillReceivedGhosts(::RectilinearGridBlock* block, int myGid, int gid,
  vtkRectilinearGrid* output, int outputGhostLevels)
{
  ::FillReceivedGhostsForStructuredData(block, myGid, gid, output, outputGhostLevels);
}

//----------------------------------------------------------------------------
void FillReceivedGhosts(::StructuredGridBlock* block, int myGid, int gid, vtkStructuredGrid* output,
  int outputGhostLevels)
{
  ::FillReceivedGhostsForStructuredData(block, myGid, gid, output, outputGhostLevels);
}

//----------------------------------------------------------------------------
std::map<vtkIdType, vtkIdType> ComputePointIdOffsetIntervals(
  const std::map<vtkIdType, vtkIdType>& redirectionMapForDuplicatePointIds)
{
  std::map<vtkIdType, vtkIdType> pointIdOffsetIntervals;
  if (redirectionMapForDuplicatePointIds.empty())
  {
    return pointIdOffsetIntervals;
  }

  // Here, we create a fast mechanism for skipping duplicate points.
  vtkIdType offset = -1;
  for (const auto& pair : redirectionMapForDuplicatePointIds)
  {
    pointIdOffsetIntervals.insert({ pair.first, ++offset });
  }
  pointIdOffsetIntervals.insert({ std::numeric_limits<vtkIdType>::max(), ++offset });

  return pointIdOffsetIntervals;
}

//----------------------------------------------------------------------------
void FillReceivedGhostPointsForUnstructuredData(::UnstructuredDataInformation& info,
  ::UnstructuredDataBlockStructure& blockStructure, vtkPointSet* output,
  vtkIdType numberOfAddedPoints)
{
  vtkPoints* outputPoints = output->GetPoints();

  // If there are no duplicate points on which we do not have ownership,
  // we can use a shortcut when copying point related data from the received buffers.
  if (blockStructure.RedirectionMapForDuplicatePointIds.empty())
  {
    if (blockStructure.GhostPoints)
    {
      outputPoints->InsertPoints(
        info.CurrentMaxPointId, numberOfAddedPoints, 0, blockStructure.GhostPoints);
    }
    ::FillReceivedGhostFieldData(blockStructure.GhostPointData, output->GetPointData(),
      info.CurrentMaxPointId, numberOfAddedPoints);
  }
  else
  {
    vtkNew<vtkIdList> identity;
    identity->SetNumberOfIds(numberOfAddedPoints);
    std::iota(identity->begin(), identity->end(), info.CurrentMaxPointId);

    vtkNew<vtkIdList> pointIds;
    pointIds->SetNumberOfIds(numberOfAddedPoints);
    vtkIdType offset = 0;
    auto it = blockStructure.RedirectionMapForDuplicatePointIds.cbegin();
    for (vtkIdType id = 0; id < numberOfAddedPoints; ++id)
    {
      while (
        it != blockStructure.RedirectionMapForDuplicatePointIds.cend() && id + offset == it->first)
      {
        ++it;
        ++offset;
      }
      pointIds->SetId(id, id + offset);
    }
    if (blockStructure.GhostPoints)
    {
      outputPoints->InsertPoints(identity, pointIds, blockStructure.GhostPoints);
    }

    ::FillReceivedGhostFieldData(
      blockStructure.GhostPointData, output->GetPointData(), pointIds, identity);
  }
}

//----------------------------------------------------------------------------
template <class DataSetT>
vtkIdType FillReceivedPointBuffersForUnstructuredData(
  typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType* block, int myGid, int gid,
  DataSetT* output)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType;
  using BlockInformationType = typename BlockType::InformationType;
  using BlockStructureType = typename BlockType::BlockStructureType;

  BlockInformationType& info = block->Information;
  BlockStructureType& blockStructure = block->BlockStructures.at(gid);

  vtkIdType numberOfAddedPoints = 0;
  if (blockStructure.GhostPoints)
  {
    numberOfAddedPoints = blockStructure.GhostPoints->GetNumberOfPoints() -
      blockStructure.RedirectionMapForDuplicatePointIds.size();
  }

  ::FillReceivedGhostPointsForUnstructuredData(info, blockStructure, output, numberOfAddedPoints);

  // We copy the received interface points that the neighboring block might have sent.
  // This way, we ensure that all partitions hold the same data and point position
  // as the partition who owns the point.
  // Note that we can receive the same point multiple times (from different blocks).
  // The owner of the point is the block of highest global id, so since the blocks are
  // sorted by global ids, so we need to skip points that were already set.
  // To find those points out, just look at the ghost array. If it was already set to
  // DUPLICATEPOINT, then the point is not virgin, and we need to skip it.
  vtkFieldData* interfacePointData = blockStructure.InterfacingPointData;
  if (interfacePointData)
  {
    vtkIdList* pointIdRemapping = blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget;
    vtkNew<vtkIdList> pointIdsWithNoDuplicate, remapping;
    ::RemoveDuplicatePointIds(block, pointIdRemapping, pointIdsWithNoDuplicate, remapping);

    ::FillReceivedGhostFieldData(
      interfacePointData, output->GetPointData(), remapping, pointIdsWithNoDuplicate);
  }

  // This needs to be called last so we don't write over it when filling field data.
  ::FillDuplicatePointGhostArrayForUnstructureData(block->GhostPointArray, myGid, gid,
    blockStructure, info.CurrentMaxPointId, numberOfAddedPoints);

  return numberOfAddedPoints;
}

//----------------------------------------------------------------------------
void FillReceivedGhosts(::UnstructuredGridBlock* block, int myGid, int gid,
  vtkUnstructuredGrid* output, int outputGhostLevels)
{
  vtkIdType numberOfAddedPoints =
    ::FillReceivedPointBuffersForUnstructuredData(block, myGid, gid, output);

  if (!outputGhostLevels)
  {
    // We're done here, the rest of the code deals with cells
    return;
  }

  ::UnstructuredGridInformation& info = block->Information;
  ::UnstructuredGridBlockStructure& blockStructure = block->BlockStructures.at(gid);

  vtkCellArray* outputCellArray = output->GetCells();
  vtkUnsignedCharArray* outputTypes = output->GetCellTypesArray();
  vtkCellArray* outputFaceLocations = output->GetPolyhedronFaceLocations();
  vtkCellArray* outputFaces = output->GetPolyhedronFaces();

  auto& buffer = blockStructure.ReceiveBuffer;
  vtkIdType numberOfAddedCells = buffer.Types->GetNumberOfValues();

  outputTypes->InsertTuples(info.CurrentMaxCellId, numberOfAddedCells, 0, buffer.Types);

  std::map<vtkIdType, vtkIdType> pointIdOffsetIntervals =
    ::ComputePointIdOffsetIntervals(blockStructure.RedirectionMapForDuplicatePointIds);

  ::InsertCells(buffer.CellArray, outputCellArray,
    blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
    blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
    info.CurrentMaxPointId, info.CurrentMaxCellId, info.CurrentConnectivitySize);

  if (vtkCellArray* faceLocations = buffer.FaceLocations)
  {
    using ArrayType32 = vtkCellArray::ArrayType32;
    using ArrayType64 = vtkCellArray::ArrayType64;

    if (!faceLocations->GetNumberOfCells())
    {
      return;
    }

    if (faceLocations->IsStorage64Bit())
    {
      ::PolyhedronsInserter<ArrayType64> inserter(faceLocations, buffer.Faces, outputFaceLocations,
        outputFaces, blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
        blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
        info.CurrentMaxPointId, info.CurrentMaxCellId, info.CurrentMaxFaceId,
        info.CurrentFacesSize);
      vtkSMPTools::For(0, faceLocations->GetNumberOfCells(), inserter);
    }
    else
    {
      ::PolyhedronsInserter<ArrayType32> inserter(faceLocations, buffer.Faces, outputFaceLocations,
        outputFaces, blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
        blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
        info.CurrentMaxPointId, info.CurrentMaxCellId, info.CurrentMaxFaceId,
        info.CurrentFacesSize);
      vtkSMPTools::For(0, faceLocations->GetNumberOfCells(), inserter);
    }
  }
  else
  {
    // Take care of polyhedron when buffer do not have them but they are needed in the end
    // Insert empty faces
    if (outputFaceLocations)
    {
      using ArrayType32 = vtkCellArray::ArrayType32;
      using ArrayType64 = vtkCellArray::ArrayType64;
      if (outputFaceLocations->IsStorage64Bit())
      {
        auto faceLocRange = vtkArrayDownCast<ArrayType64>(outputFaceLocations->GetOffsetsArray());
        for (vtkIdType pos = 0; pos < numberOfAddedCells; ++pos)
        {
          faceLocRange->SetValue(
            info.CurrentMaxCellId + pos + 1, faceLocRange->GetValue(info.CurrentMaxCellId + pos));
        }
      }
      else
      {
        auto faceLocRange = vtkArrayDownCast<ArrayType32>(outputFaceLocations->GetOffsetsArray());
        for (vtkIdType pos = 0; pos < numberOfAddedCells; ++pos)
        {
          faceLocRange->SetValue(
            info.CurrentMaxCellId + pos + 1, faceLocRange->GetValue(info.CurrentMaxCellId + pos));
        }
      }
    }
  }

  ::FillDuplicateCellGhostArrayForUnstructureData(
    block->GhostCellArray, info.CurrentMaxCellId, numberOfAddedCells);
  ::FillReceivedGhostFieldData(
    blockStructure.GhostCellData, output->GetCellData(), info.CurrentMaxCellId, numberOfAddedCells);

  info.CurrentMaxPointId += numberOfAddedPoints;
  info.CurrentMaxCellId += numberOfAddedCells;
  info.CurrentConnectivitySize += buffer.CellArray->GetConnectivityArray()->GetNumberOfTuples();
  info.CurrentFacesSize +=
    buffer.Faces ? buffer.Faces->GetConnectivityArray()->GetNumberOfTuples() : 0;
  info.CurrentMaxFaceId +=
    buffer.Faces ? buffer.Faces->GetOffsetsArray()->GetNumberOfTuples() - 1 : 0;
}

//----------------------------------------------------------------------------
void FillReceivedGhosts(
  ::PolyDataBlock* block, int myGid, int gid, vtkPolyData* output, int outputGhostLevels)
{
  vtkIdType numberOfAddedPoints =
    ::FillReceivedPointBuffersForUnstructuredData(block, myGid, gid, output);

  if (!outputGhostLevels)
  {
    // We're done here, the rest of the code deals with cells
    return;
  }

  ::PolyDataInformation& info = block->Information;
  ::PolyDataBlockStructure& blockStructure = block->BlockStructures.at(gid);

  vtkCellArray* outputPolys = output->GetPolys();
  vtkCellArray* outputStrips = output->GetStrips();
  vtkCellArray* outputLines = output->GetLines();

  auto& buffer = blockStructure.ReceiveBuffer;

  vtkIdType numberOfAddedPolyOffsets = buffer.Polys->GetOffsetsArray()->GetNumberOfTuples();
  vtkIdType numberOfAddedStripOffsets = buffer.Strips->GetOffsetsArray()->GetNumberOfTuples();
  vtkIdType numberOfAddedLineOffsets = buffer.Lines->GetOffsetsArray()->GetNumberOfTuples();

  vtkIdType numberOfAddedPolys = numberOfAddedPolyOffsets ? numberOfAddedPolyOffsets - 1 : 0;
  vtkIdType numberOfAddedStrips = numberOfAddedStripOffsets ? numberOfAddedStripOffsets - 1 : 0;
  vtkIdType numberOfAddedLines = numberOfAddedLineOffsets ? numberOfAddedLineOffsets - 1 : 0;

  vtkIdType numberOfAddedCells = numberOfAddedPolys + numberOfAddedStrips + numberOfAddedLines;

  std::map<vtkIdType, vtkIdType> pointIdOffsetIntervals =
    ::ComputePointIdOffsetIntervals(blockStructure.RedirectionMapForDuplicatePointIds);

  if (buffer.Polys->GetOffsetsArray()->GetNumberOfValues())
  {
    ::InsertCells(buffer.Polys, outputPolys,
      blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
      blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
      info.CurrentMaxPointId, info.CurrentMaxPolyId, info.CurrentPolyConnectivitySize);
  }

  if (buffer.Strips->GetOffsetsArray()->GetNumberOfValues())
  {
    ::InsertCells(buffer.Strips, outputStrips,
      blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
      blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
      info.CurrentMaxPointId, info.CurrentMaxStripId, info.CurrentStripConnectivitySize);
  }

  if (buffer.Lines->GetOffsetsArray()->GetNumberOfValues())
  {
    ::InsertCells(buffer.Lines, outputLines,
      blockStructure.RemappedMatchingReceivedPointIdsSortedLikeTarget,
      blockStructure.RedirectionMapForDuplicatePointIds, pointIdOffsetIntervals,
      info.CurrentMaxPointId, info.CurrentMaxLineId, info.CurrentLineConnectivitySize);
  }

  vtkIdType lineOffset = output->GetNumberOfVerts();
  vtkIdType polyOffset = output->GetNumberOfLines() + lineOffset;
  vtkIdType stripOffset = output->GetNumberOfPolys() + polyOffset;

  if (output->GetNumberOfLines())
  {
    ::FillDuplicateCellGhostArrayForUnstructureData(
      block->GhostCellArray, lineOffset + info.CurrentMaxLineId, numberOfAddedLines);
    ::FillReceivedGhostFieldData(blockStructure.GhostCellData, output->GetCellData(),
      lineOffset + info.CurrentMaxLineId, numberOfAddedLines);
  }
  if (output->GetNumberOfPolys())
  {
    ::FillDuplicateCellGhostArrayForUnstructureData(
      block->GhostCellArray, polyOffset + info.CurrentMaxPolyId, numberOfAddedPolys);
    ::FillReceivedGhostFieldData(blockStructure.GhostCellData, output->GetCellData(),
      polyOffset + info.CurrentMaxPolyId, numberOfAddedPolys, numberOfAddedLines);
  }
  if (output->GetNumberOfStrips())
  {
    ::FillDuplicateCellGhostArrayForUnstructureData(
      block->GhostCellArray, stripOffset + info.CurrentMaxStripId, numberOfAddedStrips);
    ::FillReceivedGhostFieldData(blockStructure.GhostCellData, output->GetCellData(),
      stripOffset + info.CurrentMaxStripId, numberOfAddedStrips,
      numberOfAddedLines + numberOfAddedPolys);
  }

  info.CurrentMaxPointId += numberOfAddedPoints;
  info.CurrentMaxCellId += numberOfAddedCells;

  info.CurrentMaxPolyId += numberOfAddedPolys;
  info.CurrentMaxStripId += numberOfAddedStrips;
  info.CurrentMaxLineId += numberOfAddedLines;

  info.CurrentPolyConnectivitySize += buffer.Polys->GetConnectivityArray()->GetNumberOfTuples();
  info.CurrentStripConnectivitySize += buffer.Strips->GetConnectivityArray()->GetNumberOfTuples();
  info.CurrentLineConnectivitySize += buffer.Lines->GetConnectivityArray()->GetNumberOfTuples();
}

//----------------------------------------------------------------------------
template <class DataSetT>
void FillReceivedGhosts(
  const diy::Master& master, std::vector<DataSetT*>& outputs, int outputGhostLevels)
{
  using BlockType = typename ::DataSetTypeToBlockTypeConverter<DataSetT>::BlockType;

  for (int localId = 0; localId < static_cast<int>(outputs.size()); ++localId)
  {
    DataSetT* output = outputs[localId];
    BlockType* block = master.block<BlockType>(localId);
    int gid = master.gid(localId);

    for (auto& pair : block->BlockStructures)
    {
      ::FillReceivedGhosts(block, gid, pair.first, output, outputGhostLevels);
    }
  }
}

//----------------------------------------------------------------------------
void CopyOuterLayerGridPoints(
  vtkStructuredGrid* input, vtkSmartPointer<vtkPoints>& outputPoints, ::ExtentType extent, int i)
{
  int j = (i + 2) % 6;
  j -= j % 2;
  int k = (i + 4) % 6;
  k -= k % 2;

  vtkPoints* inputPoints = input->GetPoints();
  int* inputExtent = input->GetExtent();

  outputPoints = vtkSmartPointer<vtkPoints>::New();
  outputPoints->SetDataType(inputPoints->GetDataType());
  outputPoints->SetNumberOfPoints(
    (extent[j + 1] - extent[j] + 1) * (extent[k + 1] - extent[k] + 1));

  // We collapse one dimension
  extent[i + (i % 2 ? -1 : 1)] = extent[i];

  int ijk[3];
  ijk[i / 2] = extent[i];
  for (int y = extent[k]; y <= extent[k + 1]; ++y)
  {
    ijk[k / 2] = y;
    for (int x = extent[j]; x <= extent[j + 1]; ++x)
    {
      ijk[j / 2] = x;
      outputPoints->SetPoint(vtkStructuredData::ComputePointIdForExtent(extent.data(), ijk),
        inputPoints->GetPoint(vtkStructuredData::ComputePointIdForExtent(inputExtent, ijk)));
    }
  }
}

//============================================================================
struct ReinitializeBitsWorker
{
  ReinitializeBitsWorker(vtkUnsignedCharArray* ghosts, unsigned char mask)
    : Ghosts(ghosts)
    , Mask(mask)
  {
  }

  void operator()(vtkIdType startId, vtkIdType endId)
  {
    auto ghosts = vtk::DataArrayValueRange<1>(this->Ghosts);
    for (vtkIdType id = startId; id < endId; ++id)
    {
      ghosts[id] &= this->Mask;
    }
  }

  vtkUnsignedCharArray* Ghosts;
  unsigned char Mask;
};
} // anonymous namespace

VTK_ABI_NAMESPACE_BEGIN

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ReinitializeSelectedBits(
  vtkUnsignedCharArray* ghosts, unsigned char bits)
{
  ::ReinitializeBitsWorker worker(ghosts, ~bits);
  vtkSMPTools::For(0, ghosts->GetNumberOfValues(), worker);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InflateBoundingBoxIfNecessary(
  vtkDataSet* vtkNotUsed(input), vtkBoundingBox& vtkNotUsed(bb))
{
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InflateBoundingBoxIfNecessary(
  vtkPointSet* vtkNotUsed(input), vtkBoundingBox& bb)
{
  // We inflate the bounding box by quite a lot (0.1% of the bounding box's largest width).
  // It is not that problematic. It might include a few extra points to be shared across partitions.
  // This loose inflation allows data sets that have very imprecise point positions and global ids
  // attached to them to succeed at generating ghosts.
  // This addresses paraview/paraview#21228
  bb.Inflate(1e-3 * bb.GetMaxLength());
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::GridBlockStructure::GridBlockStructure(const int* extent, int dim)
  : Extent{ extent[0], extent[1], extent[2], extent[3], extent[4], extent[5] }
  , DataDimension(dim)
{
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::ImageDataBlockStructure::ImageDataBlockStructure(const int extent[6], int dim,
  const double origin[3], const double spacing[3], const double orientationQuaternion[4])
  : GridBlockStructure(extent, dim)
  , Origin{ origin[0], origin[1], origin[2] }
  , Spacing{ spacing[0], spacing[1], spacing[2] }
  , OrientationQuaternion{ orientationQuaternion[0], orientationQuaternion[1],
    orientationQuaternion[2], orientationQuaternion[3] }
{
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::ImageDataBlockStructure::ImageDataBlockStructure(const int extent[6], int dim,
  const double origin[3], const double spacing[3], vtkMatrix3x3* directionMatrix)
  : GridBlockStructure(extent, dim)
  , Origin{ origin[0], origin[1], origin[2] }
  , Spacing{ spacing[0], spacing[1], spacing[2] }
{
  vtkMath::Matrix3x3ToQuaternion(directionMatrix->GetData(), OrientationQuaternion.GetData());
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::ImageDataBlockStructure::ImageDataBlockStructure(
  vtkImageData* image, const ImageDataInformation& information)
  : ImageDataBlockStructure(information.Extent.data(), image->GetDataDimension(),
      image->GetOrigin(), image->GetSpacing(), image->GetDirectionMatrix())
{
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::RectilinearGridBlockStructure::RectilinearGridBlockStructure(
  const int extent[6], int dim, vtkDataArray* xCoordinates, vtkDataArray* yCoordinates,
  vtkDataArray* zCoordinates)
  : GridBlockStructure(extent, dim)
  , XCoordinates(vtkSmartPointer<vtkDataArray>::Take(xCoordinates))
  , YCoordinates(vtkSmartPointer<vtkDataArray>::Take(yCoordinates))
  , ZCoordinates(vtkSmartPointer<vtkDataArray>::Take(zCoordinates))
{
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::RectilinearGridBlockStructure::RectilinearGridBlockStructure(
  vtkRectilinearGrid* grid, const RectilinearGridInformation& information)
  : GridBlockStructure(information.Extent.data(), grid->GetDataDimension())
  , XCoordinates(information.XCoordinates)
  , YCoordinates(information.YCoordinates)
  , ZCoordinates(information.ZCoordinates)
{
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::StructuredGridBlockStructure::StructuredGridBlockStructure(
  const int extent[6], int dim, vtkDataArray* points[6])
  : GridBlockStructure(extent, dim)
{
  for (int i = 0; i < 6; ++i)
  {
    this->OuterPointLayers[i] = vtkSmartPointer<vtkPoints>::New();
    this->OuterPointLayers[i]->SetData(points[i]);
    points[i]->FastDelete();
  }
}

//----------------------------------------------------------------------------
vtkDIYGhostUtilities::StructuredGridBlockStructure::StructuredGridBlockStructure(
  vtkStructuredGrid* grid, const StructuredGridInformation& info)
  : GridBlockStructure(info.Extent.data(), grid->GetDataDimension())
  , OuterPointLayers{ info.OuterPointLayers[0].Points, info.OuterPointLayers[1].Points,
    info.OuterPointLayers[2].Points, info.OuterPointLayers[3].Points,
    info.OuterPointLayers[4].Points, info.OuterPointLayers[5].Points }
{
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneInputData(std::vector<vtkDataSet*>& inputs,
  std::vector<vtkDataSet*>& outputs, bool syncCell, bool syncPoint)
{
  auto outputIter = outputs.begin();
  for (const auto& input : inputs)
  {
    vtkDataSet* output = *outputIter;
    output->CopyStructure(input);
    outputIter++;

    if (syncCell)
    {
      CloneInputData(input, output, vtkDataSet::AttributeTypes::CELL);
    }
    if (syncPoint)
    {
      CloneInputData(input, output, vtkDataSet::AttributeTypes::POINT);
    }
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneInputData(vtkDataSet* input, vtkDataSet* output, int fieldType)
{
  vtkDataSetAttributes* inFieldData = input->GetAttributes(fieldType);
  vtkDataSetAttributes* outFieldData = output->GetAttributes(fieldType);

  vtkSmartPointer<vtkDataArray> ghostArray = inFieldData->GetGhostArray();
  vtkSmartPointer<vtkDataArray> globalIdsArray = inFieldData->GetGlobalIds();
  vtkSmartPointer<vtkDataArray> processIdsArray = inFieldData->GetProcessIds();

  inFieldData->RemoveArray(ghostArray->GetName());
  inFieldData->RemoveArray(globalIdsArray->GetName());
  inFieldData->RemoveArray(processIdsArray->GetName());

  outFieldData->CopyAllOn();
  outFieldData->CopyAllocate(inFieldData, input->GetNumberOfElements(fieldType));
  outFieldData->SetNumberOfTuples(input->GetNumberOfElements(fieldType));

  // Copy input data (including bad ghost data)
  for (int arrayId = 0; arrayId < inFieldData->GetNumberOfArrays(); ++arrayId)
  {
    vtkAbstractArray* sourceArray = inFieldData->GetAbstractArray(arrayId);
    sourceArray->GetTuples(
      0, sourceArray->GetNumberOfTuples() - 1, outFieldData->GetAbstractArray(arrayId));
  }

  inFieldData->AddArray(ghostArray);
  inFieldData->SetGlobalIds(globalIdsArray);
  inFieldData->SetProcessIds(processIdsArray);

  outFieldData->AddArray(ghostArray);
  outFieldData->SetGlobalIds(globalIdsArray);
  outFieldData->SetProcessIds(processIdsArray);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(
  diy::Master& master, std::vector<vtkDataSet*>& inputs, bool syncCell, bool syncPoint)
{
  if (syncCell)
  {
    InitializeBlocks(master, inputs, vtkDataSet::AttributeTypes::CELL,
      vtkDataSetAttributes::CellGhostTypes::DUPLICATECELL);
  }
  if (syncPoint)
  {
    InitializeBlocks(master, inputs, vtkDataSet::AttributeTypes::POINT,
      vtkDataSetAttributes::PointGhostTypes::DUPLICATEPOINT);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(
  diy::Master& master, std::vector<vtkDataSet*>& inputs, int fieldType, unsigned char ghostFlag)
{
  for (int blockLid = 0; blockLid < static_cast<int>(inputs.size()); ++blockLid)
  {
    vtkDataSet* input = inputs[blockLid];
    DataSetBlock* block = master.block<DataSetBlock>(blockLid);
    block->GlobalToLocalIds[fieldType].clear();
    block->NeededGidsForBlocks[fieldType].clear();
    block->GhostGidsFromBlocks[fieldType].clear();

    vtkDataSetAttributes* fieldData = input->GetAttributes(fieldType);
    const auto ghostRange = vtk::DataArrayValueRange<1>(fieldData->GetGhostArray());
    const auto gidRange =
      vtk::DataArrayValueRange<1>(vtkArrayDownCast<vtkIdTypeArray>(fieldData->GetGlobalIds()));
    const auto pidRange =
      vtk::DataArrayValueRange<1>(vtkArrayDownCast<vtkIdTypeArray>(fieldData->GetProcessIds()));

    const vtkIdType nbElements = input->GetNumberOfElements(fieldType);
    for (vtkIdType localId = 0; localId < nbElements; ++localId)
    {
      const vtkIdType& gid = gidRange[localId];
      block->GlobalToLocalIds[fieldType][gid] = localId;

      if (ghostRange[localId] & ghostFlag)
      {
        const vtkIdType& pid = pidRange[localId];
        block->NeededGidsForBlocks[fieldType][pid]->InsertNextValue(gid);
      }
    }
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeNeededIds(
  diy::Master& master, const vtkDIYExplicitAssigner& assigner, bool syncCell, bool syncPoint)
{
  if (syncCell)
  {
    ExchangeNeededIds(master, assigner, vtkDataSet::AttributeTypes::CELL);
  }
  if (syncPoint)
  {
    ExchangeNeededIds(master, assigner, vtkDataSet::AttributeTypes::POINT);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeNeededIds(
  diy::Master& master, const vtkDIYExplicitAssigner& assigner, int fieldType)
{
  diy::all_to_all(master, assigner,
    [fieldType](DataSetBlock* block, const diy::ReduceProxy& srp)
    {
      if (srp.round() == 0)
      {
        for (int i = 0; i < srp.out_link().size(); ++i)
        {
          const diy::BlockID& blockId = srp.out_link().target(i);
          if (block->NeededGidsForBlocks[fieldType].find(blockId.proc) !=
            block->NeededGidsForBlocks[fieldType].end())
          {
            srp.enqueue<vtkDataArray*>(
              blockId, block->NeededGidsForBlocks[fieldType][blockId.proc]);
          }
        }
      }
      else
      {
        vtkDataArray* tmpGids = nullptr;
        for (int i = 0; i < srp.in_link().size(); ++i)
        {
          const diy::BlockID& blockId = srp.in_link().target(i);
          if (srp.incoming(blockId.gid).empty())
          {
            continue;
          }

          srp.dequeue<vtkDataArray*>(blockId, tmpGids);
          vtkSmartPointer<vtkIdTypeArray> receivedGids =
            vtkSmartPointer<vtkIdTypeArray>::Take(vtkArrayDownCast<vtkIdTypeArray>(tmpGids));

          // Make sure to keep only gids of this partition
          const auto gidsRange = vtk::DataArrayValueRange<1>(receivedGids);
          for (const auto& receivedGid : gidsRange)
          {
            if (block->GlobalToLocalIds[fieldType].find(receivedGid) !=
              block->GlobalToLocalIds[fieldType].cend())
            {
              block->GhostGidsFromBlocks[fieldType][blockId.gid]->InsertNextValue(receivedGid);
            }
          }
        }
      }
    });
}

//----------------------------------------------------------------------------
LinkMap vtkDIYGhostUtilities::ComputeLinkMapUsingNeededIds(
  const diy::Master& master, bool syncCell, bool syncPoint)
{
  LinkMap linkMap(master.size());

  for (int blockLid = 0; blockLid < static_cast<int>(master.size()); ++blockLid)
  {
    Links& links = linkMap[blockLid];
    DataSetBlock* block = master.block<DataSetBlock>(blockLid);

    if (syncCell)
    {
      ComputeLinksUsingNeededIds(links, block, vtkDataSet::AttributeTypes::CELL);
    }
    if (syncPoint)
    {
      ComputeLinksUsingNeededIds(links, block, vtkDataSet::AttributeTypes::POINT);
    }
  }
  return linkMap;
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ComputeLinksUsingNeededIds(
  Links& links, vtkDIYGhostUtilities::DataSetBlock* block, int fieldType)
{
  for (const auto& item : block->GhostGidsFromBlocks[fieldType])
  {
    if (links.find(item.first) == links.end())
    {
      links.insert(item.first);
    }
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeFieldData(diy::Master& master, std::vector<vtkDataSet*>& inputs,
  std::vector<vtkDataSet*>& outputs, bool syncCell, bool syncPoint)
{
  if (syncCell)
  {
    ExchangeFieldData(master, inputs, outputs, vtkDataSet::AttributeTypes::CELL);
  }
  if (syncPoint)
  {
    ExchangeFieldData(master, inputs, outputs, vtkDataSet::AttributeTypes::POINT);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeFieldData(diy::Master& master, std::vector<vtkDataSet*>& inputs,
  std::vector<vtkDataSet*>& outputs, int fieldType)
{
  master.foreach (
    [&master, &inputs, fieldType](DataSetBlock* block, const diy::Master::ProxyWithLink& cp)
    {
      int myBlockGid = cp.gid();
      int myBlockLid = master.lid(myBlockGid);
      auto& input = inputs[myBlockLid];

      diy::Link* link = cp.link();
      for (int id = 0; id < link->size(); ++id)
      {
        const diy::BlockID& blockId = link->target(id);
        if (block->GhostGidsFromBlocks[fieldType].find(blockId.gid) ==
          block->GhostGidsFromBlocks[fieldType].end())
        {
          continue;
        }

        vtkNew<vtkIdList> lids;
        const auto gidsRange =
          vtk::DataArrayValueRange<1>(block->GhostGidsFromBlocks[fieldType][blockId.gid]);
        for (const auto& gid : gidsRange)
        {
          lids->InsertNextId(block->GlobalToLocalIds[fieldType][gid]);
        }

        vtkNew<vtkFieldData> fieldData;
        vtkDataSetAttributes* inputFieldData = input->GetAttributes(fieldType);
        fieldData->CopyStructure(inputFieldData);
        fieldData->SetNumberOfTuples(lids->GetNumberOfIds());

        // Do not send ids info
        if (inputFieldData->GetGlobalIds())
        {
          fieldData->RemoveArray(inputFieldData->GetGlobalIds()->GetName());
        }
        if (inputFieldData->GetProcessIds())
        {
          fieldData->RemoveArray(inputFieldData->GetProcessIds()->GetName());
        }
        if (inputFieldData->GetPedigreeIds())
        {
          fieldData->RemoveArray(inputFieldData->GetPedigreeIds()->GetName());
        }
        // Do not send ghost info
        if (inputFieldData->GetGhostArray())
        {
          fieldData->RemoveArray(inputFieldData->GetGhostArray()->GetName());
        }

        // Copy needed tuples
        for (int arrayId = 0; arrayId < fieldData->GetNumberOfArrays(); ++arrayId)
        {
          vtkAbstractArray* array = fieldData->GetAbstractArray(arrayId);
          inputFieldData->GetAbstractArray(array->GetName())->GetTuples(lids, array);
        }

        // Must send non-empty field data
        if (fieldData->GetNumberOfArrays() > 0)
        {
          cp.enqueue<vtkFieldData*>(blockId, fieldData);
        }
      }
    });

  master.exchange();

  master.foreach (
    [&master, &outputs, fieldType](DataSetBlock* block, const diy::Master::ProxyWithLink& cp)
    {
      int myBlockGid = cp.gid();
      int myBlockLid = master.lid(myBlockGid);
      auto& output = outputs[myBlockLid];

      vtkFieldData* tmpFieldData = nullptr;

      diy::Link* link = cp.link();
      for (int id = 0; id < link->size(); ++id)
      {
        const diy::BlockID& blockId = link->target(id);
        // we need this extra check because incoming is not empty when using only one block
        // also because attributes may have no data to transfer (no transferable array)
        if (cp.incoming(blockId.gid).empty())
        {
          continue;
        }

        cp.dequeue<vtkFieldData*>(blockId.gid, tmpFieldData);
        vtkSmartPointer<vtkFieldData> fieldData = vtkSmartPointer<vtkFieldData>::Take(tmpFieldData);
        vtkDataSetAttributes* outputFieldData = output->GetAttributes(fieldType);

        const auto gidsRange =
          vtk::DataArrayValueRange<1>(block->NeededGidsForBlocks[fieldType][blockId.proc]);
        const vtkIdType nbElements =
          block->NeededGidsForBlocks[fieldType][blockId.proc]->GetNumberOfTuples();

        for (int arrayId = 0; arrayId < fieldData->GetNumberOfArrays(); ++arrayId)
        {
          vtkAbstractArray* inputArray = fieldData->GetAbstractArray(arrayId);
          vtkAbstractArray* outputArray = outputFieldData->GetAbstractArray(inputArray->GetName());

          for (vtkIdType i = 0; i < nbElements; ++i)
          {
            vtkIdType gid = gidsRange[i];
            vtkIdType lid = block->GlobalToLocalIds[fieldType][gid];
            outputArray->SetTuple(lid, i, inputArray);
          }
        }
      }
    });
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::SynchronizeGhostData(std::vector<vtkDataSet*>& inputs,
  std::vector<vtkDataSet*>& outputs, vtkMultiProcessController* controller, bool syncCell,
  bool syncPoint)
{
  const int size = static_cast<int>(inputs.size());
  if (size != static_cast<int>(outputs.size()))
  {
    return 0;
  }

  std::string logMessage = size
    ? std::string("Synchronizing ghosts for ") + std::string(outputs[0]->GetClassName())
    : std::string("No ghosts to synchronize for empty rank");
  vtkLogStartScope(TRACE, logMessage.c_str());

  vtkDIYGhostUtilities::CloneInputData(inputs, outputs, syncCell, syncPoint);

  vtkLogStartScope(TRACE, "Instantiating diy communicator");
  diy::mpi::communicator comm = vtkDIYUtilities::GetCommunicator(controller);
  vtkLogEndScope("Instantiating diy communicator");

  vtkLogStartScope(TRACE, "Instantiating master");
  diy::Master master(
    comm, 1, -1, []() { return static_cast<void*>(new DataSetBlock()); },
    [](void* b) -> void { delete static_cast<DataSetBlock*>(b); });
  vtkLogEndScope("Instantiating master");

  vtkLogStartScope(TRACE, "Instantiating assigner");
  vtkDIYExplicitAssigner assigner(comm, size);
  vtkLogEndScope("Instantiating assigner");

  if (!size)
  {
    // In such instance, we can just terminate. We are empty an finished communicating with other
    // ranks.
    vtkLogEndScope(logMessage.c_str());
    return 1;
  }

  vtkLogStartScope(TRACE, "Decomposing master");
  diy::RegularDecomposer<diy::DiscreteBounds> decomposer(
    /*dim*/ 1, diy::interval(0, assigner.nblocks() - 1), assigner.nblocks());
  decomposer.decompose(comm.rank(), assigner, master);
  vtkLogEndScope("Decomposing master");

  // At this step, we gather data from the inputs and store it inside the local blocks
  // so we don't have to carry extra parameters later.
  vtkLogStartScope(TRACE, "Setup block self information.");
  vtkDIYGhostUtilities::InitializeBlocks(master, inputs, syncCell, syncPoint);
  vtkLogEndScope("Setup block self information.");

  vtkLogStartScope(TRACE, "Exchanging needed ids");
  vtkDIYGhostUtilities::ExchangeNeededIds(master, assigner, syncCell, syncPoint);
  vtkLogEndScope("Exchanging needed ids");

  vtkLogStartScope(TRACE, "Computing link map using needed ids.");
  LinkMap linkMap = vtkDIYGhostUtilities::ComputeLinkMapUsingNeededIds(master, syncCell, syncPoint);
  vtkLogEndScope("Computing link map using needed ids.");

  vtkLogStartScope(TRACE, "Relinking blocks using link map");
  vtkDIYUtilities::Link(master, assigner, linkMap);
  vtkLogEndScope("Relinking blocks using link map");

  vtkLogStartScope(TRACE, "Exchanging field data");
  vtkDIYGhostUtilities::ExchangeFieldData(master, inputs, outputs, syncCell, syncPoint);
  vtkLogEndScope("Exchanging field data");

  return 1;
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(diy::Master& master, std::vector<vtkImageData*>& inputs)
{
  ::InitializeBlocksForStructuredData(master, inputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(
  diy::Master& master, std::vector<vtkRectilinearGrid*>& inputs)
{
  ::InitializeBlocksForStructuredData(master, inputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(
  diy::Master& master, std::vector<vtkStructuredGrid*>& inputs)
{
  using BlockType = StructuredGridBlock;

  ::InitializeBlocksForStructuredData(master, inputs);

  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    vtkStructuredGrid* input = inputs[localId];
    BlockType* block = master.block<BlockType>(localId);
    typename BlockType::InformationType& information = block->Information;
    information.InputPoints = input->GetPoints();
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(
  diy::Master& master, std::vector<vtkUnstructuredGrid*>& inputs)
{
  ::InitializeBlocksForUnstructuredData(master, inputs);

  using BlockType = UnstructuredGridBlock;
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    vtkUnstructuredGrid* input = inputs[localId];
    BlockType* block = master.block<BlockType>(localId);
    typename BlockType::InformationType& information = block->Information;

    vtkCellArray* faces = input->GetPolyhedronFaces();
    information.Faces = faces && faces->GetNumberOfCells() ? faces : nullptr;

    vtkCellArray* faceLocations = input->GetPolyhedronFaceLocations();
    information.FaceLocations =
      faceLocations && faceLocations->GetNumberOfCells() ? faceLocations : nullptr;
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::InitializeBlocks(diy::Master& master, std::vector<vtkPolyData*>& inputs)
{
  ::InitializeBlocksForUnstructuredData(master, inputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeBlockStructures(
  diy::Master& master, std::vector<vtkImageData*>& inputs)
{
  using BlockType = ImageDataBlock;

  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    BlockType* block = master.block<BlockType>(localId);
    block->Information.Extent = ::PeelOffGhostLayers(inputs[localId]);
  }

  master.foreach (
    [&master, &inputs](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      int myBlockId = cp.gid();
      int localId = master.lid(myBlockId);
      auto& input = inputs[localId];

      const ExtentType& extent = block->Information.Extent;
      double* origin = input->GetOrigin();
      double* spacing = input->GetSpacing();
      int dimension = input->GetDataDimension();
      QuaternionType q;
      vtkMath::Matrix3x3ToQuaternion(input->GetDirectionMatrix()->GetData(), q.GetData());
      double* qBuffer = q.GetData();
      for (int id = 0; id < cp.link()->size(); ++id)
      {
        const diy::BlockID& blockId = cp.link()->target(id);
        cp.enqueue(blockId, &dimension, 1);
        cp.enqueue(blockId, origin, 3);
        cp.enqueue(blockId, spacing, 3);
        cp.enqueue(blockId, qBuffer, 4);
        cp.enqueue(blockId, extent.data(), 6);
      }
    });

  master.exchange();

  master.foreach (
    [](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      std::vector<int> incoming;
      cp.incoming(incoming);

      int dimension;
      int extent[6];
      double origin[3];
      double spacing[3];
      double q[4];

      for (const int& gid : incoming)
      {
        // we need this extra check because incoming is not empty when using only one block
        if (!cp.incoming(gid).empty())
        {
          cp.dequeue(gid, &dimension, 1);
          cp.dequeue(gid, origin, 3);
          cp.dequeue(gid, spacing, 3);
          cp.dequeue(gid, q, 4);
          cp.dequeue(gid, extent, 6);

          block->BlockStructures.emplace(
            gid, ImageDataBlockStructure(extent, dimension, origin, spacing, q));
        }
      }
    });
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeBlockStructures(
  diy::Master& master, std::vector<vtkRectilinearGrid*>& inputs)
{
  using BlockType = RectilinearGridBlock;
  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    vtkRectilinearGrid* input = inputs[localId];
    int* inputExtent = input->GetExtent();
    if (!::IsExtentValid(inputExtent))
    {
      continue;
    }
    BlockType* block = master.block<BlockType>(localId);
    auto& info = block->Information;
    ExtentType& extent = info.Extent;
    extent = ::PeelOffGhostLayers(input);

    vtkDataArray* inputXCoordinates = input->GetXCoordinates();
    vtkDataArray* inputYCoordinates = input->GetYCoordinates();
    vtkDataArray* inputZCoordinates = input->GetZCoordinates();

    info.XCoordinates.TakeReference(inputXCoordinates->NewInstance());
    info.YCoordinates.TakeReference(inputYCoordinates->NewInstance());
    info.ZCoordinates.TakeReference(inputZCoordinates->NewInstance());

    info.XCoordinates->InsertTuples(
      0, extent[1] - extent[0] + 1, extent[0] - inputExtent[0], inputXCoordinates);

    info.YCoordinates->InsertTuples(
      0, extent[3] - extent[2] + 1, extent[2] - inputExtent[2], inputYCoordinates);
    info.ZCoordinates->InsertTuples(
      0, extent[5] - extent[4] + 1, extent[4] - inputExtent[4], inputZCoordinates);
  }

  master.foreach (
    [&master, &inputs](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      int myBlockId = cp.gid();
      int localId = master.lid(myBlockId);
      auto& input = inputs[localId];

      auto& info = block->Information;
      int dimension = input->GetDataDimension();
      const ExtentType& extent = info.Extent;
      vtkDataArray* xCoordinates = info.XCoordinates;
      vtkDataArray* yCoordinates = info.YCoordinates;
      vtkDataArray* zCoordinates = info.ZCoordinates;

      for (int id = 0; id < cp.link()->size(); ++id)
      {
        const diy::BlockID& blockId = cp.link()->target(id);
        cp.enqueue(blockId, &dimension, 1);
        cp.enqueue(blockId, extent.data(), 6);
        cp.enqueue<vtkDataArray*>(blockId, xCoordinates);
        cp.enqueue<vtkDataArray*>(blockId, yCoordinates);
        cp.enqueue<vtkDataArray*>(blockId, zCoordinates);
      }
    });

  master.exchange();

  master.foreach (
    [](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      std::vector<int> incoming;
      cp.incoming(incoming);

      int dimension;
      int extent[6];

      for (const int& gid : incoming)
      {
        // we need this extra check because incoming is not empty when using only one block
        if (!cp.incoming(gid).empty())
        {
          vtkDataArray* xCoordinates = nullptr;
          vtkDataArray* yCoordinates = nullptr;
          vtkDataArray* zCoordinates = nullptr;

          cp.dequeue(gid, &dimension, 1);
          cp.dequeue(gid, extent, 6);
          cp.dequeue(gid, xCoordinates);
          cp.dequeue(gid, yCoordinates);
          cp.dequeue(gid, zCoordinates);

          block->BlockStructures.emplace(gid,
            RectilinearGridBlockStructure(
              extent, dimension, xCoordinates, yCoordinates, zCoordinates));
        }
      }
    });
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeBlockStructures(
  diy::Master& master, std::vector<vtkStructuredGrid*>& inputs)
{
  using BlockType = StructuredGridBlock;

  // In addition to the extent, we need to share the points lying on the 6 external faces of each
  // structured grid. These points will be used to determine if structured grids are connected or
  // not.

  for (int localId = 0; localId < static_cast<int>(inputs.size()); ++localId)
  {
    vtkStructuredGrid* input = inputs[localId];
    int* inputExtent = input->GetExtent();
    if (!::IsExtentValid(inputExtent))
    {
      continue;
    }
    BlockType* block = master.block<BlockType>(localId);
    StructuredGridInformation& info = block->Information;
    ExtentType& extent = info.Extent;
    extent = ::PeelOffGhostLayers(input);

    for (int i = 0; i < 6; ++i)
    {
      ::CopyOuterLayerGridPoints(input, info.OuterPointLayers[i].Points, extent, i);
    }
  }

  master.foreach (
    [&master, &inputs](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      int myBlockId = cp.gid();
      int localId = master.lid(myBlockId);
      auto& input = inputs[localId];

      auto& info = block->Information;
      int dimension = input->GetDataDimension();
      const ExtentType& extent = info.Extent;

      for (int id = 0; id < cp.link()->size(); ++id)
      {
        const diy::BlockID& blockId = cp.link()->target(id);
        cp.enqueue(blockId, &dimension, 1);
        cp.enqueue(blockId, extent.data(), 6);
        for (int extentId = 0; extentId < 6; ++extentId)
        {
          cp.enqueue<vtkDataArray*>(blockId, info.OuterPointLayers[extentId].Points->GetData());
        }
      }
    });

  master.exchange();

  master.foreach (
    [](BlockType* block, const diy::Master::ProxyWithLink& cp)
    {
      std::vector<int> incoming;
      cp.incoming(incoming);

      int dimension;
      int extent[6];

      for (const int& gid : incoming)
      {
        // we need this extra check because incoming is not empty when using only one block
        if (!cp.incoming(gid).empty())
        {
          vtkDataArray* points[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

          cp.dequeue(gid, &dimension, 1);
          cp.dequeue(gid, extent, 6);
          for (int extentId = 0; extentId < 6; ++extentId)
          {
            vtkDataArray* tmp = points[extentId];
            cp.dequeue<vtkDataArray*>(gid, tmp);
            points[extentId] = tmp;
          }

          block->BlockStructures.emplace(
            gid, StructuredGridBlockStructure(extent, dimension, points));
        }
      }
    });
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneGeometricStructures(
  std::vector<vtkImageData*>& inputs, std::vector<vtkImageData*>& outputs)
{
  ::CloneGeometricStructuresForStructuredData(inputs, outputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneGeometricStructures(
  std::vector<vtkRectilinearGrid*>& inputs, std::vector<vtkRectilinearGrid*>& outputs)
{
  ::CloneGeometricStructuresForStructuredData(inputs, outputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneGeometricStructures(
  std::vector<vtkStructuredGrid*>& inputs, std::vector<vtkStructuredGrid*>& outputs)
{
  ::CloneGeometricStructuresForStructuredData(inputs, outputs);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneGeometricStructures(
  std::vector<vtkUnstructuredGrid*>& vtkNotUsed(inputs),
  std::vector<vtkUnstructuredGrid*>& vtkNotUsed(outputs))
{
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::CloneGeometricStructures(
  std::vector<vtkPolyData*>& vtkNotUsed(inputs), std::vector<vtkPolyData*>& vtkNotUsed(outputs))
{
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeBlockStructures(
  diy::Master& master, std::vector<vtkUnstructuredGrid*>& vtkNotUsed(inputs))
{
  ::ExchangeBlockStructuresForUnstructuredData<vtkUnstructuredGrid>(master);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::ExchangeBlockStructures(
  diy::Master& master, std::vector<vtkPolyData*>& vtkNotUsed(inputs))
{
  ::ExchangeBlockStructuresForUnstructuredData<vtkPolyData>(master);
}

//----------------------------------------------------------------------------
::LinkMap vtkDIYGhostUtilities::ComputeLinkMap(
  const diy::Master& master, std::vector<vtkImageData*>& inputs, int outputGhostLevels)
{
  return ::ComputeLinkMapForStructuredData(master, inputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
::LinkMap vtkDIYGhostUtilities::ComputeLinkMap(
  const diy::Master& master, std::vector<vtkRectilinearGrid*>& inputs, int outputGhostLevels)
{
  return ::ComputeLinkMapForStructuredData(master, inputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
::LinkMap vtkDIYGhostUtilities::ComputeLinkMap(
  const diy::Master& master, std::vector<vtkStructuredGrid*>& inputs, int outputGhostLevels)
{
  return ::ComputeLinkMapForStructuredData(master, inputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
::LinkMap vtkDIYGhostUtilities::ComputeLinkMap(
  const diy::Master& master, std::vector<vtkUnstructuredGrid*>& inputs, int outputGhostLevels)
{
  return ::ComputeLinkMapForUnstructuredData(master, inputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
::LinkMap vtkDIYGhostUtilities::ComputeLinkMap(
  const diy::Master& master, std::vector<vtkPolyData*>& inputs, int outputGhostLevels)
{
  return ::ComputeLinkMapForUnstructuredData(master, inputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::EnqueueGhosts(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, vtkImageData* input, ImageDataBlock* block)
{
  ::EnqueueGhostsForStructuredData(cp, blockId, input, block);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::EnqueueGhosts(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, vtkRectilinearGrid* input, RectilinearGridBlock* block)
{
  ::EnqueueGhostsForStructuredData(cp, blockId, input, block);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::EnqueueGhosts(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, vtkStructuredGrid* input, StructuredGridBlock* block)
{
  ::EnqueueGhostsForStructuredData(cp, blockId, input, block);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::EnqueueGhosts(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, vtkUnstructuredGrid* input, UnstructuredGridBlock* block)
{
  ::EnqueueGhostsForUnstructuredData(cp, blockId, input, block);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::EnqueueGhosts(const diy::Master::ProxyWithLink& cp,
  const diy::BlockID& blockId, vtkPolyData* input, PolyDataBlock* block)
{
  ::EnqueueGhostsForUnstructuredData(cp, blockId, input, block);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DequeueGhosts(
  const diy::Master::ProxyWithLink& cp, int gid, ImageDataBlockStructure& blockStructure)
{
  ::DequeueGhostsForStructuredData(cp, gid, blockStructure);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DequeueGhosts(
  const diy::Master::ProxyWithLink& cp, int gid, RectilinearGridBlockStructure& blockStructure)
{
  ::DequeueGhostsForStructuredData(cp, gid, blockStructure);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DequeueGhosts(
  const diy::Master::ProxyWithLink& cp, int gid, StructuredGridBlockStructure& blockStructure)
{
  ::DequeueGhostsForStructuredData(cp, gid, blockStructure);
  ::DequeuePoints(cp, gid, blockStructure.GhostPoints);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DequeueGhosts(
  const diy::Master::ProxyWithLink& cp, int gid, UnstructuredGridBlockStructure& blockStructure)
{
  ::DequeueGhostsForUnstructuredData(cp, gid, blockStructure);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DequeueGhosts(
  const diy::Master::ProxyWithLink& cp, int gid, PolyDataBlockStructure& blockStructure)
{
  ::DequeueGhostsForUnstructuredData(cp, gid, blockStructure);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DeepCopyInputAndAllocateGhosts(
  ImageDataBlock* block, vtkImageData* input, vtkImageData* output)
{
  ::DeepCopyInputAndAllocateGhostsForStructuredData(block, input, output);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DeepCopyInputAndAllocateGhosts(
  RectilinearGridBlock* block, vtkRectilinearGrid* input, vtkRectilinearGrid* output)
{
  ::DeepCopyInputAndAllocateGhostsForStructuredData(block, input, output);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DeepCopyInputAndAllocateGhosts(
  StructuredGridBlock* block, vtkStructuredGrid* input, vtkStructuredGrid* output)
{
  ::DeepCopyInputAndAllocateGhostsForStructuredData(block, input, output);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DeepCopyInputAndAllocateGhosts(
  UnstructuredGridBlock* block, vtkUnstructuredGrid* input, vtkUnstructuredGrid* output)
{
  ::DeepCopyInputAndAllocateGhostsForUnstructuredData(block, input, output);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::DeepCopyInputAndAllocateGhosts(
  PolyDataBlock* block, vtkPolyData* input, vtkPolyData* output)
{
  ::DeepCopyInputAndAllocateGhostsForUnstructuredData(block, input, output);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::FillGhostArrays(
  const diy::Master& master, std::vector<vtkImageData*>& outputs, int outputGhostLevels)
{
  ::FillReceivedGhosts(master, outputs, outputGhostLevels);
  // No need to add hidden ghosts when zero layers of ghosts are requested
  if (outputGhostLevels)
  {
    ::FillHiddenGhostsForStructuredData(master, outputs);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::FillGhostArrays(
  const diy::Master& master, std::vector<vtkRectilinearGrid*>& outputs, int outputGhostLevels)
{
  ::FillReceivedGhosts(master, outputs, outputGhostLevels);
  // No need to add hidden ghosts when zero layers of ghosts are requested
  if (outputGhostLevels)
  {
    ::FillHiddenGhostsForStructuredData(master, outputs);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::FillGhostArrays(
  const diy::Master& master, std::vector<vtkStructuredGrid*>& outputs, int outputGhostLevels)
{
  ::FillReceivedGhosts(master, outputs, outputGhostLevels);
  // No need to add hidden ghosts when zero layers of ghosts are requested
  if (outputGhostLevels)
  {
    ::FillHiddenGhostsForStructuredData(master, outputs);
  }
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::FillGhostArrays(
  const diy::Master& master, std::vector<vtkUnstructuredGrid*>& outputs, int outputGhostLevels)
{
  ::FillReceivedGhosts(master, outputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
void vtkDIYGhostUtilities::FillGhostArrays(
  const diy::Master& master, std::vector<vtkPolyData*>& outputs, int outputGhostLevels)
{
  ::FillReceivedGhosts(master, outputs, outputGhostLevels);
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::GenerateGhostCellsImageData(std::vector<vtkImageData*>& inputs,
  std::vector<vtkImageData*>& outputs, int outputGhostLevels, vtkMultiProcessController* controller)
{
  return vtkDIYGhostUtilities::GenerateGhostCells(inputs, outputs, outputGhostLevels, controller);
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::GenerateGhostCellsRectilinearGrid(
  std::vector<vtkRectilinearGrid*>& inputs, std::vector<vtkRectilinearGrid*>& outputs,
  int outputGhostLevels, vtkMultiProcessController* controller)
{
  return vtkDIYGhostUtilities::GenerateGhostCells(inputs, outputs, outputGhostLevels, controller);
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::GenerateGhostCellsStructuredGrid(std::vector<vtkStructuredGrid*>& inputs,
  std::vector<vtkStructuredGrid*>& outputs, int outputGhostLevels,
  vtkMultiProcessController* controller)
{
  return vtkDIYGhostUtilities::GenerateGhostCells(inputs, outputs, outputGhostLevels, controller);
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::GenerateGhostCellsPolyData(std::vector<vtkPolyData*>& inputs,
  std::vector<vtkPolyData*>& outputs, int outputGhostLevels, vtkMultiProcessController* controller)
{
  return vtkDIYGhostUtilities::GenerateGhostCells(inputs, outputs, outputGhostLevels, controller);
}

//----------------------------------------------------------------------------
int vtkDIYGhostUtilities::GenerateGhostCellsUnstructuredGrid(
  std::vector<vtkUnstructuredGrid*>& inputs, std::vector<vtkUnstructuredGrid*>& outputs,
  int outputGhostLevels, vtkMultiProcessController* controller)
{
  return vtkDIYGhostUtilities::GenerateGhostCells(inputs, outputs, outputGhostLevels, controller);
}
VTK_ABI_NAMESPACE_END
