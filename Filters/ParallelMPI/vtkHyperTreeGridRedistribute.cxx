// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkHyperTreeGridRedistribute.h"

#include "vtkAbstractArray.h"
#include "vtkBitArray.h"
#include "vtkCellData.h"
#include "vtkCommunicator.h"
#include "vtkHyperTree.h"
#include "vtkHyperTreeGrid.h"
#include "vtkHyperTreeGridNonOrientedCursor.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMPICommunicator.h"
#include "vtkMultiProcessController.h"
#include "vtkPartitionedDataSet.h"
#include "vtkSetGet.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkType.h"
#include "vtkTypeInt64Array.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <vector>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkHyperTreeGridRedistribute);
vtkCxxSetObjectMacro(vtkHyperTreeGridRedistribute, Controller, vtkMultiProcessController);

namespace
{
void CountTreeCells(vtkHyperTreeGridNonOrientedCursor* cursor, int& countCells, int& countMask)
{
  countMask++;

  if (cursor->IsMasked())
  {
    return;
  }

  countCells++;

  if (!cursor->IsLeaf())
  {
    for (int ichild = 0; ichild < cursor->GetNumberOfChildren(); ++ichild)
    {
      cursor->ToChild(ichild);
      ::CountTreeCells(cursor, countCells, countMask);
      cursor->ToParent();
    }
  }
}

void CollectArray(vtkHyperTreeGridNonOrientedCursor* cursor, vtkAbstractArray* arraySource,
  vtkAbstractArray* arrayDest, int& count)
{
  if (cursor->IsMasked())
  {
    return;
  }

  arrayDest->SetTuple(count++, cursor->GetGlobalNodeIndex(), arraySource);

  if (!cursor->IsLeaf())
  {
    for (int ichild = 0; ichild < cursor->GetNumberOfChildren(); ++ichild)
    {
      cursor->ToChild(ichild);
      ::CollectArray(cursor, arraySource, arrayDest, count);
      cursor->ToParent();
    }
  }
}

void CollectMask(vtkHyperTreeGridNonOrientedCursor* cursor, vtkBitArray* maskBuffer, int& offset)
{
  maskBuffer->InsertValue(offset++, cursor->IsMasked());

  if (cursor->IsMasked())
  {
    return;
  }

  if (!cursor->IsLeaf())
  {
    for (int ichild = 0; ichild < cursor->GetNumberOfChildren(); ++ichild)
    {
      cursor->ToChild(ichild);
      ::CollectMask(cursor, maskBuffer, offset);
      cursor->ToParent();
    }
  }
}

void SetMaskValues(vtkHyperTreeGridNonOrientedCursor* cursor, vtkBitArray* maskBuffer,
  vtkBitArray* destMask, int& offset)
{
  bool maskValue = maskBuffer->GetValue(offset++);
  destMask->InsertValue(cursor->GetGlobalNodeIndex(), maskValue);

  if (maskValue)
  {
    return;
  }

  if (!cursor->IsLeaf())
  {
    for (int ichild = 0; ichild < cursor->GetNumberOfChildren(); ++ichild)
    {
      cursor->ToChild(ichild);
      ::SetMaskValues(cursor, maskBuffer, destMask, offset);
      cursor->ToParent();
    }
  }
}

void SetArrayValues(vtkHyperTreeGridNonOrientedCursor* cursor, vtkAbstractArray* sourceArray,
  vtkAbstractArray* destArray, int& recvReadOffset)
{
  if (cursor->IsMasked())
  {
    return;
  }

  destArray->SetTuple(cursor->GetGlobalNodeIndex(), recvReadOffset++, sourceArray);
  if (!cursor->IsLeaf())
  {
    for (int ichild = 0; ichild < cursor->GetNumberOfChildren(); ++ichild)
    {
      cursor->ToChild(ichild);
      SetArrayValues(cursor, sourceArray, destArray, recvReadOffset);
      cursor->ToParent();
    }
  }
}

void CopyArrayValues(vtkHyperTreeGridNonOrientedCursor* inCursor,
  vtkHyperTreeGridNonOrientedCursor* outCursor, vtkBitArray* outMask, vtkAbstractArray* sourceArray,
  vtkAbstractArray* destArray)
{
  outMask->InsertValue(outCursor->GetGlobalNodeIndex(), inCursor->IsMasked());

  if (inCursor->IsMasked())
  {
    return;
  }

  destArray->SetTuple(outCursor->GetGlobalNodeIndex(), inCursor->GetGlobalNodeIndex(), sourceArray);

  if (!inCursor->IsLeaf())
  {
    for (int ichild = 0; ichild < inCursor->GetNumberOfChildren(); ++ichild)
    {
      inCursor->ToChild(ichild);
      outCursor->ToChild(ichild);

      ::CopyArrayValues(inCursor, outCursor, outMask, sourceArray, destArray);

      inCursor->ToParent();
      outCursor->ToParent();
    }
  }
}

/**
 * Get the number of bytes required to fit `nbBits`.
 * Used to align bit buffers with byte boundaries for multi-process transfers
 */
constexpr int GetNumberOfBytes(int nbBits)
{
  return ((nbBits + 7) / 8);
}
};

