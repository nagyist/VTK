// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkGroupDataSetsFilter.h"

#include "vtkConvertToMultiBlockDataSet.h"
#include "vtkConvertToPartitionedDataSetCollection.h"
#include "vtkDataSet.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMultiPieceDataSet.h"
#include "vtkObjectFactory.h"
#include "vtkPartitionedDataSet.h"
#include "vtkPartitionedDataSetCollection.h"
#include "vtkSmartPointer.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkType.h"

#include <cmath>
#include <string>
#include <vector>

#include <vtk_fmt.h>
// clang-format off
#include VTK_FMT(fmt/format.h)
// clang-format on

VTK_ABI_NAMESPACE_BEGIN
class vtkGroupDataSetsFilter::vtkInternals
{
public:
  std::vector<std::string> Names;

  std::string GetName(int index, int dataType, int precision) const
  {
    if (dataType == VTK_PARTITIONED_DATA_SET)
    {
      // VTK_PARTITIONED_DATA_SET doesn't use names.
      return {};
    }

    if (index >= 0 && index < static_cast<int>(this->Names.size()) && !this->Names[index].empty())
    {
      return this->Names[index];
    }

    return fmt::format("Block {:0{}d}", index, precision);
  }
};

vtkStandardNewMacro(vtkGroupDataSetsFilter);
//----------------------------------------------------------------------------
vtkGroupDataSetsFilter::vtkGroupDataSetsFilter()
  : OutputType(VTK_PARTITIONED_DATA_SET_COLLECTION)
  , Internals(new vtkGroupDataSetsFilter::vtkInternals())
{
}

//----------------------------------------------------------------------------
vtkGroupDataSetsFilter::~vtkGroupDataSetsFilter() = default;

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::SetOutputTypeToPartitionedDataSet()
{
  this->SetOutputType(VTK_PARTITIONED_DATA_SET);
}

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::SetOutputTypeToPartitionedDataSetCollection()
{
  this->SetOutputType(VTK_PARTITIONED_DATA_SET_COLLECTION);
}

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::SetOutputTypeToMultiBlockDataSet()
{
  this->SetOutputType(VTK_MULTIBLOCK_DATA_SET);
}

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::SetInputName(int index, const char* name)
{
  if (index < 0)
  {
    vtkErrorMacro("Invalid index specified '" << index << "'.");
    return;
  }

  const std::string safeName(name ? name : "");
  auto& internals = (*this->Internals);
  try
  {
    auto& currentName = internals.Names.at(index);
    if (currentName != safeName)
    {
      currentName = safeName;
      this->Modified();
    }
  }
  catch (std::out_of_range&)
  {
    internals.Names.resize(index + 1);
    internals.Names[index] = safeName;
    this->Modified();
  }
}

//----------------------------------------------------------------------------
const char* vtkGroupDataSetsFilter::GetInputName(int index) const
{
  auto& internals = (*this->Internals);
  return (index >= 0 && index < static_cast<int>(internals.Names.size()))
    ? internals.Names.at(index).c_str()
    : nullptr;
}

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::ClearInputNames()
{
  auto& internals = (*this->Internals);
  if (!internals.Names.empty())
  {
    internals.Names.clear();
    this->Modified();
  }
}

//----------------------------------------------------------------------------
int vtkGroupDataSetsFilter::FillInputPortInformation(int, vtkInformation* info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataObject");
  info->Set(vtkAlgorithm::INPUT_IS_REPEATABLE(), 1);
  info->Set(vtkAlgorithm::INPUT_IS_OPTIONAL(), 1);
  return 1;
}

//------------------------------------------------------------------------------
int vtkGroupDataSetsFilter::RequestInformation(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  vtkInformation* info = outputVector->GetInformationObject(0);
  info->Remove(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT());
  return 1;
}

//----------------------------------------------------------------------------
int vtkGroupDataSetsFilter::RequestDataObject(
  vtkInformation*, vtkInformationVector**, vtkInformationVector* outputVector)
{
  // create output of specified type.
  return vtkDataObjectAlgorithm::SetOutputDataObject(
           this->OutputType, outputVector->GetInformationObject(0), /*exact = */ true)
    ? 1
    : 0;
}

