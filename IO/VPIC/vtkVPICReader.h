// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkVPICReader
 * @brief   class for reading VPIC data files
 *
 * vtkDataReader is a helper superclass that reads the vtk data file header,
 * dataset type, and attribute data (point and cell attributes such as
 * scalars, vectors, normals, etc.) from a vtk data file.  See text for
 * the format of the various vtk file types.
 *
 * @sa
 * vtkPolyDataReader vtkStructuredPointsReader vtkStructuredGridReader
 * vtkUnstructuredGridReader vtkRectilinearGridReader
 */

#ifndef vtkVPICReader_h
#define vtkVPICReader_h

#include "vtkIOVPICModule.h" // For export macro
#include "vtkImageAlgorithm.h"

class VPICDataSet;
class GridExchange;

VTK_ABI_NAMESPACE_BEGIN
class vtkCallbackCommand;
class vtkDataArraySelection;
class vtkFloatArray;
class vtkStdString;
class vtkMultiProcessController;
class vtkInformation;

class VTKIOVPIC_EXPORT vtkVPICReader : public vtkImageAlgorithm
{
public:
  static vtkVPICReader* New();
  vtkTypeMacro(vtkVPICReader, vtkImageAlgorithm);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  ///@{
  /**
   * Specify file name of VPIC data file to read.
   */
  vtkSetFilePathMacro(FileName);
  vtkGetFilePathMacro(FileName);
  ///@}

  ///@{
  /**
   * Set the stride in each dimension
   */
  vtkSetVector3Macro(Stride, int);
  vtkGetVector3Macro(Stride, int);
  ///@}

  ///@{
  /**
   * Set the simulation file decomposition in each dimension
   */
  vtkSetVector2Macro(XExtent, int);
  vtkSetVector2Macro(YExtent, int);
  vtkSetVector2Macro(ZExtent, int);
  ///@}

  // Get the full layout size in files for setting the range in GUI
  vtkGetVector2Macro(XLayout, int);
  vtkGetVector2Macro(YLayout, int);
  vtkGetVector2Macro(ZLayout, int);

  ///@{
  /**
   * Get the reader's output
   */
  vtkImageData* GetOutput();
  vtkImageData* GetOutput(int index);
  ///@}

  ///@{
  /**
   * The following methods allow selective reading of solutions fields.
   * By default, ALL data fields on the nodes are read, but this can
   * be modified.
   */
  int GetNumberOfPointArrays();
  const char* GetPointArrayName(int index);
  int GetPointArrayStatus(const char* name);
  void SetPointArrayStatus(const char* name, int status);
  void DisableAllPointArrays();
  void EnableAllPointArrays();
  ///@}

protected:
  vtkVPICReader();
  ~vtkVPICReader() override;

  char* FileName; // First field part file giving path

  int Rank;      // Number of this processor
  int TotalRank; // Number of processors
  int UsedRank;  // Number of processors used in display

  VPICDataSet* vpicData;   // Data structure controlling access
  GridExchange* exchanger; // Exchange ghost cells between procs

  vtkIdType NumberOfNodes;  // Number of points in grid
  vtkIdType NumberOfCells;  // Number of cells in grid
  vtkIdType NumberOfTuples; // Number of tuples in sub extent

  int WholeExtent[6];  // Problem image extent
  int SubExtent[6];    // Processor problem extent
  int Dimension[3];    // Size of image
  int SubDimension[3]; // Size of subextent of image
  int XLayout[2];      // Extent in complete files
  int YLayout[2];      // Extent in complete files
  int ZLayout[2];      // Extent in complete files

  int NumberOfVariables;      // Number of variables to display
  vtkStdString* VariableName; // Names of each variable
  int* VariableStruct;        // Scalar, vector or tensor

  int NumberOfTimeSteps; // Temporal domain
  double* TimeSteps;     // Times available for request
  int CurrentTimeStep;   // Time currently displayed

  int Stride[3];  // Stride over actual data
  int XExtent[2]; // Subview extent in files
  int YExtent[2]; // Subview extent in files
  int ZExtent[2]; // Subview extent in files

  vtkFloatArray** data; // Actual data arrays
  int* dataLoaded;      // Data is loaded for current time

  int Start[3];            // Start offset for processor w ghosts
  int GhostDimension[3];   // Dimension including ghosts on proc
  int NumberOfGhostTuples; // Total ghost cells per component
  int ghostLevel0;         // Left plane number of ghosts
  int ghostLevel1;         // Right plane number of ghosts

  // Controls initializing and querying MPI
  void SetMPIController(vtkMultiProcessController*);
  vtkMultiProcessController* MPIController;

  // Selected field of interest
  vtkDataArraySelection* PointDataArraySelection;

  // Observer to modify this object when array selections are modified
  vtkCallbackCommand* SelectionObserver;

  int RequestData(vtkInformation*, vtkInformationVector**, vtkInformationVector*) override;
  int RequestInformation(
    vtkInformation*, vtkInformationVector** inVector, vtkInformationVector*) override;

  void LoadVariableData(int var, int timeStep);
  void LoadComponent(float* varData, float* block, int comp, int numberOfComponents);

  static void SelectionCallback(
    vtkObject* caller, unsigned long eid, void* clientdata, void* calldata);
  static void EventCallback(vtkObject* caller, unsigned long eid, void* clientdata, void* calldata);

private:
  vtkVPICReader(const vtkVPICReader&) = delete;
  void operator=(const vtkVPICReader&) = delete;
};

VTK_ABI_NAMESPACE_END
#endif
