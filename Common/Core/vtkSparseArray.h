// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-FileCopyrightText: Copyright 2008 Sandia Corporation
// SPDX-License-Identifier: LicenseRef-BSD-3-Clause-Sandia-USGov

/**
 * @class   vtkSparseArray
 * @brief   Sparse, independent coordinate storage for N-way arrays.
 *
 *
 * vtkSparseArray is a concrete vtkArray implementation that stores values using
 * sparse independent coordinate storage.  This means that the array stores the
 * complete set of coordinates and the value for each non-null value in the array.
 * While this approach requires slightly more storage than other sparse storage
 * schemes (such as Compressed-Row or Compressed-Column), it is easier and more
 * efficient to work with when implementing algorithms, and it generalizes well
 * for arbitrary numbers of dimensions.
 *
 * In addition to the value retrieval and update methods provided by vtkTypedArray,
 * vtkSparseArray provides methods to:
 *
 * Get and set a special 'null' value that will be returned when retrieving values
 * for undefined coordinates.
 *
 * Clear the contents of the array so that every set of coordinates is undefined.
 *
 * Sort the array contents so that value coordinates can be visited in a specific order.
 *
 * Retrieve pointers to the value- and coordinate-storage memory blocks.
 *
 * Reserve storage for a specific number of non-null values, for efficiency when the
 * number of non-null values is known in advance.
 *
 * Recompute the array extents so that they bound the largest set of non-nullptr values
 * along each dimension.
 *
 * Specify arbitrary array extents.
 *
 * Add values to the array in amortized-constant time.
 *
 * Validate that the array does not contain duplicate coordinates.
 *
 * @sa
 * vtkArray, vtkTypedArray, vtkDenseArray
 *
 * @par Thanks:
 * Developed by Timothy M. Shead (tshead@sandia.gov) at Sandia National Laboratories.
 */

#ifndef vtkSparseArray_h
#define vtkSparseArray_h

#include "vtkArrayCoordinates.h"
#include "vtkArraySort.h"
#include "vtkObjectFactory.h"
#include "vtkTypedArray.h"

VTK_ABI_NAMESPACE_BEGIN
template <typename T>
class vtkSparseArray : public vtkTypedArray<T>
{
public:
  vtkTemplateTypeMacro(vtkSparseArray<T>, vtkTypedArray<T>);
  static vtkSparseArray<T>* New();
  void PrintSelf(ostream& os, vtkIndent indent) override;

  typedef typename vtkArray::CoordinateT CoordinateT;
  typedef typename vtkArray::DimensionT DimensionT;
  typedef typename vtkArray::SizeT SizeT;

  // vtkArray API
  bool IsDense() VTK_FUTURE_CONST override;
  const vtkArrayExtents& GetExtents() VTK_FUTURE_CONST override;
  SizeT GetNonNullSize() VTK_FUTURE_CONST override;
  void GetCoordinatesN(SizeT n, vtkArrayCoordinates& coordinates) VTK_FUTURE_CONST override;
  vtkArray* DeepCopy() override;

  // vtkTypedArray API
  const T& GetValue(CoordinateT i) override;
  const T& GetValue(CoordinateT i, CoordinateT j) override;
  const T& GetValue(CoordinateT i, CoordinateT j, CoordinateT k) override;
  const T& GetValue(const vtkArrayCoordinates& coordinates) override;
  const T& GetValueN(SizeT n) override;
  void SetValue(CoordinateT i, const T& value) override;
  void SetValue(CoordinateT i, CoordinateT j, const T& value) override;
  void SetValue(CoordinateT i, CoordinateT j, CoordinateT k, const T& value) override;
  void SetValue(const vtkArrayCoordinates& coordinates, const T& value) override;
  void SetValueN(SizeT n, const T& value) override;

  // vtkSparseArray API

  /**
   * Set the value that will be returned by GetValue() for nullptr areas of the array.
   */
  void SetNullValue(const T& value);

  /**
   * Returns the value that will be returned by GetValue() for nullptr areas of the array.
   */
  const T& GetNullValue();

  /**
   * Remove all non-null elements from the array, leaving the number of dimensions, the
   * extent of each dimension, and the label for each dimension unchanged.
   */
  void Clear();

  /**
   * Sorts array values so that their coordinates appear in some well-defined order.
   * The supplied vtkArraySort object controls which dimensions are sorted, and in what
   * order, and should contain one-or-more sort dimensions, up to the number of dimensions
   * stored in the array.
   */
  void Sort(const vtkArraySort& sort);