//------------------------------------------------------------------------------
vtkHyperTreeGridRedistribute::vtkHyperTreeGridRedistribute()
{
  this->AppropriateOutput = true;
  this->SetController(vtkMultiProcessController::GetGlobalController());
}

//------------------------------------------------------------------------------
vtkHyperTreeGridRedistribute::~vtkHyperTreeGridRedistribute()
{
  this->SetController(nullptr);
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//------------------------------------------------------------------------------
vtkMultiProcessController* vtkHyperTreeGridRedistribute::GetController()
{
  return this->Controller.Get();
}

//------------------------------------------------------------------------------
int vtkHyperTreeGridRedistribute::FillInputPortInformation(int, vtkInformation* info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkHyperTreeGrid");
  info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPartitionedDataSet");
  return 1;
}

//------------------------------------------------------------------------------
int vtkHyperTreeGridRedistribute::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  this->UpdateProgress(0.);

  vtkInformation* info = outputVector->GetInformationObject(0);
  int currentPiece = info->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());
  this->NumPartitions = this->Controller->GetNumberOfProcesses();

  // Make sure input is either a HTG or a PartitionedDataSet that contains a HTG piece.
  this->InputHTG = vtkHyperTreeGrid::GetData(inputVector[0], 0);
  vtkPartitionedDataSet* inputPDS = vtkPartitionedDataSet::GetData(inputVector[0], 0);

  if (!inputPDS && !this->InputHTG)
  {
    vtkErrorMacro("Input data is neither HTG or PartitionedDataSet. Cannot proceed with ghost cell "
                  "generation.");
    return 0;
  }

  vtkHyperTreeGrid* outputHTG = vtkHyperTreeGrid::GetData(outputVector, 0);
  vtkPartitionedDataSet* outputPDS = vtkPartitionedDataSet::GetData(outputVector, 0);
  if (outputPDS)
  {
    outputPDS->CopyStructure(inputPDS);
  }
  if (inputPDS && outputPDS)
  {
    for (unsigned int partId = 0; partId < inputPDS->GetNumberOfPartitions(); partId++)
    {
      auto partHTG = vtkHyperTreeGrid::SafeDownCast(inputPDS->GetPartitionAsDataObject(partId));
      if (partHTG)
      {
        if (this->InputHTG)
        {
          vtkWarningMacro("Found more than one non-null HTG in the partitioned dataset for piece "
            << currentPiece << ". Generating ghost data only for partition " << partId);
        }
        this->InputHTG = partHTG;
        vtkNew<vtkHyperTreeGrid> newOutputHTG;
        outputPDS->SetPartition(partId, newOutputHTG);
        outputHTG = newOutputHTG; // Not dangling, outputPDS maintains a reference.
      }
    }
  }

  if (!outputHTG && !outputPDS)
  {
    vtkErrorMacro("No output available. Cannot proceed with hyper tree grid algorithm.");
    return 0;
  }

  if (!this->InputHTG)
  {
    vtkWarningMacro("Incorrect HTG for piece " << currentPiece);
  }

  // Make sure every HTG piece has a correct extent and can be processed.
  // This way, we make sure the `ProcessTrees` function will either be executed by all ranks
  // or by none, and avoids getting stuck on barriers.
  int correctExtent = this->InputHTG &&
    this->InputHTG->GetExtent()[0] <= this->InputHTG->GetExtent()[1] &&
    this->InputHTG->GetExtent()[2] <= this->InputHTG->GetExtent()[3] &&
    this->InputHTG->GetExtent()[4] <= this->InputHTG->GetExtent()[5];

  if (!correctExtent)
  {
    vtkWarningMacro("Piece " << currentPiece << " does not have a valid extend. Cannot process.");
  }

  int allCorrect = 1; // Reduction operation cannot be done on bools
  this->Controller->AllReduce(&correctExtent, &allCorrect, 1, vtkCommunicator::LOGICAL_AND_OP);

  if (!allCorrect)
  {
    vtkWarningMacro("Every individual distributed process does not have a valid HTG extent. No "
                    "ghost cells will be generated.");
    if (outputHTG)
    {
      outputHTG->ShallowCopy(this->InputHTG);
    }
    return 1;
  }
  else if (!this->ProcessTrees(this->InputHTG, outputHTG))
  {
    return 0;
  }

  // Update progress and return
  this->UpdateProgress(1.);
  return 1;
}

