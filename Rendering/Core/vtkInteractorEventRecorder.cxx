// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkInteractorEventRecorder.h"

#include "vtkActor2D.h"
#include "vtkCallbackCommand.h"
#include "vtkObjectFactory.h"
#include "vtkPolyDataMapper2D.h"
#include "vtkRegularPolygonSource.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRendererCollection.h"
#include "vtkStringArray.h"

#include <algorithm>
#include <cassert>
#include <locale>
#include <sstream>
#include <string>
#include <vtksys/FStream.hxx>
#include <vtksys/SystemTools.hxx>

VTK_ABI_NAMESPACE_BEGIN
vtkStandardNewMacro(vtkInteractorEventRecorder);

float vtkInteractorEventRecorder::StreamVersion = 1.2f;

//------------------------------------------------------------------------------
vtkInteractorEventRecorder::vtkInteractorEventRecorder()
{
  // take over the processing of keypress events from the superclass
  this->KeyPressCallbackCommand->SetCallback(vtkInteractorEventRecorder::ProcessCharEvent);
  this->KeyPressCallbackCommand->SetPassiveObserver(1); // get events first
  // processes delete events
  this->DeleteEventCallbackCommand = vtkCallbackCommand::New();
  this->DeleteEventCallbackCommand->SetClientData(this);
  this->DeleteEventCallbackCommand->SetCallback(vtkInteractorEventRecorder::ProcessDeleteEvent);

  this->EventCallbackCommand->SetCallback(vtkInteractorEventRecorder::ProcessEvents);
  this->EventCallbackCommand->SetPassiveObserver(1); // get events first

  this->FileName = nullptr;

  this->State = vtkInteractorEventRecorder::Start;
  this->InputStream = nullptr;
  this->OutputStream = nullptr;

  this->ReadFromInputString = 0;
  this->InputString = nullptr;

  vtkNew<vtkRegularPolygonSource> disk;
  disk->SetRadius(5);
  vtkNew<vtkPolyDataMapper2D> mapper;
  mapper->SetInputConnection(disk->GetOutputPort());
  this->CursorActor->SetMapper(mapper);
}