  /**
   * Returns the set of unique coordinates along the given dimension.
   */
  std::vector<CoordinateT> GetUniqueCoordinates(DimensionT dimension);

  /**
   * Return a read-only reference to the underlying coordinate storage.  Coordinates
   * for each dimension are stored contiguously as a one-dimensional array.  The ordering
   * of coordinates within the array depends on the order in which values were added to
   * the array.
   */
  const CoordinateT* GetCoordinateStorage(DimensionT dimension) const;

  /**
   * Return a mutable reference to the underlying coordinate storage.  Coordinates
   * for each dimension are stored contiguously as a one-dimensional array.  The ordering
   * of coordinates within the array depends on the order in which values were added to
   * the array, and any subsequent sorting.  Use at your own risk!
   */
  CoordinateT* GetCoordinateStorage(DimensionT dimension);

  /**
   * Return a read-only reference to the underlying value storage.  Values are stored
   * contiguously, but in arbitrary order.  Use GetCoordinateStorage() if you need to
   * get the corresponding coordinates for a value.
   */
  const T* GetValueStorage() const;

  /**
   * Return a mutable reference to the underlying value storage.  Values are stored
   * contiguously, but in arbitrary order.  Use GetCoordinateStorage() if you need to
   * get the corresponding coordinates for a value.  Use at your own risk!
   */
  T* GetValueStorage();

  /**
   * Reserve storage for a specific number of values.  This is useful for reading external
   * data using GetCoordinateStorage() and GetValueStorage(), when the total
   * number of non-nullptr values in the array can be determined in advance.  Note that after
   * calling ReserveStorage(), all coordinates and values will be undefined, so you must
   * ensure that every set of coordinates and values is overwritten.  It is the caller's
   * responsibility to ensure that duplicate coordinates are not inserted into the array.
   */
  void ReserveStorage(SizeT value_count);

  /**
   * Update the array extents to match its contents, so that the extent along each dimension
   * matches the maximum index value along that dimension.
   */
  void SetExtentsFromContents();
  /**
   * Specify arbitrary array extents, without altering the contents of the array.  Note
   * that the extents must be as-large-or-larger-than the extents of the actual values
   * stored in the array.  The number of dimensions in the supplied extents must match the
   * number of dimensions currently stored in the array.
   */
  void SetExtents(const vtkArrayExtents& extents);

  ///@{
  /**
   * Adds a new non-null element to the array.  Does not test to see if an element with
   * matching coordinates already exists.  Useful for providing fast initialization of the
   * array as long as the caller is prepared to guarantee that no duplicate coordinates are
   * ever used.
   */
  inline void AddValue(CoordinateT i, const T& value);
  inline void AddValue(CoordinateT i, CoordinateT j, const T& value);
  inline void AddValue(CoordinateT i, CoordinateT j, CoordinateT k, const T& value);
  void AddValue(const vtkArrayCoordinates& coordinates, const T& value);
  ///@}

  /**
   * Validate the contents of the array, returning false if there are any problems.
   * Potential problems include duplicate coordinates, which can be introduced into the
   * array either through AddValue() or direct access to coordinates storage; and coordinates
   * out-of-bounds given the current array extents.

   * Note that Validate() is a heavyweight O(N log N) operation that is intended for
   * temporary use during debugging.
   */
  bool Validate();

protected:
  vtkSparseArray();
  ~vtkSparseArray() override;

private:
  vtkSparseArray(const vtkSparseArray&) = delete;
  void operator=(const vtkSparseArray&) = delete;

  void InternalResize(const vtkArrayExtents& extents) override;
  void InternalSetDimensionLabel(DimensionT i, const vtkStdString& label) override;
  vtkStdString InternalGetDimensionLabel(DimensionT i) VTK_FUTURE_CONST override;

  typedef vtkSparseArray<T> ThisT;

  /**
   * Stores the current array extents (size along each dimension)
   */
  vtkArrayExtents Extents;

  /**
   * Stores a label for each array dimension
   */
  std::vector<std::string> DimensionLabels;

  /**
   * Stores the coordinates of each non-null element within the array,
   * using one contiguous array to store the coordinates for each dimension.
   */
  std::vector<std::vector<CoordinateT>> Coordinates;

  /**
   * Stores the value of each non-null element within the array
   */
  std::vector<T> Values;

  ///@{
  /**
   * Stores the value that will be returned when accessing nullptr areas
   * of the array.
   */
  T NullValue;
  ///@}
};

VTK_ABI_NAMESPACE_END
#include "vtkSparseArray.txx"

#endif
// VTK-HeaderTest-Exclude: vtkSparseArray.h
