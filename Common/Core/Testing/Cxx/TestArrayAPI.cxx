// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-FileCopyrightText: Copyright 2008 Sandia Corporation
// SPDX-License-Identifier: LicenseRef-BSD-3-Clause-Sandia-USGov

#include <vtkDenseArray.h>
#include <vtkSmartPointer.h>
#include <vtkSparseArray.h>

#include <iostream>
#include <sstream>
#include <stdexcept>

#define test_expression(expression)                                                                \
  do                                                                                               \
  {                                                                                                \
    if (!(expression))                                                                             \
    {                                                                                              \
      std::ostringstream buffer;                                                                   \
      buffer << "Expression failed at line " << __LINE__ << ": " << #expression;                   \
      throw std::runtime_error(buffer.str());                                                      \
    }                                                                                              \
  } while (false)

int TestArrayAPI(int vtkNotUsed(argc), char* vtkNotUsed(argv)[])
{
  try
  {
    vtkSmartPointer<vtkArray> array;

    // Test to see that we can create every supported combination of storage- and value-type.
    std::vector<int> storage_types;
    storage_types.push_back(vtkArray::DENSE);
    storage_types.push_back(vtkArray::SPARSE);

    std::vector<int> value_types;
    value_types.push_back(VTK_CHAR);
    value_types.push_back(VTK_UNSIGNED_CHAR);
    value_types.push_back(VTK_SHORT);
    value_types.push_back(VTK_UNSIGNED_SHORT);
    value_types.push_back(VTK_INT);
    value_types.push_back(VTK_UNSIGNED_INT);
    value_types.push_back(VTK_LONG);
    value_types.push_back(VTK_UNSIGNED_LONG);
    value_types.push_back(VTK_DOUBLE);
    value_types.push_back(VTK_ID_TYPE);
    value_types.push_back(VTK_STRING);
    value_types.push_back(VTK_VARIANT);

    std::vector<vtkVariant> sample_values;
    sample_values.emplace_back(static_cast<char>(1));
    sample_values.emplace_back(static_cast<unsigned char>(2));
    sample_values.emplace_back(static_cast<short>(3));
    sample_values.emplace_back(static_cast<unsigned short>(4));
    sample_values.emplace_back(5);
    sample_values.emplace_back(static_cast<unsigned int>(6));
    sample_values.emplace_back(static_cast<long>(7));
    sample_values.emplace_back(static_cast<unsigned long>(8));
    sample_values.emplace_back(9.0);
    sample_values.emplace_back(static_cast<vtkIdType>(10));
    sample_values.emplace_back(vtkStdString("11"));
    sample_values.emplace_back(12.0f);

    for (std::vector<int>::const_iterator storage_type = storage_types.begin();
         storage_type != storage_types.end(); ++storage_type)
    {
      for (size_t value_type = 0; value_type != value_types.size(); ++value_type)
      {
        cerr << "creating array with storage type " << *storage_type << " and value type "
             << vtkImageScalarTypeNameMacro(value_types[value_type]) << endl;

        array.TakeReference(vtkArray::CreateArray(*storage_type, value_types[value_type]));
        test_expression(array);

        test_expression(array->GetName().empty());
        array->SetName("foo");
        test_expression(array->GetName() == "foo");

        array->Resize(10);
        array->SetVariantValue(5, sample_values[value_type]);
        test_expression(array->GetVariantValue(5).IsValid());
        test_expression(array->GetVariantValue(5) == sample_values[value_type]);
      }
    }

    // Do some spot-checking to see that the actual type matches what we expect ...
    array.TakeReference(vtkArray::CreateArray(vtkArray::DENSE, VTK_DOUBLE));
    test_expression(vtkDenseArray<double>::SafeDownCast(array));

    array.TakeReference(vtkArray::CreateArray(vtkArray::SPARSE, VTK_STRING));
    test_expression(vtkSparseArray<vtkStdString>::SafeDownCast(array));

    return 0;
  }
  catch (std::exception& e)
  {
    cerr << e.what() << endl;
    return 1;
  }
}