//------------------------------------------------------------------------------
int vtkHyperTreeGridRedistribute::ProcessTrees(vtkHyperTreeGrid* input, vtkDataObject* outputDO)
{
  this->OutputHTG = vtkHyperTreeGrid::SafeDownCast(outputDO);
  if (!this->OutputHTG)
  {
    vtkErrorMacro("Incorrect type of output: " << outputDO->GetClassName());
    return 0;
  }

  this->OutputHTG->CopyEmptyStructure(input);
  this->OutputHTG->GetCellData()->CopyStructure(this->InputHTG->GetCellData());

  if (this->InputHTG->HasMask())
  {
    this->OutMask = vtk::TakeSmartPointer(vtkBitArray::New());
  }

  vtkDebugMacro("Collecting local tree indices");
  this->CollectLocalTreeIds();
  this->UpdateProgress(0.2);

  // Compute tree id <=> target partition id map
  vtkDebugMacro("Build target partition map");
  this->TreeTargetPartId.resize(this->InputHTG->GetMaxNumberOfTrees());
  this->BuildTargetPartMap();
  this->UpdateProgress(0.4);

  vtkDebugMacro("Exchanging HTG meta-data");
  vtkNew<vtkBitArray> descriptorSendBuffer;
  std::vector<int> descriptorSizesReceivedBuffer;
  std::vector<int> treeSizesSendBuffer;
  std::vector<int> maskSizesSendBuffer;
  std::vector<int> treeSizesReceivedBuffer;
  std::vector<int> maskSizesReceivedBuffer;
  std::vector<int> descriptorsByteOffsets;
  this->ExchangeHTGMetaData(descriptorSendBuffer, descriptorSizesReceivedBuffer,
    treeSizesSendBuffer, maskSizesSendBuffer, treeSizesReceivedBuffer, maskSizesReceivedBuffer,
    descriptorsByteOffsets);

  vtkDebugMacro("Building output trees");
  this->BuildOutputTrees(
    descriptorSendBuffer, descriptorSizesReceivedBuffer, descriptorsByteOffsets);
  this->OutputHTG->InitializeLocalIndexNode();
  this->UpdateProgress(0.6);

  if (this->InputHTG->HasMask())
  {
    vtkDebugMacro("Exchange mask information");
    this->ExchangeMask(maskSizesSendBuffer, maskSizesReceivedBuffer);
  }
  this->UpdateProgress(0.8);

  vtkDebugMacro("Exchange cell data");
  std::vector<int> cellsSentPerPartOffset(this->NumPartitions, 0);
  std::vector<int> cellsReceivedPerPartOffset(this->NumPartitions, 0);
  std::vector<int> nbCellDataSentPerPart(this->NumPartitions, 0);
  std::vector<int> nbCellDataReceivedPerPart(this->NumPartitions, 0);
  this->CollectCellArraySizes(treeSizesSendBuffer, treeSizesReceivedBuffer, cellsSentPerPartOffset,
    cellsReceivedPerPartOffset, nbCellDataSentPerPart, nbCellDataReceivedPerPart);

  for (int arrayId = 0; arrayId < this->OutputHTG->GetCellData()->GetNumberOfArrays(); arrayId++)
  {
    this->ExchangeCellArray(arrayId, cellsSentPerPartOffset, cellsReceivedPerPartOffset,
      nbCellDataSentPerPart, nbCellDataReceivedPerPart);
  }

  // Free up memory for the heavier arrays
  this->TreesToSend.clear();
  this->LocalTreeIds.clear();
  this->TreeTargetPartId.clear();
  this->TreeIdsReceivedBuffer.clear();

  return 1;
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::CollectLocalTreeIds()
{
  vtkNew<vtkHyperTreeGridNonOrientedCursor> cursor;
  vtkHyperTreeGrid::vtkHyperTreeGridIterator inputIterator;
  vtkIdType inTreeIndex = 0;
  this->InputHTG->InitializeTreeIterator(inputIterator);

  vtkIdType numLocalTrees = this->InputHTG->GetNumberOfNonEmptyTrees();
  this->LocalTreeIds.resize(numLocalTrees);

  vtkIdType treeCount = 0;
  while (inputIterator.GetNextTree(inTreeIndex))
  {
    this->InputHTG->InitializeNonOrientedCursor(cursor, inTreeIndex);
    if (cursor->HasTree())
    {
      this->LocalTreeIds[treeCount++] = inTreeIndex;
    }
  }
}
//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::BuildTargetPartMap()
{
  /*
   * The strategy to distribute HyperTrees used is currently very simple:
   * all the partitions should contain the same number of trees.
   * There may be better strategies to group trees spatially, and balance partitions
   * using the number of cells in each tree
   */

  vtkIdType maxTrees = this->InputHTG->GetMaxNumberOfTrees();

  for (vtkIdType part = 0; part < this->NumPartitions; part++)
  {
    vtkIdType startId = std::ceil(static_cast<double>(part * maxTrees) / this->NumPartitions);
    vtkIdType endId =
      std::ceil(static_cast<double>((part + 1) * maxTrees) / this->NumPartitions - 1.0);
    for (vtkIdType id = startId; id <= endId; id++)
    {
      this->TreeTargetPartId[id] = part;
    }
  }

  // Compute which trees to send to which processes
  this->TreesToSend.resize(this->NumPartitions);
  for (vtkIdType& id : this->LocalTreeIds)
  {
    if (this->TreeTargetPartId[id] != this->Controller->GetLocalProcessId())
    {
      this->TreesToSend[this->TreeTargetPartId[id]].emplace_back(id);
    }
  }
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::ExchangeHTGMetaData(vtkBitArray* descriptorSendBuffer,
  std::vector<int>& descriptorSizesReceivedBuffer, std::vector<int>& treeSizesSendBuffer,
  std::vector<int>& maskSizesSendBuffer, std::vector<int>& treeSizesReceivedBuffer,
  std::vector<int>& maskSizesReceivedBuffer, std::vector<int>& descriptorsByteOffsets)
{
  std::vector<int> descriptorSizesSendBuffer;
  std::vector<int> treeIdSendBuffer;

  this->NbTreesSentPerPart.resize(this->NumPartitions);
  std::vector<int> treeTargetPartId(this->NumPartitions, 0);
  this->NbDescriptorsBytesPerPart.resize(this->NumPartitions);
  descriptorsByteOffsets.resize(this->NumPartitions, 0);

  // Browse local trees to collect information about descriptor size, number of cell data and mask
  // size. These are all different informations because there is no descriptor for the lowest level,
  // and masked cells have a value for the mask array but not for cell data arrays.
  vtkNew<vtkHyperTreeGridNonOrientedCursor> cursor;
  for (vtkIdType part = 0; part < this->NumPartitions; part++)
  {
    int descriptorSizeCum = 0;
    for (vtkIdType& id : this->TreesToSend[part])
    {
      vtkNew<vtkTypeInt64Array> numberOfVerticesPerDepth;
      vtkNew<vtkBitArray> descriptor;
      vtkNew<vtkIdList> breadthFirstIdMap;
      this->InputHTG->GetTree(id)->ComputeBreadthFirstOrderDescriptor(
        this->InputHTG->GetDepthLimiter(), this->InputHTG->GetMask(), numberOfVerticesPerDepth,
        descriptor, breadthFirstIdMap);
      descriptorSizesSendBuffer.emplace_back(descriptor->GetNumberOfTuples());
      treeIdSendBuffer.emplace_back(id);

      descriptorSendBuffer->InsertTuples(
        descriptorSendBuffer->GetNumberOfTuples(), descriptor->GetNumberOfTuples(), 0, descriptor);
      descriptorSizeCum += descriptor->GetNumberOfTuples();

      this->InputHTG->InitializeNonOrientedCursor(cursor, id);
      int countCells = 0, countMask = 0;
      ::CountTreeCells(cursor, countCells, countMask);
      treeSizesSendBuffer.emplace_back(countCells);
      maskSizesSendBuffer.emplace_back(countMask);
    }

    // Make sure that we're starting the partition on a full byte
    int byteAlignedSize = ::GetNumberOfBytes(descriptorSizeCum) * 8;
    descriptorSendBuffer->Resize(
      descriptorSendBuffer->GetNumberOfTuples() + (byteAlignedSize - descriptorSizeCum));

    this->NbDescriptorsBytesPerPart[part] = byteAlignedSize / 8;
    if (part > 0)
    {
      descriptorsByteOffsets[part] =
        descriptorsByteOffsets[part - 1] + this->NbDescriptorsBytesPerPart[part - 1];
    }

    this->NbTreesSentPerPart[part] = static_cast<int>(this->TreesToSend[part].size());
    if (part > 0)
    {
      treeTargetPartId[part] = treeTargetPartId[part - 1] + this->NbTreesSentPerPart[part - 1];
    }
  }

  // Exchange number of tree ids to send
  std::vector<int> treesToSendGathered(this->NumPartitions * this->NumPartitions);
  this->Controller->AllGather(
    this->NbTreesSentPerPart.data(), treesToSendGathered.data(), this->NumPartitions);

  this->NbTreesReceivedPerPart.resize(this->NumPartitions);
  std::vector<int> treeReceivedOffsets(this->NumPartitions, 0);
  for (int i = 0; i < this->NumPartitions; i++)
  {
    this->NbTreesReceivedPerPart[i] =
      treesToSendGathered[this->NumPartitions * i + this->Controller->GetLocalProcessId()];
    if (i > 0)
    {
      treeReceivedOffsets[i] = this->NbTreesReceivedPerPart[i - 1] + treeReceivedOffsets[i - 1];
    }
  }

  // Exchange descriptors sizes and tree ids
  vtkIdType totalNbTreesReceived =
    std::accumulate(this->NbTreesReceivedPerPart.begin(), this->NbTreesReceivedPerPart.end(), 0);

  // Exchange tree ids
  this->TreeIdsReceivedBuffer.resize(totalNbTreesReceived);
  this->MPIComm = vtkMPICommunicator::SafeDownCast(this->Controller->GetCommunicator());

  this->MPIComm->AllToAllVVoidArray(treeIdSendBuffer.data(), this->NbTreesSentPerPart.data(),
    treeTargetPartId.data(), this->TreeIdsReceivedBuffer.data(),
    this->NbTreesReceivedPerPart.data(), treeReceivedOffsets.data(), VTK_INT);

  // Exchange descriptor sizes
  descriptorSizesReceivedBuffer.resize(totalNbTreesReceived);
  this->MPIComm->AllToAllVVoidArray(descriptorSizesSendBuffer.data(),
    this->NbTreesSentPerPart.data(), treeTargetPartId.data(), descriptorSizesReceivedBuffer.data(),
    this->NbTreesReceivedPerPart.data(), treeReceivedOffsets.data(), VTK_INT);

  // Exchange number of cells per tree
  treeSizesReceivedBuffer.resize(totalNbTreesReceived);
  this->MPIComm->AllToAllVVoidArray(treeSizesSendBuffer.data(), this->NbTreesSentPerPart.data(),
    treeTargetPartId.data(), treeSizesReceivedBuffer.data(), this->NbTreesReceivedPerPart.data(),
    treeReceivedOffsets.data(), VTK_INT);

  // Exchange mask size (in bits)
  maskSizesReceivedBuffer.resize(totalNbTreesReceived);
  this->MPIComm->AllToAllVVoidArray(maskSizesSendBuffer.data(), this->NbTreesSentPerPart.data(),
    treeTargetPartId.data(), maskSizesReceivedBuffer.data(), this->NbTreesReceivedPerPart.data(),
    treeReceivedOffsets.data(), VTK_INT);
}
//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::BuildOutputTrees(vtkBitArray* descriptorSendBuffer,
  std::vector<int>& descriptorSizesReceivedBuffer, std::vector<int>& descriptorsByteOffsets)
{
  // Build output trees from descriptors received from other processes

  std::vector<int> descriptorRecvSizePerPart(this->NumPartitions);
  std::vector<int> descriptorRecvOffsets(this->NumPartitions, 0);
  for (int part = 0, treeId = 0; part < this->NumPartitions; part++)
  {
    int descriptorTotalSize = 0;
    for (int tree = 0; tree < this->NbTreesReceivedPerPart[part]; tree++)
    {
      descriptorTotalSize += descriptorSizesReceivedBuffer[treeId];
      treeId++;
    }

    // Round to the nearest higher byte size
    int byteAlignedSize = ::GetNumberOfBytes(descriptorTotalSize) * 8;
    descriptorRecvSizePerPart[part] = byteAlignedSize / 8;
    if (part > 0)
    {
      descriptorRecvOffsets[part] =
        descriptorRecvOffsets[part - 1] + descriptorRecvSizePerPart[part - 1];
    }
  }

  int totalRecvSize =
    std::accumulate(descriptorRecvSizePerPart.begin(), descriptorRecvSizePerPart.end(), 0);
  vtkNew<vtkBitArray> descriptorRecv;
  descriptorRecv->SetNumberOfTuples(totalRecvSize * 8);

  this->MPIComm->AllToAllVVoidArray(descriptorSendBuffer->GetVoidPointer(0),
    this->NbDescriptorsBytesPerPart.data(), descriptorsByteOffsets.data(),
    descriptorRecv->GetVoidPointer(0), descriptorRecvSizePerPart.data(),
    descriptorRecvOffsets.data(), VTK_UNSIGNED_CHAR);

  for (int part = 0, currentTreeId = 0, descriptorReadOffset = 0; part < this->NumPartitions;
       part++)
  {
    for (int id = 0; id < this->NbTreesReceivedPerPart[part]; id++)
    {
      int descriptorSize = descriptorSizesReceivedBuffer[currentTreeId];

      vtkHyperTree* newTree =
        this->OutputHTG->GetTree(this->TreeIdsReceivedBuffer[currentTreeId], true);
      newTree->BuildFromBreadthFirstOrderDescriptor(
        descriptorRecv, descriptorSize, descriptorReadOffset);

      descriptorReadOffset += descriptorSize;

      currentTreeId++;
    }

    // When changing rank we reveive descriptors from, make sure we're reading a new byte
    descriptorReadOffset = ::GetNumberOfBytes(descriptorReadOffset) * 8;
  }

  // Add existing trees
  for (vtkIdType& id : this->LocalTreeIds)
  {
    if (this->TreeTargetPartId[id] == this->Controller->GetLocalProcessId())
    {
      assert(this->InputHTG->GetTree(id));
      vtkHyperTree* ht = this->OutputHTG->GetTree(id, true);
      ht->CopyStructure(this->InputHTG->GetTree(id));
    }
  }
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::ExchangeMask(
  std::vector<int>& maskSizesSendBuffer, std::vector<int>& maskSizesReceivedBuffer)
{
  // Collect number of mask values sent
  std::vector<int> maskSendBytesPerPart(this->NumPartitions, 0);
  std::vector<int> maskSendOffsetsPerPart(this->NumPartitions);
  for (int part = 0, count = 0; part < this->NumPartitions; part++)
  {
    for (int id = 0; id < this->NbTreesSentPerPart[part]; id++)
    {
      maskSendBytesPerPart[part] += maskSizesSendBuffer[count++];
    }
    // Count in bytes
    maskSendBytesPerPart[part] = ::GetNumberOfBytes(maskSendBytesPerPart[part]);

    if (part > 0)
    {
      maskSendOffsetsPerPart[part] =
        maskSendBytesPerPart[part - 1] + maskSendOffsetsPerPart[part - 1];
    }
  }

  // Collect number of mask values to be received
  std::vector<int> maskRecvBytesPerPart(this->NumPartitions);
  std::vector<int> maskRecvOffsetsPerPart(this->NumPartitions);
  for (int part = 0, count = 0; part < this->NumPartitions; part++)
  {
    for (int id = 0; id < this->NbTreesReceivedPerPart[part]; id++)
    {
      maskRecvBytesPerPart[part] += maskSizesReceivedBuffer[count++];
    }

    maskRecvBytesPerPart[part] = ::GetNumberOfBytes(maskRecvBytesPerPart[part]);

    if (part > 0)
    {
      maskRecvOffsetsPerPart[part] =
        maskRecvOffsetsPerPart[part - 1] + maskRecvBytesPerPart[part - 1];
    }
  }
  int totalNbMaskBytesReceived =
    std::accumulate(maskRecvBytesPerPart.begin(), maskRecvBytesPerPart.end(), 0);

  // Collect mask values
  vtkNew<vtkBitArray> maskSendBuffer;
  int sendMaskOffsetBits = 0;
  vtkNew<vtkHyperTreeGridNonOrientedCursor> maskCursor;
  for (int part = 0; part < this->NumPartitions; part++)
  {
    for (size_t id = 0; id < this->TreesToSend[part].size(); id++)
    {
      this->InputHTG->InitializeNonOrientedCursor(maskCursor, this->TreesToSend[part][id]);
      ::CollectMask(maskCursor, maskSendBuffer, sendMaskOffsetBits);
    }

    // Add padding to finish on a full byte
    sendMaskOffsetBits = ::GetNumberOfBytes(sendMaskOffsetBits) * 8;
  }

  // Exchange received mask sizes

  vtkNew<vtkBitArray> maskRecvBuffer;
  maskRecvBuffer->SetNumberOfValues(totalNbMaskBytesReceived * 8);

  // Exchange masks
  this->MPIComm->AllToAllVVoidArray(maskSendBuffer->GetVoidPointer(0), maskSendBytesPerPart.data(),
    maskSendOffsetsPerPart.data(), maskRecvBuffer->GetVoidPointer(0), maskRecvBytesPerPart.data(),
    maskRecvOffsetsPerPart.data(), VTK_UNSIGNED_CHAR);

  int recvMaskOffset = 0;
  vtkNew<vtkHyperTreeGridNonOrientedCursor> cursor;
  for (int part = 0, treeId = 0; part < this->NumPartitions; part++)
  {
    for (int tree = 0; tree < this->NbTreesReceivedPerPart[part]; tree++)
    {
      int currentTreeId = this->TreeIdsReceivedBuffer[treeId++];
      this->OutputHTG->InitializeNonOrientedCursor(cursor, currentTreeId);
      ::SetMaskValues(cursor, maskRecvBuffer, this->OutMask, recvMaskOffset);
    }

    recvMaskOffset = ::GetNumberOfBytes(recvMaskOffset) * 8;
  }

  this->OutMask->Squeeze();
  this->OutputHTG->SetMask(this->OutMask);
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::CollectCellArraySizes(std::vector<int>& treeSizesSendBuffer,
  std::vector<int>& treeSizesReceivedBuffer, std::vector<int>& cellsSentPerPartOffset,
  std::vector<int>& cellsReceivedPerPartOffset, std::vector<int>& nbCellDataSentPerPart,
  std::vector<int>& nbCellDataReceivedPerPart)
{
  // Collect cell data sent sizes
  for (int part = 0, count = 0; part < this->NumPartitions; part++)
  {
    for (int id = 0; id < this->NbTreesSentPerPart[part]; id++)
    {
      nbCellDataSentPerPart[part] += treeSizesSendBuffer[count++];
    }
    if (part > 0)
    {
      cellsSentPerPartOffset[part] =
        cellsSentPerPartOffset[part - 1] + nbCellDataSentPerPart[part - 1];
    }
  }

  // Collect number of cell data received
  for (int part = 0, count = 0; part < this->NumPartitions; part++)
  {
    for (int id = 0; id < this->NbTreesReceivedPerPart[part]; id++)
    {
      nbCellDataReceivedPerPart[part] += treeSizesReceivedBuffer[count++];
    }
    if (part > 0)
    {
      cellsReceivedPerPartOffset[part] =
        cellsReceivedPerPartOffset[part - 1] + nbCellDataReceivedPerPart[part - 1];
    }
  }
}

//------------------------------------------------------------------------------
void vtkHyperTreeGridRedistribute::ExchangeCellArray(int arrayId,
  std::vector<int>& cellsSentPerPartOffset, std::vector<int>& cellsReceivedPerPartOffset,
  std::vector<int>& nbCellDataSentPerPart, std::vector<int>& nbCellDataReceivedPerPart)
{
  vtkAbstractArray* inputArray = this->InputHTG->GetCellData()->GetArray(arrayId);
  int numComp = inputArray->GetNumberOfComponents();

  vtkSmartPointer<vtkAbstractArray> cellDataSendArrayBuffer =
    vtk::TakeSmartPointer(inputArray->NewInstance());
  cellDataSendArrayBuffer->SetNumberOfComponents(numComp);
  int totalNbCellsSent =
    std::accumulate(nbCellDataSentPerPart.begin(), nbCellDataSentPerPart.end(), 0);
  cellDataSendArrayBuffer->SetNumberOfTuples(totalNbCellsSent);

  // Extract data from local trees
  int countCells = 0;
  vtkNew<vtkHyperTreeGridNonOrientedCursor> cursor;
  for (int part = 0; part < this->NumPartitions; part++)
  {
    for (size_t id = 0; id < this->TreesToSend[part].size(); id++)
    {
      this->InputHTG->InitializeNonOrientedCursor(cursor, this->TreesToSend[part][id]);
      ::CollectArray(cursor, inputArray, cellDataSendArrayBuffer, countCells);
    }
  }

  int totalNbCellsReceived =
    std::accumulate(nbCellDataReceivedPerPart.begin(), nbCellDataReceivedPerPart.end(), 0);

  // Prepare input send/recv structures
  vtkSmartPointer<vtkAbstractArray> cellDataReceivedBuffer =
    vtk::TakeSmartPointer(inputArray->NewInstance());
  cellDataReceivedBuffer->SetNumberOfComponents(numComp);
  cellDataReceivedBuffer->SetNumberOfTuples(totalNbCellsReceived);

  std::vector<int> cellDataSentSizes(this->NumPartitions);
  std::transform(nbCellDataSentPerPart.begin(), nbCellDataSentPerPart.end(),
    cellDataSentSizes.begin(), [numComp](int elem) { return elem * numComp; });
  std::vector<int> cellDataSentOffsets(this->NumPartitions);
  std::transform(cellsSentPerPartOffset.begin(), cellsSentPerPartOffset.end(),
    cellDataSentOffsets.begin(), [numComp](int elem) { return elem * numComp; });
  std::vector<int> cellDataReceivedSizes(this->NumPartitions);
  std::transform(nbCellDataReceivedPerPart.begin(), nbCellDataReceivedPerPart.end(),
    cellDataReceivedSizes.begin(), [numComp](int elem) { return elem * numComp; });
  std::vector<int> cellDataReceivedOffsets(this->NumPartitions);
  std::transform(cellsReceivedPerPartOffset.begin(), cellsReceivedPerPartOffset.end(),
    cellDataReceivedOffsets.begin(), [numComp](int elem) { return elem * numComp; });

  // Exchange cell data information
  this->MPIComm->AllToAllVVoidArray(cellDataSendArrayBuffer->GetVoidPointer(0),
    cellDataSentSizes.data(), cellDataSentOffsets.data(), cellDataReceivedBuffer->GetVoidPointer(0),
    cellDataReceivedSizes.data(), cellDataReceivedOffsets.data(), inputArray->GetDataType());

  // Iterate over trees received
  vtkAbstractArray* outputDataArray = this->OutputHTG->GetCellData()->GetArray(arrayId);
  outputDataArray->SetNumberOfTuples(this->OutputHTG->GetNumberOfCells());

  for (int part = 0, treeId = 0, recvReadOffset = 0; part < this->NumPartitions; part++)
  {
    for (int tree = 0; tree < this->NbTreesReceivedPerPart[part]; tree++)
    {
      int currentTreeId = this->TreeIdsReceivedBuffer[treeId++];
      this->OutputHTG->InitializeNonOrientedCursor(cursor, currentTreeId);
      ::SetArrayValues(cursor, cellDataReceivedBuffer, outputDataArray, recvReadOffset);
    }
  }

  // Iterate over local trees and copy cell data values from the input
  for (vtkIdType& id : this->LocalTreeIds)
  {
    if (this->TreeTargetPartId[id] == this->Controller->GetLocalProcessId())
    {
      vtkNew<vtkHyperTreeGridNonOrientedCursor> inCursor;
      vtkNew<vtkHyperTreeGridNonOrientedCursor> outCursor;

      this->InputHTG->InitializeNonOrientedCursor(inCursor, id);
      this->OutputHTG->InitializeNonOrientedCursor(outCursor, id);

      ::CopyArrayValues(inCursor, outCursor, this->OutMask, inputArray, outputDataArray);
    }
  }

  inputArray->Squeeze();
}

VTK_ABI_NAMESPACE_END
