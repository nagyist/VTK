// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkTextureMapToSphere.h"

#include "vtkCellData.h"
#include "vtkDataSet.h"
#include "vtkFloatArray.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMath.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkTextureMapToSphere);

// Create object with Center (0,0,0) and the PreventSeam ivar is set to true. The
// sphere center is automatically computed.
vtkTextureMapToSphere::vtkTextureMapToSphere()
{
  this->Center[0] = this->Center[1] = this->Center[2] = 0.0;

  this->AutomaticSphereGeneration = 1;
  this->PreventSeam = 1;
}

int vtkTextureMapToSphere::RequestData(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // get the info objects
  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // get the input and output
  vtkDataSet* input = vtkDataSet::SafeDownCast(inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkDataSet* output = vtkDataSet::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

  if (!input || !output)
  {
    vtkErrorMacro(<< "Invalid input or output");
    return 1;
  }

  vtkFloatArray* newTCoords;
  vtkIdType numPts = input->GetNumberOfPoints();
  vtkIdType ptId;
  double x[3], rho, r, tc[2], phi = 0.0, thetaX, thetaY;
  double diff, PiOverTwo = vtkMath::Pi() / 2.0;

  vtkDebugMacro(<< "Generating Spherical Texture Coordinates");

  // First, copy the input to the output as a starting point
  output->CopyStructure(input);

  if (numPts < 1)
  {
    // Needs to be called in case MPI is in use
    this->ComputeCenter(input);
    return 1;
  }

  this->ComputeCenter(input);

  // loop over all points computing spherical coordinates. Only tricky part
  // is keeping track of singularities/numerical problems.
  newTCoords = vtkFloatArray::New();
  newTCoords->SetName("Texture Coordinates");
  newTCoords->SetNumberOfComponents(2);
  newTCoords->SetNumberOfTuples(numPts);
  for (ptId = 0; ptId < numPts; ptId++)
  {
    input->GetPoint(ptId, x);
    rho = sqrt(vtkMath::Distance2BetweenPoints(x, this->Center));
    if (rho != 0.0)
    {
      // watch for truncation problems
      if (fabs((diff = x[2] - this->Center[2])) > rho)
      {
        phi = 0.0;
        if (diff > 0.0)
        {
          tc[1] = 0.0;
        }
        else
        {
          tc[1] = 1.0;
        }
      }
      else
      {
        phi = acos((diff / rho));
        tc[1] = phi / vtkMath::Pi();
      }
    }
    else
    {
      tc[1] = 0.0;
    }

    r = rho * sin(phi);
    if (r != 0.0)
    {
      // watch for truncation problems
      if (fabs((diff = x[0] - this->Center[0])) > r)
      {
        if (diff > 0.0)
        {
          thetaX = 0.0;
        }
        else
        {
          thetaX = vtkMath::Pi();
        }
      }
      else
      {
        thetaX = acos(diff / r);
      }

      if (fabs((diff = x[1] - this->Center[1])) > r)
      {
        if (diff > 0.0)
        {
          thetaY = PiOverTwo;
        }
        else
        {
          thetaY = -PiOverTwo;
        }
      }
      else
      {
        thetaY = asin(diff / r);
      }
    }
    else
    {
      thetaX = thetaY = 0.0;
    }

    if (this->PreventSeam)
    {
      tc[0] = thetaX / vtkMath::Pi();
    }
    else
    {
      tc[0] = thetaX / (2.0 * vtkMath::Pi());
      if (thetaY < 0.0)
      {
        tc[0] = 1.0 - tc[0];
      }
    }

    newTCoords->SetTuple(ptId, tc);
  }

  output->GetPointData()->CopyTCoordsOff();
  output->GetPointData()->PassData(input->GetPointData());

  output->GetCellData()->PassData(input->GetCellData());

  output->GetPointData()->SetTCoords(newTCoords);
  newTCoords->Delete();

  return 1;
}

void vtkTextureMapToSphere::ComputeCenter(vtkDataSet* dataSet)
{
  if (this->AutomaticSphereGeneration)
  {
    double x[3];
    vtkIdType numPts = dataSet->GetNumberOfPoints();
    this->Center[0] = this->Center[1] = this->Center[2] = 0.0;
    for (vtkIdType ptId = 0; ptId < numPts; ++ptId)
    {
      dataSet->GetPoint(ptId, x);
      this->Center[0] += x[0];
      this->Center[1] += x[1];
      this->Center[2] += x[2];
    }
    this->Center[0] /= numPts;
    this->Center[1] /= numPts;
    this->Center[2] /= numPts;

    vtkDebugMacro(<< "Center computed as: (" << this->Center[0] << ", " << this->Center[1] << ", "
                  << this->Center[2] << ")");
  }
}

void vtkTextureMapToSphere::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent
     << "Automatic Sphere Generation: " << (this->AutomaticSphereGeneration ? "On\n" : "Off\n");
  os << indent << "Prevent Seam: " << (this->PreventSeam ? "On\n" : "Off\n");
  os << indent << "Center: (" << this->Center[0] << ", " << this->Center[1] << ", "
     << this->Center[2] << ")\n";
}
VTK_ABI_NAMESPACE_END
