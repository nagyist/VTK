// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-FileCopyrightText: Copyright (c) Kitware, Inc.
// SPDX-FileCopyrightText: Copyright 2012 Sandia Corporation.
// SPDX-License-Identifier: LicenseRef-BSD-3-Clause-Sandia-USGov
#include "vtkmNDHistogram.h"
#include "vtkmConfigFilters.h"

#include "vtkArrayData.h"
#include "vtkCellData.h"
#include "vtkDataSet.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkSparseArray.h"
#include "vtkTable.h"

#include "vtkmlib/ArrayConverters.h"
#include "vtkmlib/DataSetConverters.h"

#include <viskores/filter/density_estimate/NDHistogram.h>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkmNDHistogram);

//------------------------------------------------------------------------------
void vtkmNDHistogram::PrintSelf(std::ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "FieldNames: "
     << "\n";
  for (const auto& fieldName : FieldNames)
  {
    os << indent << fieldName << " ";
  }
  os << indent << "\n";
  os << indent << "NumberOfBins: "
     << "\n";
  for (const auto& nob : NumberOfBins)
  {
    os << indent << nob << " ";
  }
  os << indent << "\n";
  os << indent << "BinDeltas: "
     << "\n";
  for (const auto& bd : BinDeltas)
  {
    os << indent << bd << " ";
  }
  os << indent << "\n";
  os << indent << "DataRanges: "
     << "\n";
  for (const auto& dr : DataRanges)
  {
    os << indent << dr.first << " " << dr.second << " ";
  }
  os << indent << "\n";
}

//------------------------------------------------------------------------------
vtkmNDHistogram::vtkmNDHistogram() = default;

//------------------------------------------------------------------------------
vtkmNDHistogram::~vtkmNDHistogram() = default;

//------------------------------------------------------------------------------
void vtkmNDHistogram::AddFieldAndBin(const std::string& fieldName, const vtkIdType& numberOfBins)
{
  this->FieldNames.push_back(fieldName);
  this->NumberOfBins.push_back(numberOfBins);
  this->SetInputArrayToProcess(static_cast<int>(this->FieldNames.size()), 0, 0,
    vtkDataObject::FIELD_ASSOCIATION_POINTS, fieldName.c_str());
}

//------------------------------------------------------------------------------
double vtkmNDHistogram::GetBinDelta(size_t fieldIndex)
{
  return this->BinDeltas[fieldIndex];
}

//------------------------------------------------------------------------------
std::pair<double, double> vtkmNDHistogram::GetDataRange(size_t fieldIndex)
{
  return this->DataRanges[fieldIndex];
}

//------------------------------------------------------------------------------
int vtkmNDHistogram::FillInputPortInformation(int port, vtkInformation* info)
{
  this->Superclass::FillInputPortInformation(port, info);

  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataObject");
  return 1;
}

//------------------------------------------------------------------------------
int vtkmNDHistogram::GetFieldIndexFromFieldName(const std::string& fieldName)
{
  auto iter = std::find(this->FieldNames.begin(), this->FieldNames.end(), fieldName);
  return (iter == std::end(this->FieldNames)) ? -1
                                              : static_cast<int>(iter - this->FieldNames.begin());
}

//------------------------------------------------------------------------------
int vtkmNDHistogram::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  vtkDataSet* input = vtkDataSet::SafeDownCast(inInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkArrayData* output = vtkArrayData::GetData(outputVector, 0);
  output->ClearArrays();

  try
  {
    viskores::cont::DataSet in = tovtkm::Convert(input, tovtkm::FieldsFlag::PointsAndCells);

    viskores::filter::density_estimate::NDHistogram filter;
    for (size_t i = 0; i < this->FieldNames.size(); i++)
    {
      filter.AddFieldAndBin(this->FieldNames[i], this->NumberOfBins[i]);
    }
    viskores::cont::DataSet out = filter.Execute(in);

    viskores::Id numberOfFields = out.GetNumberOfFields();
    this->BinDeltas.clear();
    this->DataRanges.clear();
    this->BinDeltas.reserve(static_cast<size_t>(numberOfFields));
    this->DataRanges.reserve(static_cast<size_t>(numberOfFields));

    // Fetch the field array out of the viskores filter result
    size_t index = 0;
    std::vector<vtkDataArray*> fArrays;
    for (auto& fn : this->FieldNames)
    {
      vtkDataArray* fnArray = fromvtkm::Convert(out.GetField(fn));
      fnArray->SetName(fn.c_str());
      fArrays.push_back(fnArray);
      this->BinDeltas.push_back(filter.GetBinDelta(index));
      this->DataRanges.emplace_back(filter.GetDataRange(index).Min, filter.GetDataRange(index).Max);
      index++;
    }
    vtkDataArray* frequencyArray = fromvtkm::Convert(out.GetField("Frequency"));
    frequencyArray->SetName("Frequency");

    // Create the sparse array
    vtkSparseArray<double>* sparseArray = vtkSparseArray<double>::New();
    vtkArrayExtents sae; // sparse array extent
    size_t ndims(fArrays.size());
    sae.SetDimensions(static_cast<vtkArrayExtents::DimensionT>(ndims));
    for (size_t i = 0; i < ndims; i++)
    {
      sae[static_cast<vtkArrayExtents::DimensionT>(i)] =
        vtkArrayRange(0, fArrays[i]->GetNumberOfValues());
    }
    sparseArray->Resize(sae);

    // Set the dimension label
    for (size_t i = 0; i < ndims; i++)
    {
      sparseArray->SetDimensionLabel(static_cast<vtkIdType>(i), fArrays[i]->GetName());
    }
    // Fill in the sparse array
    for (vtkIdType i = 0; i < frequencyArray->GetNumberOfValues(); i++)
    {
      vtkArrayCoordinates coords;
      coords.SetDimensions(static_cast<vtkArrayCoordinates::DimensionT>(ndims));
      for (size_t j = 0; j < ndims; j++)
      {
        coords[static_cast<vtkArrayCoordinates::DimensionT>(j)] = fArrays[j]->GetComponent(i, 0);
      }
      sparseArray->SetValue(coords, frequencyArray->GetComponent(i, 0));
    }
    output->AddArray(sparseArray);

    // Clean up the memory
    for (auto& fArray : fArrays)
    {
      fArray->FastDelete();
    }
    frequencyArray->FastDelete();
    sparseArray->FastDelete();
  }
  catch (const viskores::cont::Error& e)
  {
    vtkErrorMacro(<< "Viskores error: " << e.GetMessage());
    return 0;
  }
  return 1;
}
VTK_ABI_NAMESPACE_END
