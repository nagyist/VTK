// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkDebugLeaks.h"

#include "vtkDebug.h"
#include "vtkLogger.h"
#include "vtkObjectFactory.h"
#include "vtkWindows.h"

#include <vtksys/Encoding.hxx>
#include <vtksys/SystemInformation.hxx>
#include <vtksys/SystemTools.hxx>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

VTK_ABI_NAMESPACE_BEGIN
static const char* vtkDebugLeaksIgnoreClasses[] = { nullptr };
std::vector<std::function<void()>>* vtkDebugLeaks::Finalizers = nullptr;

//------------------------------------------------------------------------------
// return 1 if the class should be ignored
static int vtkDebugLeaksIgnoreClassesCheck(const char* s)
{
  int i = 0;
  while (vtkDebugLeaksIgnoreClasses[i])
  {
    if (strcmp(s, vtkDebugLeaksIgnoreClasses[i]) == 0)
    {
      return 1;
    }
    i++;
  }
  return 0;
}

vtkStandardNewMacro(vtkDebugLeaks);

//------------------------------------------------------------------------------
void vtkDebugLeaks::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//------------------------------------------------------------------------------
class vtkDebugLeaksHashTable
{
public:
  vtkDebugLeaksHashTable() = default;
  ~vtkDebugLeaksHashTable() = default;
  void IncrementCount(const char* name);
  vtkTypeBool DecrementCount(const char* name);
  void PrintTable(std::string& os);
  bool IsEmpty();

private:
  std::unordered_map<const char*, unsigned int> CountMap;
};

//------------------------------------------------------------------------------
void vtkDebugLeaksHashTable::IncrementCount(const char* name)
{
  this->CountMap[name]++;
}

//------------------------------------------------------------------------------
bool vtkDebugLeaksHashTable::IsEmpty()
{
  return this->CountMap.empty();
}

//------------------------------------------------------------------------------
vtkTypeBool vtkDebugLeaksHashTable::DecrementCount(const char* name)
{
  if (this->CountMap.count(name) > 0)
  {
    this->CountMap[name]--;
    if (this->CountMap[name] == 0)
    {
      this->CountMap.erase(name);
    }
    return 1;
  }
  else
  {
    return 0;
  }
}

//------------------------------------------------------------------------------
void vtkDebugLeaksHashTable::PrintTable(std::string& os)
{
  auto iter = this->CountMap.begin();
  while (iter != this->CountMap.end())
  {
    if (iter->second > 0 && !vtkDebugLeaksIgnoreClassesCheck(iter->first))
    {
      char tmp[256];
      snprintf(tmp, 256, "\" has %u %s still around.\n", iter->second,
        (iter->second == 1) ? "instance" : "instances");
      os += "Class \"";
      os += iter->first;
      os += tmp;
    }
    ++iter;
  }
}

//------------------------------------------------------------------------------
class vtkDebugLeaksTraceManager
{
public:
  vtkDebugLeaksTraceManager()
  {
    const char* debugLeaksTraceClasses =
      vtksys::SystemTools::GetEnv("VTK_DEBUG_LEAKS_TRACE_CLASSES");
    if (debugLeaksTraceClasses)
    {
      std::vector<std::string> classes;
      vtksys::SystemTools::Split(debugLeaksTraceClasses, classes, ',');
      this->ClassesToTrace.insert(classes.begin(), classes.end());
    }
  }
  ~vtkDebugLeaksTraceManager() = default;

