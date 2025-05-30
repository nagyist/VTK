// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
/**
 * @class   vtkAMRBaseParticlesReader
 * @brief   An abstract base class that implements all the common functionality for
 *  all particle readers.
 */

#ifndef vtkAMRBaseParticlesReader_h
#define vtkAMRBaseParticlesReader_h

#include "vtkIOAMRModule.h" // For export macro
#include "vtkMultiBlockDataSetAlgorithm.h"

VTK_ABI_NAMESPACE_BEGIN
class vtkInformation;
class vtkInformationVector;
class vtkIndent;
class vtkMultiProcessController;
class vtkPolyData;
class vtkDataArraySelection;
class vtkCallbackCommand;

class VTKIOAMR_EXPORT vtkAMRBaseParticlesReader : public vtkMultiBlockDataSetAlgorithm
{
public:
  vtkTypeMacro(vtkAMRBaseParticlesReader, vtkMultiBlockDataSetAlgorithm);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  ///@{
  /**
   * Set & Get the frequency.
   */
  vtkGetMacro(Frequency, int);
  vtkSetMacro(Frequency, int);
  ///@}

  ///@{
  /**
   * Set & Get the multi-process controller.
   */
  vtkGetObjectMacro(Controller, vtkMultiProcessController);
  virtual void SetController(vtkMultiProcessController*);
  ///@}

  ///@{
  /**
   * Set & Get for filter location and boolean macro
   */
  vtkSetMacro(FilterLocation, vtkTypeBool);
  vtkGetMacro(FilterLocation, vtkTypeBool);
  vtkBooleanMacro(FilterLocation, vtkTypeBool);
  ///@}

  ///@{
  /**
   * Get the data array selection tables used to configure which data
   * arrays are loaded by the reader.
   */
  vtkGetObjectMacro(ParticleDataArraySelection, vtkDataArraySelection);
  ///@}

  /**
   * Get the number of particles arrays available in the input.
   */
  int GetNumberOfParticleArrays();

  /**
   * Get the particle array name of the array associated with the given
   * index.
   */
  const char* GetParticleArrayName(int index);

  ///@{
  /**
   * Get/Set whether the particle array status.
   */
  int GetParticleArrayStatus(const char* name);
  void SetParticleArrayStatus(const char* name, int status);
  ///@}

  virtual void SetFileName(VTK_FILEPATH const char* fileName);
  vtkGetFilePathMacro(FileName);

  ///@{
  /**
   * Sets the min location
   */
  void SetMinLocation(double minx, double miny, double minz)
  {
    this->MinLocation[0] = minx;
    this->MinLocation[1] = miny;
    this->MinLocation[2] = minz;
  }
  ///@}

  ///@{
  /**
   * Sets the max location
   */
  void SetMaxLocation(double maxx, double maxy, double maxz)
  {
    this->MaxLocation[0] = maxx;
    this->MaxLocation[1] = maxy;
    this->MaxLocation[2] = maxz;
  }
  ///@}

  /**
   * Returns the total number of particles
   */
  virtual int GetTotalNumberOfParticles() = 0;

protected:
  vtkAMRBaseParticlesReader();
  ~vtkAMRBaseParticlesReader() override;

  /**
   * Reads the metadata, e.g., the number of blocks in the file.
   * After the metadata is read, this->Initialized is set to true.
   * Furthermore, to limit I/O, all concrete classes must make sure
   * that this method returns immediately if this->Initialized is true.
   */
  virtual void ReadMetaData() = 0;

  /**
   * Reads the particles corresponding to the block associated with the
   * given supplied block index.
   */
  virtual vtkPolyData* ReadParticles(int blkIdx) = 0;

  /**
   * Filters particles by their location. If FilterLocation is ON, this
   * method returns whether or not the particle with the supplied xyz
   * coordinates class within the bounding box specified by the user using
   * the SetMinLocation & SetMaxLocation.
   */
  bool CheckLocation(double x, double y, double z);

  /**
   * Determines whether this reader instance is running in parallel or not.
   */
  bool IsParallel();

  /**
   * Determines if the block associated with the given block index belongs
   * to the process that executes the current instance of the reader.
   */
  bool IsBlockMine(int blkIdx);

  /**
   * Given the block index, this method determines the process Id.
   * If the reader instance is serial this method always returns 0.
   * Otherwise, static block-cyclic-distribution is assumed and each
   * block is assigned to a process according to blkIdx%N, where N is
   * the total number of processes.
   */
  int GetBlockProcessId(int blkIdx);

  /**
   * Initializes the AMR Particles reader
   * NOTE: must be called in the constructor of concrete classes.
   */
  void Initialize();

  ///@{
  /**
   * Standard Array selection variables & methods
   */
  vtkDataArraySelection* ParticleDataArraySelection;
  vtkCallbackCommand* SelectionObserver;
  ///@}

  /**
   * Initializes the ParticleDataArraySelection object. This method
   * only executes for an initial request in which case all arrays are
   * deselected.
   */
  void InitializeParticleDataSelections();

  /**
   * Sets up the ParticleDataArraySelection. Implemented
   * by concrete classes.
   */
  virtual void SetupParticleDataSelections() = 0;

  /**
   * Call-back registered with the SelectionObserver for selecting/deselecting
   * particles
   */
  static void SelectionModifiedCallback(
    vtkObject* caller, unsigned long eid, void* clientdata, void* calldata);

  ///@{
  /**
   * Standard pipeline operations
   */
  int RequestData(vtkInformation* request, vtkInformationVector** inputVector,
    vtkInformationVector* outputVector) override;
  int FillOutputPortInformation(int port, vtkInformation* info) override;
  ///@}

  int NumberOfBlocks;

  vtkTypeBool FilterLocation;
  double MinLocation[3];
  double MaxLocation[3];

  int Frequency;
  vtkMultiProcessController* Controller;

  bool InitialRequest;
  bool Initialized;
  char* FileName;

private:
  vtkAMRBaseParticlesReader(const vtkAMRBaseParticlesReader&) = delete;
  void operator=(const vtkAMRBaseParticlesReader&) = delete;
};

VTK_ABI_NAMESPACE_END
#endif /* vtkAMRBaseParticlesReader_h */
