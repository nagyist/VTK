// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkActor.h"
#include "vtkConeSource.h"
#include "vtkLight.h"
#include "vtkNew.h"
#include "vtkPolyDataMapper.h"
#include "vtkRenderWindow.h"
#include "vtkRenderer.h"
#include "vtkTestUtilities.h"
#include "vtkWindowNode.h"
#include "vtksys/SystemTools.hxx"

#include "vtkArchiver.h"
#include "vtkJSONRenderWindowExporter.h"

#include "vtkPartitionedArchiver.h"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <cstring>
#include <fstream>

// Construct a scene and write it to disk and to buffer. Decompress the buffer
// and compare its contents to the files on disk.
int TestPartitionedRenderWindowExporter(int argc, char* argv[])
{
  char* tempDir =
    vtkTestUtilities::GetArgOrEnvOrDefault("-T", argc, argv, "VTK_TEMP_DIR", "Testing/Temporary");
  if (!tempDir)
  {
    std::cout << "Could not determine temporary directory.\n";
    return EXIT_FAILURE;
  }
  std::string testDirectory = tempDir;
  delete[] tempDir;

  std::string directoryName = testDirectory + std::string("/") + std::string("ExportVtkJS");

  vtkNew<vtkConeSource> cone;
  vtkNew<vtkPolyDataMapper> pmap;
  pmap->SetInputConnection(cone->GetOutputPort());

  vtkNew<vtkRenderWindow> rwin;

  vtkNew<vtkRenderer> ren;
  rwin->AddRenderer(ren);

  vtkNew<vtkLight> light;
  ren->AddLight(light);

  vtkNew<vtkActor> actor;
  ren->AddActor(actor);

  actor->SetMapper(pmap);

  {
    vtkNew<vtkJSONRenderWindowExporter> exporter;
    exporter->GetArchiver()->SetArchiveName(directoryName.c_str());
    exporter->SetRenderWindow(rwin);
    exporter->Write();
  }

  vtkNew<vtkJSONRenderWindowExporter> exporter;
  vtkNew<vtkPartitionedArchiver> partitionedArchiver;
  exporter->SetArchiver(partitionedArchiver);
  exporter->SetRenderWindow(rwin);
  exporter->Write();

  for (std::size_t i = 0; i < partitionedArchiver->GetNumberOfBuffers(); i++)
  {
    std::string bufferName(partitionedArchiver->GetBufferName(i));

    struct archive* a = archive_read_new();
    archive_read_support_filter_gzip(a);
    archive_read_support_format_zip(a);
#if ARCHIVE_VERSION_NUMBER < 3002000
    int r = archive_read_open_memory(a,
      const_cast<char*>(partitionedArchiver->GetBuffer(bufferName.c_str())),
      partitionedArchiver->GetBufferSize(bufferName.c_str()));
#else
    int r = archive_read_open_memory(a, partitionedArchiver->GetBuffer(bufferName.c_str()),
      partitionedArchiver->GetBufferSize(bufferName.c_str()));
#endif

    if (r != ARCHIVE_OK)
    {
      vtkErrorWithObjectMacro(nullptr, "Cannot open archive from memory");
      return EXIT_FAILURE;
    }

    struct archive_entry* entry;
    char* buffer;
    std::size_t size;
    if (archive_read_next_header(a, &entry) != ARCHIVE_OK || r != ARCHIVE_OK)
    {
      vtkErrorWithObjectMacro(nullptr, "Cannot access archive header");
      return EXIT_FAILURE;
    }

    {
      std::string fileName = directoryName + "/" + archive_entry_pathname(entry);
      size = archive_entry_size(entry);
      buffer = (char*)malloc(size);
      if (buffer == nullptr)
      {
        vtkErrorWithObjectMacro(nullptr, "Could not allocate buffer");
        r = ARCHIVE_FATAL;
        break;
      }

      archive_read_data(a, buffer, size);

      {
        std::FILE* fp;
        char* fbuffer;

        fp = vtksys::SystemTools::Fopen(fileName, "rb");
        if (fp == nullptr)
        {
          vtkErrorWithObjectMacro(nullptr, "Could not open file on disk");
          r = ARCHIVE_FATAL;
          break;
        }

        std::fseek(fp, 0L, SEEK_END);
        long lSize = std::ftell(fp);
        if (size != static_cast<std::size_t>(lSize))
        {
          vtkErrorWithObjectMacro(nullptr, "Buffered file size does not match file size on disk");
          r = ARCHIVE_FATAL;
          std::fclose(fp);
          break;
        }

        clearerr(fp);           // clear error and EOF flags
        fseek(fp, 0, SEEK_SET); // move to beginning

        // allocate memory for entire content
        fbuffer = (char*)malloc(lSize);
        if (fbuffer == nullptr)
        {
          r = ARCHIVE_FATAL;
          std::fclose(fp);
          std::free(buffer);
          break;
        }

        // copy the file into the buffer
        std::size_t r_ = std::fread(fbuffer, lSize, 1, fp);
        (void)r_;

        if (std::memcmp(fbuffer, buffer, size) != 0)
        {
          vtkErrorWithObjectMacro(nullptr, "Buffered file does not match file on disk");
          r = ARCHIVE_FATAL;
        }

        std::fclose(fp);
        std::free(buffer);
        std::free(fbuffer);
      }
    }

    if (r != ARCHIVE_OK)
    {
      vtkErrorWithObjectMacro(nullptr, "Comparison to on-disk archive failed");
      archive_read_free(a);
      return EXIT_FAILURE;
    }

    r = archive_read_free(a);
    if (r != ARCHIVE_OK)
    {
      vtkErrorWithObjectMacro(nullptr, "Cannot close archive");
      return EXIT_FAILURE;
    }
  }

  vtksys::SystemTools::RemoveADirectory(directoryName);

  return EXIT_SUCCESS;
}
