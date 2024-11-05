// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// --------------------------------------------------------------------------------
// DomainAssembly.cpp
//

// --------------------------------------------------------------------------------


#include "common.h"

// --------------------------------------------------------------------------------
// Headers
// --------------------------------------------------------------------------------

#include "invokeutil.h"
#include "eeconfig.h"
#include "dynamicmethod.h"
#include "field.h"
#include "dbginterface.h"
#include "eventtrace.h"

#include "dllimportcallback.h"
#include "peimagelayout.inl"

#ifndef DACCESS_COMPILE
DomainAssembly::DomainAssembly(PEAssembly* pPEAssembly, LoaderAllocator* pLoaderAllocator, AllocMemTracker* memTracker)
    : m_pAssembly(NULL)
    , m_pPEAssembly(pPEAssembly)
    , m_pModule(NULL)
    , m_fCollectible(pLoaderAllocator->IsCollectible())
    , m_NextDomainAssemblyInSameALC(NULL)
    , m_pLoaderAllocator(pLoaderAllocator)
{
    CONTRACTL
    {
        CONSTRUCTOR_CHECK;
        THROWS;             // ValidateForExecution
        GC_TRIGGERS;        // ValidateForExecution
        MODE_ANY;
    }
    CONTRACTL_END;

    pPEAssembly->AddRef();
    pPEAssembly->ValidateForExecution();

    // Create the Assembly
    NewHolder<Assembly> assembly = Assembly::Create(pPEAssembly, memTracker, pLoaderAllocator);

    m_pAssembly = assembly.Extract();
    m_pModule = m_pAssembly->GetModule();

    m_pAssembly->SetDomainAssembly(this);

    // Creating the Assembly should have ensured the PEAssembly is loaded
    _ASSERT(GetPEAssembly()->IsLoaded());
}

DomainAssembly::~DomainAssembly()
{
    CONTRACTL
    {
        DESTRUCTOR_CHECK;
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    m_pPEAssembly->Release();

    if (m_pAssembly != NULL)
    {
        delete m_pAssembly;
    }
}

// Optimization intended for EnsureLoadLevel only
#include <optsmallperfcritical.h>
void Assembly::EnsureLoadLevel(FileLoadLevel targetLevel)
{
    CONTRACT_VOID
    {
        INSTANCE_CHECK;
        THROWS;
        GC_TRIGGERS;
    }
    CONTRACT_END;

    TRIGGERSGC ();
    if (IsLoading())
    {
        AppDomain::GetCurrentDomain()->LoadAssembly(this, targetLevel);

        // Enforce the loading requirement.  Note that we may have a deadlock in which case we
        // may be off by one which is OK.  (At this point if we are short of targetLevel we know
        // we have done so because of reentrancy constraints.)

        RequireLoadLevel((FileLoadLevel)(targetLevel-1));
    }
    else
        ThrowIfError(targetLevel);

    RETURN;
}
#include <optdefault.h>

CHECK Assembly::CheckLoadLevel(FileLoadLevel requiredLevel, BOOL deadlockOK)
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        NOTHROW;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    if (deadlockOK)
    {
        // CheckLoading requires waiting on a host-breakable lock.
        // Since this is only a checked-build assert and we've been
        // living with it for a while, I'll leave it as is.
        //@TODO: CHECK statements are *NOT* debug-only!!!
        CONTRACT_VIOLATION(ThrowsViolation|GCViolation|TakesLockViolation);
        CHECK(AppDomain::GetCurrentDomain()->CheckLoading(this, requiredLevel));
    }
    else
    {
        CHECK_MSG(m_level >= requiredLevel,
                  "File not sufficiently loaded");
    }

    CHECK_OK;
}



void Assembly::RequireLoadLevel(FileLoadLevel targetLevel)
{
    CONTRACT_VOID
    {
        INSTANCE_CHECK;
        THROWS;
        GC_TRIGGERS;
    }
    CONTRACT_END;

    if (GetLoadLevel() < targetLevel)
    {
        ThrowIfError(targetLevel);
        ThrowHR(MSEE_E_ASSEMBLYLOADINPROGRESS); // @todo: better exception
    }

    RETURN;
}