//----------------------------------------------------------------------------
int vtkGroupDataSetsFilter::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  const auto& internals = (*this->Internals);
  std::vector<std::pair<std::string, vtkSmartPointer<vtkDataObject>>> inputs;

  const int numInputs = inputVector[0]->GetNumberOfInformationObjects();
  const int precision = numInputs > 0 ? static_cast<int>(std::log10(numInputs) + 1) : 1;
  for (int cc = 0; cc < numInputs; ++cc)
  {
    auto dObject = vtkDataObject::GetData(inputVector[0], cc);
    auto name = internals.GetName(cc, this->OutputType, precision);
    inputs.emplace_back(name, dObject);
  }

  if (this->OutputType == VTK_PARTITIONED_DATA_SET)
  {
    unsigned int next = 0;
    auto output = vtkPartitionedDataSet::GetData(outputVector, 0);
    for (auto& input : inputs)
    {
      if (this->CheckAbort())
      {
        break;
      }
      const auto datasets = vtkCompositeDataSet::GetDataSets<vtkDataObject>(input.second);
      for (auto& ds : datasets)
      {
        output->SetPartition(next++, ds);
      }
    }
  }
  else if (this->OutputType == VTK_MULTIBLOCK_DATA_SET)
  {
    unsigned int next = 0;
    auto output = vtkMultiBlockDataSet::GetData(outputVector, 0);
    for (auto& input : inputs)
    {
      if (this->CheckAbort())
      {
        break;
      }
      const auto idx = next++;
      vtkSmartPointer<vtkDataObject> inputDO;
      auto inputMB = vtkMultiBlockDataSet::SafeDownCast(input.second);
      if (vtkPartitionedDataSetCollection::SafeDownCast(input.second))
      {
        vtkNew<vtkConvertToMultiBlockDataSet> converter;
        converter->SetInputDataObject(input.second);
        converter->Update();
        inputDO = converter->GetOutput();
      }
      else if (auto inputPD = vtkPartitionedDataSet::SafeDownCast(input.second))
      {
        vtkNew<vtkMultiPieceDataSet> data;
        data->ShallowCopy(inputPD);
        inputDO = data;
      }
      else if (inputMB && this->CombineFirstLayerMultiblock)
      {
        for (unsigned int i = 0; i < inputMB->GetNumberOfBlocks(); i++)
        {
          output->SetBlock(idx + i, inputMB->GetBlock(i));
          const char* blockName = inputMB->GetMetaData(i)->Get(vtkCompositeDataSet::NAME());
          std::string safeName = blockName ? std::string(blockName) : "Block" + std::to_string(i);
          output->GetMetaData(idx + i)->Set(
            vtkCompositeDataSet::NAME(), input.first + "_" + safeName);
        }
        next += inputMB->GetNumberOfBlocks() - 1;
        continue;
      }
      else
      {
        inputDO = input.second;
      }
      output->SetBlock(idx, inputDO);
      output->GetMetaData(idx)->Set(vtkCompositeDataSet::NAME(), input.first.c_str());
    }
  }
  else if (this->OutputType == VTK_PARTITIONED_DATA_SET_COLLECTION)
  {
    unsigned int next = 0;
    auto output = vtkPartitionedDataSetCollection::GetData(outputVector, 0);
    for (auto& input : inputs)
    {
      if (this->CheckAbort())
      {
        break;
      }
      if (input.second->IsA("vtkPartitionedDataSetCollection") ||
        input.second->IsA("vtkMultiBlockDataSet") || input.second->IsA("vtkUniformGridAMR"))
      {
        vtkNew<vtkConvertToPartitionedDataSetCollection> converter;
        converter->SetInputDataObject(input.second);
        converter->Update();
        auto tempPDC = converter->GetOutput();

        auto numPartitionDataSets = tempPDC->GetNumberOfPartitionedDataSets();
        output->SetNumberOfPartitionedDataSets(next + numPartitionDataSets);
        for (unsigned int i = 0; i < numPartitionDataSets; ++i)
        {
          const auto idx = next++;
          for (unsigned int j = 0; j < tempPDC->GetNumberOfPartitions(i); ++j)
          {
            output->SetPartition(idx, j, tempPDC->GetPartition(i, j));
          }
          std::string partitionName;
          if (tempPDC->GetMetaData(i)->Has(vtkCompositeDataSet::NAME()) &&
            tempPDC->GetMetaData(i)->Get(vtkCompositeDataSet::NAME()))
          {
            partitionName =
              input.first + "_" + tempPDC->GetMetaData(i)->Get(vtkCompositeDataSet::NAME());
          }
          else
          {
            partitionName = input.first + "_" + std::to_string(i);
          }
          output->GetMetaData(idx)->Set(vtkCompositeDataSet::NAME(), partitionName.c_str());
        }
      }
      else
      {
        const auto idx = next++;
        output->SetNumberOfPartitionedDataSets(idx + 1);
        output->GetMetaData(idx)->Set(vtkCompositeDataSet::NAME(), input.first.c_str());
        if (auto pd = vtkPartitionedDataSet::SafeDownCast(input.second))
        {
          unsigned int piece = 0;
          const auto datasets = vtkCompositeDataSet::GetDataSets<vtkDataObject>(pd);
          for (auto& ds : datasets)
          {
            output->SetPartition(idx, piece++, ds);
          }
        }
        else
        {
          output->SetPartition(idx, 0, input.second);
        }
      }
    }
  }
  else
  {
    vtkErrorMacro("Unsupported output type: " << this->OutputType);
    return 0;
  }
  return 1;
}

//----------------------------------------------------------------------------
void vtkGroupDataSetsFilter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
VTK_ABI_NAMESPACE_END