  void RegisterObject(vtkObjectBase* obj);
  void UnRegisterObject(vtkObjectBase* obj);
  void PrintObjects(std::ostream& os);

private:
  std::set<std::string> ClassesToTrace;
  std::map<vtkObjectBase*, std::string> ObjectTraceMap;
};

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaksTraceManager::RegisterObject(vtkObjectBase* obj)
{
  // Get the current stack trace
  if (this->ClassesToTrace.find(obj->GetClassName()) != this->ClassesToTrace.end())
  {
    const int firstFrame = 5; // skip debug leaks frames and start at the call to New()
    const int wholePath = 1;  // produce the whole path to the file if available
    std::string trace = vtksys::SystemInformation::GetProgramStack(firstFrame, wholePath);
    this->ObjectTraceMap[obj] = trace;
  }
}
#else
void vtkDebugLeaksTraceManager::RegisterObject(vtkObjectBase* vtkNotUsed(obj)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaksTraceManager::UnRegisterObject(vtkObjectBase* obj)
{
  this->ObjectTraceMap.erase(obj);
}
#else
void vtkDebugLeaksTraceManager::UnRegisterObject(vtkObjectBase* vtkNotUsed(obj)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaksTraceManager::PrintObjects(std::ostream& os)
{
  // Iterate over any remaining object traces and print them
  auto iter = this->ObjectTraceMap.begin();
  while (iter != this->ObjectTraceMap.end())
  {
    os << "Remaining instance of object '" << iter->first->GetClassName();
    os << "' was allocated at:\n";
    os << iter->second << "\n";
    ++iter;
  }
}
#else
void vtkDebugLeaksTraceManager::PrintObjects(std::ostream& vtkNotUsed(os)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaks::ConstructClass(vtkObjectBase* object)
{
  const std::lock_guard<std::mutex> lock(*vtkDebugLeaks::CriticalSection);
  (void)lock; // To avoid compiler warning on unused variables.
  vtkDebugLeaks::MemoryTable->IncrementCount(object->GetDebugClassName());
  vtkDebugLeaks::TraceManager->RegisterObject(object);
}
#else
void vtkDebugLeaks::ConstructClass(vtkObjectBase* vtkNotUsed(object)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaks::ConstructClass(const char* className)
{
  const std::lock_guard<std::mutex> lock(*vtkDebugLeaks::CriticalSection);
  (void)lock; // To avoid compiler warning on unused variables.
  vtkDebugLeaks::MemoryTable->IncrementCount(className);
}
#else
void vtkDebugLeaks::ConstructClass(const char* vtkNotUsed(className)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaks::DestructClass(vtkObjectBase* object)
{
  bool need_warning = false;
  {
    const std::lock_guard<std::mutex> lock(*vtkDebugLeaks::CriticalSection);
    (void)lock; // To avoid compiler warning on unused variables.

    // Ensure the trace manager has not yet been deleted.
    if (vtkDebugLeaks::TraceManager)
    {
      vtkDebugLeaks::TraceManager->UnRegisterObject(object);
    }

    // Due to globals being deleted, this table may already have
    // been deleted.
    // The warning must be deferred until after the critical section because
    // creating a new instance of `vtkOutputWindow` ends up deadlocking when it
    // calls `vtkDebugLeaks::ConstructClass`.
    need_warning = (vtkDebugLeaks::MemoryTable &&
      !vtkDebugLeaks::MemoryTable->DecrementCount(object->GetDebugClassName()));
  }

  if (need_warning)
  {
    vtkGenericWarningMacro("Deleting unknown object: " << object->GetClassName());
  }
}
#else
void vtkDebugLeaks::DestructClass(vtkObjectBase* vtkNotUsed(object)) {}
#endif

//------------------------------------------------------------------------------
#ifdef VTK_DEBUG_LEAKS
void vtkDebugLeaks::DestructClass(const char* className)
{
  bool need_warning = false;
  {
    const std::lock_guard<std::mutex> lock(*vtkDebugLeaks::CriticalSection);
    (void)lock; // To avoid compiler warning on unused variables.

    // Due to globals being deleted, this table may already have
    // been deleted.
    // The warning must be deferred until after the critical section because
    // creating a new instance of `vtkOutputWindow` ends up deadlocking when it
    // calls `vtkDebugLeaks::ConstructClass`.
    need_warning =
      vtkDebugLeaks::MemoryTable && !vtkDebugLeaks::MemoryTable->DecrementCount(className);
  }

  if (need_warning)
  {
    vtkGenericWarningMacro("Deleting unknown object: " << className);
  }
}
#else
void vtkDebugLeaks::DestructClass(const char* vtkNotUsed(className)) {}
#endif

//------------------------------------------------------------------------------
void vtkDebugLeaks::SetDebugLeaksObserver(vtkDebugLeaksObserver* observer)
{
  vtkDebugLeaks::Observer = observer;
}

//------------------------------------------------------------------------------
vtkDebugLeaksObserver* vtkDebugLeaks::GetDebugLeaksObserver()
{
  return vtkDebugLeaks::Observer;
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::AddFinalizer(std::function<void()> finalizer)
{
  if (!vtkDebugLeaks::Finalizers)
  {
    vtkDebugLeaks::Finalizers = new std::vector<std::function<void()>>();
  }
  vtkDebugLeaks::Finalizers->push_back(finalizer);
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::ConstructingObject(vtkObjectBase* object)
{
  if (vtkDebugLeaks::Observer)
  {
    vtkDebugLeaks::Observer->ConstructingObject(object);
  }
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::DestructingObject(vtkObjectBase* object)
{
  if (vtkDebugLeaks::Observer)
  {
    vtkDebugLeaks::Observer->DestructingObject(object);
  }
}

//------------------------------------------------------------------------------
int vtkDebugLeaks::PrintCurrentLeaks()
{
#ifdef VTK_DEBUG_LEAKS
  if (vtkDebugLeaks::MemoryTable->IsEmpty())
  {
    vtkLogF(TRACE, "vtkDebugLeaks has found no leaks.");
    return 0;
  }

  std::string leaks;
  std::string msg = "vtkDebugLeaks has detected LEAKS!\n";
  vtkDebugLeaks::MemoryTable->PrintTable(leaks);
  cerr << msg;
  cerr << leaks << endl << std::flush;

  vtkDebugLeaks::TraceManager->PrintObjects(std::cerr);

#ifdef _WIN32
  if (getenv("DASHBOARD_TEST_FROM_CTEST") || getenv("DART_TEST_FROM_DART"))
  {
    // Skip dialogs when running on dashboard.
    return 1;
  }
  std::string::size_type myPos = 0;
  int cancel = 0;
  int count = 0;
  while (!cancel && myPos != leaks.npos)
  {
    std::string::size_type newPos = leaks.find('\n', myPos);
    if (newPos != leaks.npos)
    {
      msg += leaks.substr(myPos, newPos - myPos);
      msg += "\n";
      myPos = newPos;
      myPos++;
    }
    else
    {
      myPos = newPos;
    }
    count++;
    if (count == 10)
    {
      count = 0;
      cancel = vtkDebugLeaks::DisplayMessageBox(msg.c_str());
      msg = "";
    }
  }
  if (!cancel && count > 0)
  {
    vtkDebugLeaks::DisplayMessageBox(msg.c_str());
  }
#endif
#endif
  return 1;
}

//------------------------------------------------------------------------------
#ifdef _WIN32
int vtkDebugLeaks::DisplayMessageBox(const char* msg)
{
  std::wstring wmsg = vtksys::Encoding::ToWide(msg);
  return MessageBoxW(nullptr, wmsg.c_str(), L"Error", MB_ICONERROR | MB_OKCANCEL) == IDCANCEL;
}
#else
int vtkDebugLeaks::DisplayMessageBox(const char*)
{
  return 0;
}
#endif

//------------------------------------------------------------------------------
int vtkDebugLeaks::GetExitError()
{
  return vtkDebugLeaks::ExitError;
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::SetExitError(int flag)
{
  vtkDebugLeaks::ExitError = flag;
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::ClassInitialize()
{
#ifdef VTK_DEBUG_LEAKS
  // Create the hash table.
  vtkDebugLeaks::MemoryTable = new vtkDebugLeaksHashTable;

  // Create the trace manager.
  vtkDebugLeaks::TraceManager = new vtkDebugLeaksTraceManager;

  // Create the lock for the critical sections.
  vtkDebugLeaks::CriticalSection = new std::mutex;

  // Default to error when leaks occur while running tests.
  vtkDebugLeaks::ExitError = 1;
  vtkDebugLeaks::Observer = nullptr;
#else
  vtkDebugLeaks::MemoryTable = nullptr;
  vtkDebugLeaks::CriticalSection = nullptr;
  vtkDebugLeaks::ExitError = 0;
  vtkDebugLeaks::Observer = nullptr;
#endif
}

//------------------------------------------------------------------------------
void vtkDebugLeaks::ClassFinalize()
{
#ifdef VTK_DEBUG_LEAKS
  // Allow persistent objects to be cleaned up before debugging leaks.
  if (vtkDebugLeaks::Finalizers)
  {
    for (const auto& finalizer : *vtkDebugLeaks::Finalizers)
    {
      finalizer();
    }
    delete vtkDebugLeaks::Finalizers;
    vtkDebugLeaks::Finalizers = nullptr;
  }

  // Report leaks.
  int leaked = vtkDebugLeaks::PrintCurrentLeaks();

  // Destroy the hash table.
  delete vtkDebugLeaks::MemoryTable;
  vtkDebugLeaks::MemoryTable = nullptr;

  // Destroy the trace manager.
  delete vtkDebugLeaks::TraceManager;
  vtkDebugLeaks::TraceManager = nullptr;

  // Destroy the lock for the critical sections.
  delete vtkDebugLeaks::CriticalSection;
  vtkDebugLeaks::CriticalSection = nullptr;

  // Exit with error if leaks occurred and error mode is on.
  if (leaked && vtkDebugLeaks::ExitError)
  {
    exit(1);
  }
#endif
}

//------------------------------------------------------------------------------

// Purposely not initialized.  ClassInitialize will handle it.
vtkDebugLeaksHashTable* vtkDebugLeaks::MemoryTable;

vtkDebugLeaksTraceManager* vtkDebugLeaks::TraceManager;

// Purposely not initialized.  ClassInitialize will handle it.
std::mutex* vtkDebugLeaks::CriticalSection;

// Purposely not initialized.  ClassInitialize will handle it.
int vtkDebugLeaks::ExitError;

// Purposely not initialized.  ClassInitialize will handle it.
vtkDebugLeaksObserver* vtkDebugLeaks::Observer;
VTK_ABI_NAMESPACE_END