void Assembly::SetError(Exception *ex)
{
    CONTRACT_VOID
    {
        PRECONDITION(!IsError());
        PRECONDITION(ex != NULL);
        INSTANCE_CHECK;
        THROWS;
        GC_TRIGGERS;
        POSTCONDITION(IsError());
    }
    CONTRACT_END;

    m_pError = ex->DomainBoundClone();

    if (m_pModule)
    {
        m_pModule->NotifyEtwLoadFinished(ex->GetHR());

        if (!IsProfilerNotified())
        {
            SetProfilerNotified();

#ifdef PROFILING_SUPPORTED
            // Only send errors for non-shared assemblies; other assemblies might be successfully completed
            // in another app domain later.
            m_pModule->NotifyProfilerLoadFinished(ex->GetHR());
#endif
        }
    }

    RETURN;
}

void Assembly::ThrowIfError(FileLoadLevel targetLevel)
{
    CONTRACT_VOID
    {
        INSTANCE_CHECK;
        MODE_ANY;
        THROWS;
        GC_TRIGGERS;
    }
    CONTRACT_END;

    if (m_level < targetLevel && m_pError != NULL)
    {
        PAL_CPP_THROW(Exception*, m_pError->DomainBoundClone());
    }

    RETURN;
}

CHECK Assembly::CheckNoError(FileLoadLevel targetLevel)
{
    LIMITED_METHOD_CONTRACT;
    CHECK(m_level >= targetLevel
          || !IsError());

    CHECK_OK;
}

CHECK Assembly::CheckActivated()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END;

    CHECK_MSG(CheckNoError(FILE_ACTIVE), "Assembly load resulted in an error");

    if (IsActive())
        CHECK_OK;

    // CoreLib is allowed to run managed code much earlier than other
    // assemblies for bootstrapping purposes.  This is because it has no
    // dependencies, security checks, and doesn't rely on loader notifications.

    if (GetPEAssembly()->IsSystem())
        CHECK_OK;

    CHECK_MSG(GetPEAssembly()->IsLoaded(), "PEAssembly has not been loaded");
    CHECK_MSG(IsLoaded(), "Assembly has not been fully loaded");
#ifdef _DEBUG
    CHECK_MSG(m_bDisableActivationCheck || CheckLoadLevel(FILE_ACTIVE), "File has not had execution verified");
#endif
    CHECK_OK;
}

#endif //!DACCESS_COMPILE

#ifndef DACCESS_COMPILE

BOOL Assembly::DoIncrementalLoad(FileLoadLevel level)
{
    STANDARD_VM_CONTRACT;

    if (IsError())
        return FALSE;

    switch (level)
    {
    case FILE_LOAD_BEGIN:
        Begin();
        break;

    case FILE_LOAD_BEFORE_TYPE_LOAD:
        BeforeTypeLoad();
        break;

    case FILE_LOAD_EAGER_FIXUPS:
        EagerFixups();
        break;

    case FILE_LOAD_DELIVER_EVENTS:
        DeliverSyncEvents();
        break;

    case FILE_LOAD_VTABLE_FIXUPS:
        VtableFixups();
        break;

    case FILE_LOADED:
        FinishLoad();
        break;

    case FILE_ACTIVE:
        Activate();
        break;

    default:
        UNREACHABLE();
    }

#ifdef FEATURE_MULTICOREJIT
    {
        Module * pModule = m_pModule;

        if (pModule != NULL) // Should not triggle assert when module is NULL
        {
            AppDomain::GetCurrentDomain()->GetMulticoreJitManager().RecordModuleLoad(pModule, level);
        }
    }
#endif

    return TRUE;
}