//------------------------------------------------------------------------------
vtkInteractorEventRecorder::~vtkInteractorEventRecorder()
{
  this->SetInteractor(nullptr);

  delete[] this->FileName;

  if (this->InputStream)
  {
    this->InputStream->clear();
    delete this->InputStream;
    this->InputStream = nullptr;
  }

  delete this->OutputStream;
  this->OutputStream = nullptr;

  delete[] this->InputString;
  this->InputString = nullptr;
  this->DeleteEventCallbackCommand->Delete();
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::SetEnabled(int enabling)
{
  if (!this->Interactor)
  {
    vtkErrorMacro(<< "The interactor must be set prior to enabling/disabling widget");
    return;
  }

  if (enabling) //----------------------------------------------------------
  {
    vtkDebugMacro(<< "Enabling widget");

    if (this->Enabled) // already enabled, just return
    {
      return;
    }

    this->Enabled = 1;

    // listen to any event
    vtkRenderWindowInteractor* i = this->Interactor;
    i->AddObserver(vtkCommand::AnyEvent, this->EventCallbackCommand, this->Priority);

    // Make sure that the interactor does not exit in response
    // to a StartEvent. The Interactor has code to allow others to handle
    // the event look of they want to
    i->HandleEventLoop = 1;

    this->InvokeEvent(vtkCommand::EnableEvent, nullptr);
  }

  else // disabling-----------------------------------------------------------
  {
    vtkDebugMacro(<< "Disabling widget");

    if (!this->Enabled) // already disabled, just return
    {
      return;
    }

    this->Enabled = 0;

    // don't listen for events any more
    this->Interactor->RemoveObserver(this->EventCallbackCommand);
    this->Interactor->HandleEventLoop = 0;

    this->InvokeEvent(vtkCommand::DisableEvent, nullptr);
  }
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::Record()
{
  if (this->State == vtkInteractorEventRecorder::Start)
  {
    if (!this->OutputStream) // need to open file
    {
      this->OutputStream = new vtksys::ofstream(this->FileName, ios::out);
      if (this->OutputStream->fail())
      {
        vtkErrorMacro(<< "Unable to open file: " << this->FileName);
        delete this->OutputStream;
        this->OutputStream = nullptr;
        return;
      }

      // Use C locale. We don't want the user-defined locale when we write
      // float values.
      (*this->OutputStream).imbue(std::locale::classic());

      *this->OutputStream << "# StreamVersion " << vtkInteractorEventRecorder::StreamVersion
                          << "\n";
    }

    vtkDebugMacro(<< "Recording");
    this->State = vtkInteractorEventRecorder::Recording;
  }
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::Play()
{
  if (this->State == vtkInteractorEventRecorder::Start)
  {
    if (this->ReadFromInputString)
    {
      vtkDebugMacro(<< "Reading from InputString");
      size_t len = 0;
      if (this->InputString != nullptr)
      {
        len = strlen(this->InputString);
      }
      if (len == 0)
      {
        vtkErrorMacro(<< "No input string specified");
        return;
      }
      std::string inputStr(this->InputString, len);
      delete this->InputStream;
      this->InputStream = new std::istringstream(inputStr);
      if (this->InputStream->fail())
      {
        vtkErrorMacro(<< "Unable to read from string");
        delete this->InputStream;
        this->InputStream = nullptr;
        return;
      }
    }
    else
    {
      if (!this->InputStream) // need to open file
      {
        this->InputStream = new vtksys::ifstream(this->FileName, ios::in);
        if (this->InputStream->fail())
        {
          vtkErrorMacro(<< "Unable to open file: " << this->FileName);
          delete this->InputStream;
          this->InputStream = nullptr;
          return;
        }
      }
    }

    if (this->ShowCursor)
    {
      vtkRenderWindow* win = this->Interactor->GetRenderWindow();
      vtkRendererCollection* collec = win->GetRenderers();
      vtkRenderer* ren = collec->GetFirstRenderer();
      ren->AddActor(this->CursorActor);
    }

    vtkDebugMacro(<< "Playing");
    this->State = vtkInteractorEventRecorder::Playing;

    std::string line;
    this->CurrentStreamVersion = 0;
    while (vtksys::SystemTools::GetLineFromStream(*this->InputStream, line))
    {
      this->ReadEvent(line);
    }

    if (this->ShowCursor)
    {
      vtkRenderWindow* win = this->Interactor->GetRenderWindow();
      vtkRendererCollection* collec = win->GetRenderers();
      vtkRenderer* ren = collec->GetFirstRenderer();
      ren->RemoveActor(this->CursorActor);
    }
  }

  this->State = vtkInteractorEventRecorder::Start;
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::Stop()
{
  this->State = vtkInteractorEventRecorder::Start;
  this->Modified();
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::Clear()
{
  this->Stop();

  if (this->InputStream)
  {
    this->InputStream->clear();
    delete this->InputStream;
    this->InputStream = nullptr;
  }

  if (this->OutputStream)
  {
    delete this->OutputStream;
    this->OutputStream = nullptr;
  }

  this->Modified();
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::Rewind()
{
  if (!this->InputStream) // need to already have an open file
  {
    vtkGenericWarningMacro(<< "No input file opened to rewind...");
    return;
  }
  this->InputStream->clear();
  this->InputStream->seekg(0);
}

//------------------------------------------------------------------------------
// This adds the keypress event observer and the delete event observer
void vtkInteractorEventRecorder::SetInteractor(vtkRenderWindowInteractor* i)
{
  if (i == this->Interactor)
  {
    return;
  }

  // if we already have an Interactor then stop observing it
  if (this->Interactor)
  {
    this->SetEnabled(0); // disable the old interactor
    this->Interactor->RemoveObserver(this->KeyPressCallbackCommand);
    this->Interactor->RemoveObserver(this->DeleteEventCallbackCommand);
  }

  this->Interactor = i;

  // add observers for each of the events handled in ProcessEvents
  if (i)
  {
    i->AddObserver(vtkCommand::CharEvent, this->KeyPressCallbackCommand, this->Priority);
    i->AddObserver(vtkCommand::DeleteEvent, this->DeleteEventCallbackCommand, this->Priority);
  }

  this->Modified();
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::ProcessDeleteEvent(
  vtkObject* vtkNotUsed(object), unsigned long event, void* clientData, void* vtkNotUsed(callData))
{
  assert(event == vtkCommand::DeleteEvent);
  (void)event;
  vtkInteractorEventRecorder* self = reinterpret_cast<vtkInteractorEventRecorder*>(clientData);
  // if the interactor is being deleted then remove the event handlers
  self->SetInteractor(nullptr);
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::ProcessCharEvent(
  vtkObject* object, unsigned long event, void* clientData, void* vtkNotUsed(callData))
{
  assert(event == vtkCommand::CharEvent);
  (void)event;
  vtkInteractorEventRecorder* self = reinterpret_cast<vtkInteractorEventRecorder*>(clientData);
  vtkRenderWindowInteractor* rwi = static_cast<vtkRenderWindowInteractor*>(object);
  if (self->KeyPressActivation)
  {
    if (rwi->GetKeyCode() == self->KeyPressActivationValue)
    {
      if (!self->Enabled)
      {
        self->On();
      }
      else
      {
        self->Off();
      }
    } // event not aborted
  }   // if activation enabled
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::ProcessEvents(
  vtkObject* object, unsigned long event, void* clientData, void* callData)
{
  vtkInteractorEventRecorder* self = reinterpret_cast<vtkInteractorEventRecorder*>(clientData);
  vtkRenderWindowInteractor* rwi = static_cast<vtkRenderWindowInteractor*>(object);

  // all events are processed
  if (self->State == vtkInteractorEventRecorder::Recording)
  {
    if (event != vtkCommand::ModifiedEvent)
    {
      char* cKeySym = rwi->GetKeySym();
      std::string keySym = cKeySym != nullptr ? cKeySym : "";
      std::transform(keySym.begin(), keySym.end(), keySym.begin(), ::toupper);
      // A 'e' or a 'q' will stop the recording
      if (keySym == "E" || keySym == "Q")
      {
        self->Off();
      }
      else
      {
        int m = 0;
        if (rwi->GetShiftKey())
        {
          m |= ModifierKey::ShiftKey;
        }
        if (rwi->GetControlKey())
        {
          m |= ModifierKey::ControlKey;
        }
        if (rwi->GetAltKey())
        {
          m |= ModifierKey::AltKey;
        }
        self->WriteEvent(vtkCommand::GetStringFromEventId(event), rwi->GetEventPosition(), m,
          rwi->GetKeyCode(), rwi->GetRepeatCount(), cKeySym, callData);
      }
    }
    self->OutputStream->flush();
  }
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::WriteEvent(const char* event, int pos[2], int modifiers,
  int keyCode, int repeatCount, char* keySym, void* callData)
{
  *this->OutputStream << event << " " << pos[0] << " " << pos[1] << " " << modifiers << " "
                      << keyCode << " " << repeatCount << " ";
  if (keySym)
  {
    *this->OutputStream << keySym << " ";
  }
  else
  {
    *this->OutputStream << "0 ";
  }

  unsigned int eventId = vtkCommand::GetEventIdFromString(event);
  if (eventId == vtkCommand::DropFilesEvent)
  {
    *this->OutputStream << static_cast<int>(vtkEventDataType::StringArray) << " ";
    // This should go into its own method once more events are supported
    vtkStringArray* filesArr = static_cast<vtkStringArray*>(callData);

    // Recover the number of string, with a sanity check
    vtkIdType dataNum = filesArr ? filesArr->GetNumberOfValues() : 0;
    *this->OutputStream << dataNum << " ";
    if (dataNum > 0)
    {
      for (vtkIdType i = 0; i < dataNum; i++)
      {
        *this->OutputStream << filesArr->GetValue(i) << " ";
      }
    }
    *this->OutputStream << "\n";
  }
  else
  {
    *this->OutputStream << static_cast<int>(vtkEventDataType::None) << "\n";
  }
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::ReadEvent(const std::string& line)
{
  // Read events and invoke them on the object in question
  char event[256] = {}, keySym[256] = {};
  int pos[2], ctrlKey, shiftKey, altKey, keyCode, repeatCount;
  float tempf;

  std::istringstream iss(line);

  // Use classic locale, we don't want to parse float values with
  // user-defined locale.
  iss.imbue(std::locale::classic());

  iss.width(256);
  iss >> event;

  // Quick skip comment
  if (*event == '#')
  {
    // Parse the StreamVersion (not using >> since comment could be empty)
    // Expecting: # StreamVersion x.y

    if (line.size() > 16 && !strncmp(line.c_str(), "# StreamVersion ", 16))
    {
      int res = sscanf(line.c_str() + 16, "%f", &tempf);
      if (res && res != EOF)
      {
        this->CurrentStreamVersion = tempf;
      }
    }
  }
  else
  {
    if (this->CurrentStreamVersion == 0)
    {
      vtkWarningMacro("StreamVersion has not been read, parsing may be incorrect");
    }

    unsigned long ievent = vtkCommand::GetEventIdFromString(event);
    if (ievent != vtkCommand::NoEvent)
    {
      iss >> pos[0];
      iss >> pos[1];
      if (this->CurrentStreamVersion >= 1.1)
      {
        int m;
        iss >> m;
        shiftKey = (m & ModifierKey::ShiftKey) ? 1 : 0;
        ctrlKey = (m & ModifierKey::ControlKey) ? 1 : 0;
        altKey = (m & ModifierKey::AltKey) ? 1 : 0;
      }
      else
      {
        iss >> ctrlKey;
        iss >> shiftKey;
        altKey = 0;
      }
      iss >> keyCode;
      iss >> repeatCount;
      iss >> keySym;

      void* callData = nullptr;
      vtkSmartPointer<vtkStringArray> stringArray;
      if (this->CurrentStreamVersion >= 1.2)
      {
        int tmp;
        iss >> tmp;
        vtkEventDataType dataType = static_cast<vtkEventDataType>(tmp);
        if (dataType == vtkEventDataType::StringArray)
        {
          vtkIdType dataNum;
          iss >> dataNum;
          stringArray = vtkSmartPointer<vtkStringArray>::New();
          for (vtkIdType i = 0; i < dataNum; i++)
          {
            std::string str;
            iss >> str;
            stringArray->InsertNextValue(str);
          }
          callData = stringArray.Get();
        }
      }

      if (this->ShowCursor)
      {
        this->CursorActor->SetPosition(pos[0], pos[1]);
      }

      this->Interactor->SetEventPosition(pos);
      this->Interactor->SetControlKey(ctrlKey);
      this->Interactor->SetShiftKey(shiftKey);
      this->Interactor->SetAltKey(altKey);
      this->Interactor->SetKeyCode(static_cast<char>(keyCode));
      this->Interactor->SetRepeatCount(repeatCount);
      this->Interactor->SetKeySym(keySym);

      this->Interactor->InvokeEvent(ievent, callData);
    }
  }
  assert(iss.good() || iss.eof());
}

//------------------------------------------------------------------------------
void vtkInteractorEventRecorder::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  if (this->FileName)
  {
    os << indent << "File Name: " << this->FileName << "\n";
  }

  os << indent << "ReadFromInputString: " << (this->ReadFromInputString ? "On\n" : "Off\n");

  if (this->InputString)
  {
    os << indent << "Input String: " << this->InputString << "\n";
  }
  else
  {
    os << indent << "Input String: (None)\n";
  }
}
VTK_ABI_NAMESPACE_END