void Assembly::BeforeTypeLoad()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        // Note that GetPEAssembly()->EnsureLoaded must be called before this OUTSIDE OF THE LOCKS
        PRECONDITION(GetPEAssembly()->IsLoaded());
        STANDARD_VM_CHECK;
    }
    CONTRACTL_END;

#ifdef PROFILING_SUPPORTED
    // After this point, it is possible to load types.
    // We need to notify the profiler now because the profiler may need to inject methods into
    // the module, and to do so reliably, it must have the chance to do so before
    // any types are loaded from the module.
    //
    // In the past we only allowed injecting types/methods on non-NGEN images so notifying here
    // worked ok, but for NGEN images this is pretty ugly. Rejitting often occurs in this callback,
    // but then during fixup the results of LoadedMethodDesc iterator would change and we would
    // need to re-iterate everything. Aside from Rejit other code often wasn't designed to handle
    // running before Fixup. A concrete example VS recently hit, calling GetClassLayout using
    // a MethodTable which doesn't need restore but its parent pointer isn't fixed up yet.
    // We've already set the rules so that profilers can't modify the member list of types in NGEN images
    // so it doesn't matter if types are pre-loaded. We only need the guarantee that code for the
    // loaded types won't execute yet. For NGEN images we deliver the load notification in
    // FILE_LOAD_DELIVER_EVENTS.
    if (!IsProfilerNotified())
    {
        SetProfilerNotified();
        GetModule()->NotifyProfilerLoadFinished(S_OK);
    }

#endif
}

void Assembly::EagerFixups()
{
    WRAPPER_NO_CONTRACT;

#ifdef FEATURE_READYTORUN
    if (GetModule()->IsReadyToRun())
    {
        GetModule()->RunEagerFixups();

    }
#endif // FEATURE_READYTORUN
}

void Assembly::VtableFixups()
{
    WRAPPER_NO_CONTRACT;

    GetModule()->FixupVTables();
}

void Assembly::FinishLoad()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        STANDARD_VM_CHECK;
    }
    CONTRACTL_END;

    // Must set this a bit prematurely for the DAC stuff to work
    m_level = FILE_LOADED;

    // Now the DAC can find this module by enumerating assemblies in a domain.
    DACNotify::DoModuleLoadNotification(m_pModule);
}

void Assembly::Activate()
{
    CONTRACT_VOID
    {
        INSTANCE_CHECK;
        PRECONDITION(IsLoaded());
        STANDARD_VM_CHECK;
    }
    CONTRACT_END;

    // We cannot execute any code in this assembly until we know what exception plan it is on.
    // At the point of an exception's stack-crawl it is too late because we cannot tolerate a GC.
    // See PossiblyUnwrapThrowable and its callers.
    GetModule()->IsRuntimeWrapExceptions();

    //
    // Now call the module constructor.  Note that this might cause reentrancy;
    // this is fine and will be handled by the class cctor mechanism.
    //

    MethodTable *pMT = m_pModule->GetGlobalMethodTable();
    if (pMT != NULL)
    {
        pMT->CheckRestore();
#ifdef _DEBUG
        m_bDisableActivationCheck=TRUE;
#endif
        pMT->CheckRunClassInitThrowing();
    }
#ifdef _DEBUG
    if (g_pConfig->ExpandModulesOnLoad())
    {
        m_pModule->ExpandAll();
    }
#endif //_DEBUG

#ifdef FEATURE_READYTORUN
    if (m_pModule->IsReadyToRun())
    {
        m_pModule->GetReadyToRunInfo()->RegisterUnrelatedR2RModule();
    }
#endif

    RETURN;
}

void Assembly::Begin()
{
    STANDARD_VM_CONTRACT;

    {
        AppDomain::LoadLockHolder lock(AppDomain::GetCurrentDomain());
        AppDomain::GetCurrentDomain()->AddAssembly(GetDomainAssembly());
    }
    // Make it possible to find this DomainAssembly object from associated BINDER_SPACE::Assembly.
    RegisterWithHostAssembly();
}

void Assembly::RegisterWithHostAssembly()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END

    if (GetPEAssembly()->HasHostAssembly())
    {
        GetPEAssembly()->GetHostAssembly()->SetRuntimeAssembly(this);
    }
}

void Assembly::UnregisterFromHostAssembly()
{
    CONTRACTL
    {
        NOTHROW;
        GC_NOTRIGGER;
        MODE_ANY;
    }
    CONTRACTL_END

    if (GetPEAssembly()->HasHostAssembly())
    {
        GetPEAssembly()->GetHostAssembly()->SetRuntimeAssembly(nullptr);
    }
}

void Assembly::DeliverAsyncEvents()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        NOTHROW;
        GC_TRIGGERS;
        MODE_ANY;
    }
    CONTRACTL_END;

    OVERRIDE_LOAD_LEVEL_LIMIT(FILE_ACTIVE);
    AppDomain::GetCurrentDomain()->RaiseLoadingAssemblyEvent(this);
}

void Assembly::DeliverSyncEvents()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        STANDARD_VM_CHECK;
    }
    CONTRACTL_END;

    GetModule()->NotifyEtwLoadFinished(S_OK);

#ifdef PROFILING_SUPPORTED
    if (!IsProfilerNotified())
    {
        SetProfilerNotified();
        GetModule()->NotifyProfilerLoadFinished(S_OK);
    }
#endif

#ifdef DEBUGGING_SUPPORTED
    GCX_COOP();
    if (!IsDebuggerNotified())
    {
        SetShouldNotifyDebugger();

        // Still work to do even if no debugger is attached.
        NotifyDebuggerLoad(ATTACH_ASSEMBLY_LOAD, FALSE);

    }
#endif // DEBUGGING_SUPPORTED
}

DebuggerAssemblyControlFlags Assembly::ComputeDebuggingConfig()
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        THROWS;
        WRAPPER(GC_TRIGGERS);
        MODE_ANY;
        INJECT_FAULT(COMPlusThrowOM(););
    }
    CONTRACTL_END;

#ifdef DEBUGGING_SUPPORTED
    DWORD dacfFlags = DACF_ALLOW_JIT_OPTS;
    IfFailThrow(GetDebuggingCustomAttributes(&dacfFlags));
    return (DebuggerAssemblyControlFlags)dacfFlags;
#else // !DEBUGGING_SUPPORTED
    return 0;
#endif // DEBUGGING_SUPPORTED
}

// For right now, we only check to see if the DebuggableAttribute is present - later may add fields/properties to the
// attributes.
HRESULT Assembly::GetDebuggingCustomAttributes(DWORD *pdwFlags)
{
    CONTRACTL
    {
        INSTANCE_CHECK;
        NOTHROW;
        WRAPPER(GC_TRIGGERS);
        MODE_ANY;
        FORBID_FAULT;
        PRECONDITION(CheckPointer(pdwFlags));
    }
    CONTRACTL_END;

    ULONG size;
    BYTE *blob;
    IMDInternalImport* mdImport = GetPEAssembly()->GetMDImport();
    mdAssembly asTK = TokenFromRid(mdtAssembly, 1);

    HRESULT hr = mdImport->GetCustomAttributeByName(asTK,
                                            DEBUGGABLE_ATTRIBUTE_TYPE,
                                            (const void**)&blob,
                                            &size);

    // If there is no custom value, then there is no entrypoint defined.
    if (FAILED(hr) || hr == S_FALSE)
        return hr;

    // We're expecting a 6 or 8 byte blob:
    //
    // 1, 0, enable tracking, disable opts, 0, 0
    if ((size == 6) || (size == 8))
    {
        if (!((blob[0] == 1) && (blob[1] == 0)))
        {
            BAD_FORMAT_NOTHROW_ASSERT(!"Invalid blob format for custom attribute");
            return COR_E_BADIMAGEFORMAT;
        }

        if (blob[2] & 0x1)
        {
            *pdwFlags |= DACF_OBSOLETE_TRACK_JIT_INFO;
        }
        else
        {
            *pdwFlags &= (~DACF_OBSOLETE_TRACK_JIT_INFO);
        }

        if (blob[2] & 0x2)
        {
            *pdwFlags |= DACF_IGNORE_PDBS;
        }
        else
        {
            *pdwFlags &= (~DACF_IGNORE_PDBS);
        }

        // For compatibility, we enable optimizations if the tracking byte is zero,
        // even if disable opts is nonzero
        if (((blob[2] & 0x1) == 0) || (blob[3] == 0))
        {
            *pdwFlags |= DACF_ALLOW_JIT_OPTS;
        }
        else
        {
            *pdwFlags &= (~DACF_ALLOW_JIT_OPTS);
        }

        LOG((LF_CORDB, LL_INFO10, "Assembly %s: has %s=%d,%d bits = 0x%x\n", GetDebugName(),
                DEBUGGABLE_ATTRIBUTE_TYPE_NAME,
                blob[2], blob[3], *pdwFlags));
    }

    return hr;
}

BOOL Assembly::NotifyDebuggerLoad(int flags, BOOL attaching)
{
    WRAPPER_NO_CONTRACT;

    BOOL result = FALSE;

    // Debugger Attach is done totally out-of-process. Does not call code in-proc.
    _ASSERTE(!attaching);

    // Make sure the debugger has been initialized.  See code:Debugger::Startup.
    if (g_pDebugInterface == NULL)
    {
        _ASSERTE(!CORDebuggerAttached());
        return FALSE;
    }

    // There is still work we need to do even when no debugger is attached.
    if (flags & ATTACH_ASSEMBLY_LOAD)
    {
        if (ShouldNotifyDebugger())
        {
            g_pDebugInterface->LoadAssembly(GetDomainAssembly());
        }
        result = TRUE;
    }

    if(this->ShouldNotifyDebugger())
    {
        result = result ||
            this->GetModule()->NotifyDebuggerLoad(GetDomainAssembly(), flags, attaching);
    }

    if( ShouldNotifyDebugger())
    {
           result|=m_pModule->NotifyDebuggerLoad(GetDomainAssembly(), ATTACH_MODULE_LOAD, attaching);
           SetDebuggerNotified();
    }

    return result;
}

void Assembly::NotifyDebuggerUnload()
{
    LIMITED_METHOD_CONTRACT;

    if (!AppDomain::GetCurrentDomain()->IsDebuggerAttached())
        return;

    // Dispatch module unload for the module. Debugger is resilient in case we haven't dispatched
    // a previous load event (such as if debugger attached after the modules was loaded).
    this->GetModule()->NotifyDebuggerUnload();

    g_pDebugInterface->UnloadAssembly(GetDomainAssembly());
}

#endif // #ifndef DACCESS_COMPILE

#ifdef DACCESS_COMPILE

void DomainAssembly::EnumMemoryRegions(CLRDataEnumMemoryFlags flags)
{
    SUPPORTS_DAC;

    DAC_ENUM_DTHIS();

    // Modules are needed for all minidumps, but they are enumerated elsewhere
    // so we don't need to duplicate effort; thus we do noting with m_pModule.

    // For MiniDumpNormal, we only want the file name.
    if (m_pPEAssembly.IsValid())
    {
        m_pPEAssembly->EnumMemoryRegions(flags);
    }

    if (flags == CLRDATA_ENUM_MEM_HEAP2)
    {
        GetLoaderAllocator()->EnumMemoryRegions(flags);
    }
    else if (flags != CLRDATA_ENUM_MEM_MINI && flags != CLRDATA_ENUM_MEM_TRIAGE)
    {
        if (m_pAssembly.IsValid())
        {
            m_pAssembly->EnumMemoryRegions(flags);
        }
    }
}

#endif // #ifdef DACCESS_COMPILE
