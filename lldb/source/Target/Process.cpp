//===-- Process.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/Process.h"

#include "lldb/lldb-private-log.h"

#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Core/Event.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/State.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Host/Host.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/DynamicLoader.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/CPPLanguageRuntime.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadPlan.h"

using namespace lldb;
using namespace lldb_private;

Process*
Process::FindPlugin (Target &target, const char *plugin_name, Listener &listener)
{
    ProcessCreateInstance create_callback = NULL;
    if (plugin_name)
    {
        create_callback  = PluginManager::GetProcessCreateCallbackForPluginName (plugin_name);
        if (create_callback)
        {
            std::auto_ptr<Process> debugger_ap(create_callback(target, listener));
            if (debugger_ap->CanDebug(target))
                return debugger_ap.release();
        }
    }
    else
    {
        for (uint32_t idx = 0; (create_callback = PluginManager::GetProcessCreateCallbackAtIndex(idx)) != NULL; ++idx)
        {
            std::auto_ptr<Process> debugger_ap(create_callback(target, listener));
            if (debugger_ap->CanDebug(target))
                return debugger_ap.release();
        }
    }
    return NULL;
}


//----------------------------------------------------------------------
// Process constructor
//----------------------------------------------------------------------
Process::Process(Target &target, Listener &listener) :
    UserID (LLDB_INVALID_PROCESS_ID),
    Broadcaster ("lldb.process"),
    ProcessInstanceSettings (*(Process::GetSettingsController().get())),
    m_target (target),
    m_public_state (eStateUnloaded),
    m_private_state (eStateUnloaded),
    m_private_state_broadcaster ("lldb.process.internal_state_broadcaster"),
    m_private_state_control_broadcaster ("lldb.process.internal_state_control_broadcaster"),
    m_private_state_listener ("lldb.process.internal_state_listener"),
    m_private_state_control_wait(),
    m_private_state_thread (LLDB_INVALID_HOST_THREAD),
    m_stop_id (0),
    m_thread_index_id (0),
    m_exit_status (-1),
    m_exit_string (),
    m_thread_list (this),
    m_notifications (),
    m_persistent_vars(),
    m_listener(listener),
    m_unix_signals ()
{
    UpdateInstanceName();

    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Process::Process()", this);

    SetEventName (eBroadcastBitStateChanged, "state-changed");
    SetEventName (eBroadcastBitInterrupt, "interrupt");
    SetEventName (eBroadcastBitSTDOUT, "stdout-available");
    SetEventName (eBroadcastBitSTDERR, "stderr-available");
    
    listener.StartListeningForEvents (this,
                                      eBroadcastBitStateChanged |
                                      eBroadcastBitInterrupt |
                                      eBroadcastBitSTDOUT |
                                      eBroadcastBitSTDERR);

    m_private_state_listener.StartListeningForEvents(&m_private_state_broadcaster,
                                                     eBroadcastBitStateChanged);

    m_private_state_listener.StartListeningForEvents(&m_private_state_control_broadcaster,
                                                     eBroadcastInternalStateControlStop |
                                                     eBroadcastInternalStateControlPause |
                                                     eBroadcastInternalStateControlResume);
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
Process::~Process()
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Process::~Process()", this);
    StopPrivateStateThread();
}

void
Process::Finalize()
{
    // Do any cleanup needed prior to being destructed... Subclasses
    // that override this method should call this superclass method as well.
}

void
Process::RegisterNotificationCallbacks (const Notifications& callbacks)
{
    m_notifications.push_back(callbacks);
    if (callbacks.initialize != NULL)
        callbacks.initialize (callbacks.baton, this);
}

bool
Process::UnregisterNotificationCallbacks(const Notifications& callbacks)
{
    std::vector<Notifications>::iterator pos, end = m_notifications.end();
    for (pos = m_notifications.begin(); pos != end; ++pos)
    {
        if (pos->baton == callbacks.baton &&
            pos->initialize == callbacks.initialize &&
            pos->process_state_changed == callbacks.process_state_changed)
        {
            m_notifications.erase(pos);
            return true;
        }
    }
    return false;
}

void
Process::SynchronouslyNotifyStateChanged (StateType state)
{
    std::vector<Notifications>::iterator notification_pos, notification_end = m_notifications.end();
    for (notification_pos = m_notifications.begin(); notification_pos != notification_end; ++notification_pos)
    {
        if (notification_pos->process_state_changed)
            notification_pos->process_state_changed (notification_pos->baton, this, state);
    }
}

// FIXME: We need to do some work on events before the general Listener sees them.
// For instance if we are continuing from a breakpoint, we need to ensure that we do
// the little "insert real insn, step & stop" trick.  But we can't do that when the
// event is delivered by the broadcaster - since that is done on the thread that is
// waiting for new events, so if we needed more than one event for our handling, we would
// stall.  So instead we do it when we fetch the event off of the queue.
//

StateType
Process::GetNextEvent (EventSP &event_sp)
{
    StateType state = eStateInvalid;

    if (m_listener.GetNextEventForBroadcaster (this, event_sp) && event_sp)
        state = Process::ProcessEventData::GetStateFromEvent (event_sp.get());

    return state;
}


StateType
Process::WaitForProcessToStop (const TimeValue *timeout)
{
    StateType match_states[] = { eStateStopped, eStateCrashed, eStateDetached, eStateExited, eStateUnloaded };
    return WaitForState (timeout, match_states, sizeof(match_states) / sizeof(StateType));
}


StateType
Process::WaitForState
(
    const TimeValue *timeout,
    const StateType *match_states, const uint32_t num_match_states
)
{
    EventSP event_sp;
    uint32_t i;
    StateType state = GetState();
    while (state != eStateInvalid)
    {
        // If we are exited or detached, we won't ever get back to any
        // other valid state...
        if (state == eStateDetached || state == eStateExited)
            return state;

        state = WaitForStateChangedEvents (timeout, event_sp);

        for (i=0; i<num_match_states; ++i)
        {
            if (match_states[i] == state)
                return state;
        }
    }
    return state;
}

bool
Process::HijackProcessEvents (Listener *listener)
{
    if (listener != NULL)
    {
        return HijackBroadcaster(listener, eBroadcastBitStateChanged);
    }
    else
        return false;
}

void
Process::RestoreProcessEvents ()
{
    RestoreBroadcaster();
}

StateType
Process::WaitForStateChangedEvents (const TimeValue *timeout, EventSP &event_sp)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));

    if (log)
        log->Printf ("Process::%s (timeout = %p, event_sp)...", __FUNCTION__, timeout);

    StateType state = eStateInvalid;
    if (m_listener.WaitForEventForBroadcasterWithType (timeout,
                                                       this,
                                                       eBroadcastBitStateChanged,
                                                       event_sp))
        state = Process::ProcessEventData::GetStateFromEvent(event_sp.get());

    log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS);
    if (log)
        log->Printf ("Process::%s (timeout = %p, event_sp) => %s",
                     __FUNCTION__,
                     timeout,
                     StateAsCString(state));
    return state;
}

Event *
Process::PeekAtStateChangedEvents ()
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));

    if (log)
        log->Printf ("Process::%s...", __FUNCTION__);

    Event *event_ptr;
    event_ptr = m_listener.PeekAtNextEventForBroadcasterWithType (this,
                                                                  eBroadcastBitStateChanged);
    log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS);
    if (log)
    {
        if (event_ptr)
        {
            log->Printf ("Process::%s (event_ptr) => %s",
                         __FUNCTION__,
                         StateAsCString(ProcessEventData::GetStateFromEvent (event_ptr)));
        }
        else 
        {
            log->Printf ("Process::%s no events found",
                         __FUNCTION__);
        }
    }
    return event_ptr;
}

StateType
Process::WaitForStateChangedEventsPrivate (const TimeValue *timeout, EventSP &event_sp)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));

    if (log)
        log->Printf ("Process::%s (timeout = %p, event_sp)...", __FUNCTION__, timeout);

    StateType state = eStateInvalid;
    if (m_private_state_listener.WaitForEventForBroadcasterWithType(timeout,
                                                                    &m_private_state_broadcaster,
                                                                    eBroadcastBitStateChanged,
                                                                    event_sp))
        state = Process::ProcessEventData::GetStateFromEvent(event_sp.get());

    // This is a bit of a hack, but when we wait here we could very well return
    // to the command-line, and that could disable the log, which would render the
    // log we got above invalid.
    log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS);
    if (log)
        log->Printf ("Process::%s (timeout = %p, event_sp) => %s", __FUNCTION__, timeout, StateAsCString(state));
    return state;
}

bool
Process::WaitForEventsPrivate (const TimeValue *timeout, EventSP &event_sp, bool control_only)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));

    if (log)
        log->Printf ("Process::%s (timeout = %p, event_sp)...", __FUNCTION__, timeout);

    if (control_only)
        return m_private_state_listener.WaitForEventForBroadcaster(timeout, &m_private_state_control_broadcaster, event_sp);
    else
        return m_private_state_listener.WaitForEvent(timeout, event_sp);
}

bool
Process::IsRunning () const
{
    return StateIsRunningState (m_public_state.GetValue());
}

int
Process::GetExitStatus ()
{
    if (m_public_state.GetValue() == eStateExited)
        return m_exit_status;
    return -1;
}

const char *
Process::GetExitDescription ()
{
    if (m_public_state.GetValue() == eStateExited && !m_exit_string.empty())
        return m_exit_string.c_str();
    return NULL;
}

void
Process::SetExitStatus (int status, const char *cstr)
{
    m_exit_status = status;
    if (cstr)
        m_exit_string = cstr;
    else
        m_exit_string.clear();

    SetPrivateState (eStateExited);
}

// This static callback can be used to watch for local child processes on
// the current host. The the child process exits, the process will be
// found in the global target list (we want to be completely sure that the
// lldb_private::Process doesn't go away before we can deliver the signal.
bool
Process::SetProcessExitStatus
(
    void *callback_baton,
    lldb::pid_t pid,
    int signo,      // Zero for no signal
    int exit_status      // Exit value of process if signal is zero
)
{
    if (signo == 0 || exit_status)
    {
        TargetSP target_sp(Debugger::FindTargetWithProcessID (pid));
        if (target_sp)
        {
            ProcessSP process_sp (target_sp->GetProcessSP());
            if (process_sp)
            {
                const char *signal_cstr = NULL;
                if (signo)
                    signal_cstr = process_sp->GetUnixSignals().GetSignalAsCString (signo);

                process_sp->SetExitStatus (exit_status, signal_cstr);
            }
        }
        return true;
    }
    return false;
}


uint32_t
Process::GetNextThreadIndexID ()
{
    return ++m_thread_index_id;
}

StateType
Process::GetState()
{
    // If any other threads access this we will need a mutex for it
    return m_public_state.GetValue ();
}

void
Process::SetPublicState (StateType new_state)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_STATE));
    if (log)
        log->Printf("Process::SetPublicState (%s)", StateAsCString(new_state));
    m_public_state.SetValue (new_state);
}

StateType
Process::GetPrivateState ()
{
    return m_private_state.GetValue();
}

void
Process::SetPrivateState (StateType new_state)
{
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_STATE));
    bool state_changed = false;

    if (log)
        log->Printf("Process::SetPrivateState (%s)", StateAsCString(new_state));

    Mutex::Locker locker(m_private_state.GetMutex());

    const StateType old_state = m_private_state.GetValueNoLock ();
    state_changed = old_state != new_state;
    if (state_changed)
    {
        m_private_state.SetValueNoLock (new_state);
        if (StateIsStoppedState(new_state))
        {
            m_stop_id++;
            if (log)
                log->Printf("Process::SetPrivateState (%s) stop_id = %u", StateAsCString(new_state), m_stop_id);
        }
        // Use our target to get a shared pointer to ourselves...
        m_private_state_broadcaster.BroadcastEvent (eBroadcastBitStateChanged, new ProcessEventData (GetTarget().GetProcessSP(), new_state));
    }
    else
    {
        if (log)
            log->Printf("Process::SetPrivateState (%s) state didn't change. Ignoring...", StateAsCString(new_state), StateAsCString(old_state));
    }
}


uint32_t
Process::GetStopID() const
{
    return m_stop_id;
}

addr_t
Process::GetImageInfoAddress()
{
    return LLDB_INVALID_ADDRESS;
}

//----------------------------------------------------------------------
// LoadImage
//
// This function provides a default implementation that works for most
// unix variants. Any Process subclasses that need to do shared library
// loading differently should override LoadImage and UnloadImage and
// do what is needed.
//----------------------------------------------------------------------
uint32_t
Process::LoadImage (const FileSpec &image_spec, Error &error)
{
    DynamicLoader *loader = GetDynamicLoader();
    if (loader)
    {
        error = loader->CanLoadImage();
        if (error.Fail())
            return LLDB_INVALID_IMAGE_TOKEN;
    }
    
    if (error.Success())
    {
        ThreadSP thread_sp(GetThreadList ().GetSelectedThread());
        if (thread_sp == NULL)
            thread_sp = GetThreadList ().GetThreadAtIndex(0, true);
        
        if (thread_sp)
        {
            StackFrameSP frame_sp (thread_sp->GetStackFrameAtIndex (0));
            
            if (frame_sp)
            {
                ExecutionContext exe_ctx;
                frame_sp->CalculateExecutionContext (exe_ctx);
                bool unwind_on_error = true;
                StreamString expr;
                char path[PATH_MAX];
                image_spec.GetPath(path, sizeof(path));
                expr.Printf("dlopen (\"%s\", 2)", path);
                const char *prefix = "extern \"C\" void* dlopen (const char *path, int mode);\n";
                lldb::ValueObjectSP result_valobj_sp (ClangUserExpression::Evaluate (exe_ctx, unwind_on_error, expr.GetData(), prefix));
                if (result_valobj_sp->GetError().Success())
                {
                    Scalar scalar;
                    if (result_valobj_sp->ResolveValue (frame_sp.get(), scalar))
                    {
                        addr_t image_ptr = scalar.ULongLong(LLDB_INVALID_ADDRESS);
                        if (image_ptr != 0 && image_ptr != LLDB_INVALID_ADDRESS)
                        {
                            uint32_t image_token = m_image_tokens.size();
                            m_image_tokens.push_back (image_ptr);
                            return image_token;
                        }
                    }
                }
            }
        }
    }
    return LLDB_INVALID_IMAGE_TOKEN;
}

//----------------------------------------------------------------------
// UnloadImage
//
// This function provides a default implementation that works for most
// unix variants. Any Process subclasses that need to do shared library
// loading differently should override LoadImage and UnloadImage and
// do what is needed.
//----------------------------------------------------------------------
Error
Process::UnloadImage (uint32_t image_token)
{
    Error error;
    if (image_token < m_image_tokens.size())
    {
        const addr_t image_addr = m_image_tokens[image_token];
        if (image_addr == LLDB_INVALID_ADDRESS)
        {
            error.SetErrorString("image already unloaded");
        }
        else
        {
            DynamicLoader *loader = GetDynamicLoader();
            if (loader)
                error = loader->CanLoadImage();
            
            if (error.Success())
            {
                ThreadSP thread_sp(GetThreadList ().GetSelectedThread());
                if (thread_sp == NULL)
                    thread_sp = GetThreadList ().GetThreadAtIndex(0, true);
                
                if (thread_sp)
                {
                    StackFrameSP frame_sp (thread_sp->GetStackFrameAtIndex (0));
                    
                    if (frame_sp)
                    {
                        ExecutionContext exe_ctx;
                        frame_sp->CalculateExecutionContext (exe_ctx);
                        bool unwind_on_error = true;
                        StreamString expr;
                        expr.Printf("dlclose ((void *)0x%llx)", image_addr);
                        const char *prefix = "extern \"C\" int dlclose(void* handle);\n";
                        lldb::ValueObjectSP result_valobj_sp (ClangUserExpression::Evaluate (exe_ctx, unwind_on_error, expr.GetData(), prefix));
                        if (result_valobj_sp->GetError().Success())
                        {
                            Scalar scalar;
                            if (result_valobj_sp->ResolveValue (frame_sp.get(), scalar))
                            {
                                if (scalar.UInt(1))
                                {
                                    error.SetErrorStringWithFormat("expression failed: \"%s\"", expr.GetData());
                                }
                                else
                                {
                                    m_image_tokens[image_token] = LLDB_INVALID_ADDRESS;
                                }
                            }
                        }
                        else
                        {
                            error = result_valobj_sp->GetError();
                        }
                    }
                }
            }
        }
    }
    else
    {
        error.SetErrorString("invalid image token");
    }
    return error;
}

DynamicLoader *
Process::GetDynamicLoader()
{
    return NULL;
}

const ABI *
Process::GetABI()
{
    ConstString& triple = m_target_triple;

    if (triple.IsEmpty())
        return NULL;

    if (m_abi_sp.get() == NULL)
    {
        m_abi_sp.reset(ABI::FindPlugin(triple));
    }

    return m_abi_sp.get();
}

LanguageRuntime *
Process::GetLanguageRuntime(lldb::LanguageType language)
{
    LanguageRuntimeCollection::iterator pos;
    pos = m_language_runtimes.find (language);
    if (pos == m_language_runtimes.end())
    {
        lldb::LanguageRuntimeSP runtime(LanguageRuntime::FindPlugin(this, language));
        
        m_language_runtimes[language] 
            = runtime;
        return runtime.get();
    }
    else
        return (*pos).second.get();
}

CPPLanguageRuntime *
Process::GetCPPLanguageRuntime ()
{
    LanguageRuntime *runtime = GetLanguageRuntime(eLanguageTypeC_plus_plus);
    if (runtime != NULL && runtime->GetLanguageType() == eLanguageTypeC_plus_plus)
        return static_cast<CPPLanguageRuntime *> (runtime);
    return NULL;
}

ObjCLanguageRuntime *
Process::GetObjCLanguageRuntime ()
{
    LanguageRuntime *runtime = GetLanguageRuntime(eLanguageTypeObjC);
    if (runtime != NULL && runtime->GetLanguageType() == eLanguageTypeObjC)
        return static_cast<ObjCLanguageRuntime *> (runtime);
    return NULL;
}

BreakpointSiteList &
Process::GetBreakpointSiteList()
{
    return m_breakpoint_site_list;
}

const BreakpointSiteList &
Process::GetBreakpointSiteList() const
{
    return m_breakpoint_site_list;
}


void
Process::DisableAllBreakpointSites ()
{
    m_breakpoint_site_list.SetEnabledForAll (false);
}

Error
Process::ClearBreakpointSiteByID (lldb::user_id_t break_id)
{
    Error error (DisableBreakpointSiteByID (break_id));
    
    if (error.Success())
        m_breakpoint_site_list.Remove(break_id);

    return error;
}

Error
Process::DisableBreakpointSiteByID (lldb::user_id_t break_id)
{
    Error error;
    BreakpointSiteSP bp_site_sp = m_breakpoint_site_list.FindByID (break_id);
    if (bp_site_sp)
    {
        if (bp_site_sp->IsEnabled())
            error = DisableBreakpoint (bp_site_sp.get());
    }
    else
    {
        error.SetErrorStringWithFormat("invalid breakpoint site ID: %i", break_id);
    }

    return error;
}

Error
Process::EnableBreakpointSiteByID (lldb::user_id_t break_id)
{
    Error error;
    BreakpointSiteSP bp_site_sp = m_breakpoint_site_list.FindByID (break_id);
    if (bp_site_sp)
    {
        if (!bp_site_sp->IsEnabled())
            error = EnableBreakpoint (bp_site_sp.get());
    }
    else
    {
        error.SetErrorStringWithFormat("invalid breakpoint site ID: %i", break_id);
    }
    return error;
}

lldb::break_id_t
Process::CreateBreakpointSite (BreakpointLocationSP &owner, bool use_hardware)
{
    const addr_t load_addr = owner->GetAddress().GetLoadAddress (&m_target);
    if (load_addr != LLDB_INVALID_ADDRESS)
    {
        BreakpointSiteSP bp_site_sp;

        // Look up this breakpoint site.  If it exists, then add this new owner, otherwise
        // create a new breakpoint site and add it.

        bp_site_sp = m_breakpoint_site_list.FindByAddress (load_addr);

        if (bp_site_sp)
        {
            bp_site_sp->AddOwner (owner);
            owner->SetBreakpointSite (bp_site_sp);
            return bp_site_sp->GetID();
        }
        else
        {
            bp_site_sp.reset (new BreakpointSite (&m_breakpoint_site_list, owner, load_addr, LLDB_INVALID_THREAD_ID, use_hardware));
            if (bp_site_sp)
            {
                if (EnableBreakpoint (bp_site_sp.get()).Success())
                {
                    owner->SetBreakpointSite (bp_site_sp);
                    return m_breakpoint_site_list.Add (bp_site_sp);
                }
            }
        }
    }
    // We failed to enable the breakpoint
    return LLDB_INVALID_BREAK_ID;

}

void
Process::RemoveOwnerFromBreakpointSite (lldb::user_id_t owner_id, lldb::user_id_t owner_loc_id, BreakpointSiteSP &bp_site_sp)
{
    uint32_t num_owners = bp_site_sp->RemoveOwner (owner_id, owner_loc_id);
    if (num_owners == 0)
    {
        DisableBreakpoint(bp_site_sp.get());
        m_breakpoint_site_list.RemoveByAddress(bp_site_sp->GetLoadAddress());
    }
}


size_t
Process::RemoveBreakpointOpcodesFromBuffer (addr_t bp_addr, size_t size, uint8_t *buf) const
{
    size_t bytes_removed = 0;
    addr_t intersect_addr;
    size_t intersect_size;
    size_t opcode_offset;
    size_t idx;
    BreakpointSiteSP bp;

    for (idx = 0; (bp = m_breakpoint_site_list.GetByIndex(idx)) != NULL; ++idx)
    {
        if (bp->GetType() == BreakpointSite::eSoftware)
        {
            if (bp->IntersectsRange(bp_addr, size, &intersect_addr, &intersect_size, &opcode_offset))
            {
                assert(bp_addr <= intersect_addr && intersect_addr < bp_addr + size);
                assert(bp_addr < intersect_addr + intersect_size && intersect_addr + intersect_size <= bp_addr + size);
                assert(opcode_offset + intersect_size <= bp->GetByteSize());
                size_t buf_offset = intersect_addr - bp_addr;
                ::memcpy(buf + buf_offset, bp->GetSavedOpcodeBytes() + opcode_offset, intersect_size);
            }
        }
    }
    return bytes_removed;
}


Error
Process::EnableSoftwareBreakpoint (BreakpointSite *bp_site)
{
    Error error;
    assert (bp_site != NULL);
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    const addr_t bp_addr = bp_site->GetLoadAddress();
    if (log)
        log->Printf ("Process::EnableSoftwareBreakpoint (site_id = %d) addr = 0x%llx", bp_site->GetID(), (uint64_t)bp_addr);
    if (bp_site->IsEnabled())
    {
        if (log)
            log->Printf ("Process::EnableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- already enabled", bp_site->GetID(), (uint64_t)bp_addr);
        return error;
    }

    if (bp_addr == LLDB_INVALID_ADDRESS)
    {
        error.SetErrorString("BreakpointSite contains an invalid load address.");
        return error;
    }
    // Ask the lldb::Process subclass to fill in the correct software breakpoint
    // trap for the breakpoint site
    const size_t bp_opcode_size = GetSoftwareBreakpointTrapOpcode(bp_site);

    if (bp_opcode_size == 0)
    {
        error.SetErrorStringWithFormat ("Process::GetSoftwareBreakpointTrapOpcode() returned zero, unable to get breakpoint trap for address 0x%llx.\n", bp_addr);
    }
    else
    {
        const uint8_t * const bp_opcode_bytes = bp_site->GetTrapOpcodeBytes();

        if (bp_opcode_bytes == NULL)
        {
            error.SetErrorString ("BreakpointSite doesn't contain a valid breakpoint trap opcode.");
            return error;
        }

        // Save the original opcode by reading it
        if (DoReadMemory(bp_addr, bp_site->GetSavedOpcodeBytes(), bp_opcode_size, error) == bp_opcode_size)
        {
            // Write a software breakpoint in place of the original opcode
            if (DoWriteMemory(bp_addr, bp_opcode_bytes, bp_opcode_size, error) == bp_opcode_size)
            {
                uint8_t verify_bp_opcode_bytes[64];
                if (DoReadMemory(bp_addr, verify_bp_opcode_bytes, bp_opcode_size, error) == bp_opcode_size)
                {
                    if (::memcmp(bp_opcode_bytes, verify_bp_opcode_bytes, bp_opcode_size) == 0)
                    {
                        bp_site->SetEnabled(true);
                        bp_site->SetType (BreakpointSite::eSoftware);
                        if (log)
                            log->Printf ("Process::EnableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- SUCCESS",
                                         bp_site->GetID(),
                                         (uint64_t)bp_addr);
                    }
                    else
                        error.SetErrorString("Failed to verify the breakpoint trap in memory.");
                }
                else
                    error.SetErrorString("Unable to read memory to verify breakpoint trap.");
            }
            else
                error.SetErrorString("Unable to write breakpoint trap to memory.");
        }
        else
            error.SetErrorString("Unable to read memory at breakpoint address.");
    }
    if (log)
        log->Printf ("Process::EnableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- FAILED: %s",
                     bp_site->GetID(),
                     (uint64_t)bp_addr,
                     error.AsCString());
    return error;
}

Error
Process::DisableSoftwareBreakpoint (BreakpointSite *bp_site)
{
    Error error;
    assert (bp_site != NULL);
    LogSP log(lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    addr_t bp_addr = bp_site->GetLoadAddress();
    lldb::user_id_t breakID = bp_site->GetID();
    if (log)
        log->Printf ("ProcessMacOSX::DisableBreakpoint (breakID = %d) addr = 0x%llx", breakID, (uint64_t)bp_addr);

    if (bp_site->IsHardware())
    {
        error.SetErrorString("Breakpoint site is a hardware breakpoint.");
    }
    else if (bp_site->IsEnabled())
    {
        const size_t break_op_size = bp_site->GetByteSize();
        const uint8_t * const break_op = bp_site->GetTrapOpcodeBytes();
        if (break_op_size > 0)
        {
            // Clear a software breakoint instruction
            uint8_t curr_break_op[8];
            assert (break_op_size <= sizeof(curr_break_op));
            bool break_op_found = false;

            // Read the breakpoint opcode
            if (DoReadMemory (bp_addr, curr_break_op, break_op_size, error) == break_op_size)
            {
                bool verify = false;
                // Make sure we have the a breakpoint opcode exists at this address
                if (::memcmp (curr_break_op, break_op, break_op_size) == 0)
                {
                    break_op_found = true;
                    // We found a valid breakpoint opcode at this address, now restore
                    // the saved opcode.
                    if (DoWriteMemory (bp_addr, bp_site->GetSavedOpcodeBytes(), break_op_size, error) == break_op_size)
                    {
                        verify = true;
                    }
                    else
                        error.SetErrorString("Memory write failed when restoring original opcode.");
                }
                else
                {
                    error.SetErrorString("Original breakpoint trap is no longer in memory.");
                    // Set verify to true and so we can check if the original opcode has already been restored
                    verify = true;
                }

                if (verify)
                {
                    uint8_t verify_opcode[8];
                    assert (break_op_size < sizeof(verify_opcode));
                    // Verify that our original opcode made it back to the inferior
                    if (DoReadMemory (bp_addr, verify_opcode, break_op_size, error) == break_op_size)
                    {
                        // compare the memory we just read with the original opcode
                        if (::memcmp (bp_site->GetSavedOpcodeBytes(), verify_opcode, break_op_size) == 0)
                        {
                            // SUCCESS
                            bp_site->SetEnabled(false);
                            if (log)
                                log->Printf ("Process::DisableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- SUCCESS", bp_site->GetID(), (uint64_t)bp_addr);
                            return error;
                        }
                        else
                        {
                            if (break_op_found)
                                error.SetErrorString("Failed to restore original opcode.");
                        }
                    }
                    else
                        error.SetErrorString("Failed to read memory to verify that breakpoint trap was restored.");
                }
            }
            else
                error.SetErrorString("Unable to read memory that should contain the breakpoint trap.");
        }
    }
    else
    {
        if (log)
            log->Printf ("Process::DisableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- already disabled", bp_site->GetID(), (uint64_t)bp_addr);
        return error;
    }

    if (log)
        log->Printf ("Process::DisableSoftwareBreakpoint (site_id = %d) addr = 0x%llx -- FAILED: %s",
                     bp_site->GetID(),
                     (uint64_t)bp_addr,
                     error.AsCString());
    return error;

}


size_t
Process::ReadMemory (addr_t addr, void *buf, size_t size, Error &error)
{
    if (buf == NULL || size == 0)
        return 0;

    size_t bytes_read = 0;
    uint8_t *bytes = (uint8_t *)buf;
    
    while (bytes_read < size)
    {
        const size_t curr_size = size - bytes_read;
        const size_t curr_bytes_read = DoReadMemory (addr + bytes_read, 
                                                     bytes + bytes_read, 
                                                     curr_size,
                                                     error);
        bytes_read += curr_bytes_read;
        if (curr_bytes_read == curr_size || curr_bytes_read == 0)
            break;
    }

    // Replace any software breakpoint opcodes that fall into this range back
    // into "buf" before we return
    if (bytes_read > 0)
        RemoveBreakpointOpcodesFromBuffer (addr, bytes_read, (uint8_t *)buf);
    return bytes_read;
}

size_t
Process::WriteMemoryPrivate (addr_t addr, const void *buf, size_t size, Error &error)
{
    size_t bytes_written = 0;
    const uint8_t *bytes = (const uint8_t *)buf;
    
    while (bytes_written < size)
    {
        const size_t curr_size = size - bytes_written;
        const size_t curr_bytes_written = DoWriteMemory (addr + bytes_written, 
                                                         bytes + bytes_written, 
                                                         curr_size,
                                                         error);
        bytes_written += curr_bytes_written;
        if (curr_bytes_written == curr_size || curr_bytes_written == 0)
            break;
    }
    return bytes_written;
}

size_t
Process::WriteMemory (addr_t addr, const void *buf, size_t size, Error &error)
{
    if (buf == NULL || size == 0)
        return 0;
    // We need to write any data that would go where any current software traps
    // (enabled software breakpoints) any software traps (breakpoints) that we
    // may have placed in our tasks memory.

    BreakpointSiteList::collection::const_iterator iter = m_breakpoint_site_list.GetMap()->lower_bound (addr);
    BreakpointSiteList::collection::const_iterator end =  m_breakpoint_site_list.GetMap()->end();

    if (iter == end || iter->second->GetLoadAddress() > addr + size)
        return DoWriteMemory(addr, buf, size, error);

    BreakpointSiteList::collection::const_iterator pos;
    size_t bytes_written = 0;
    addr_t intersect_addr = 0;
    size_t intersect_size = 0;
    size_t opcode_offset = 0;
    const uint8_t *ubuf = (const uint8_t *)buf;

    for (pos = iter; pos != end; ++pos)
    {
        BreakpointSiteSP bp;
        bp = pos->second;

        assert(bp->IntersectsRange(addr, size, &intersect_addr, &intersect_size, &opcode_offset));
        assert(addr <= intersect_addr && intersect_addr < addr + size);
        assert(addr < intersect_addr + intersect_size && intersect_addr + intersect_size <= addr + size);
        assert(opcode_offset + intersect_size <= bp->GetByteSize());

        // Check for bytes before this breakpoint
        const addr_t curr_addr = addr + bytes_written;
        if (intersect_addr > curr_addr)
        {
            // There are some bytes before this breakpoint that we need to
            // just write to memory
            size_t curr_size = intersect_addr - curr_addr;
            size_t curr_bytes_written = WriteMemoryPrivate (curr_addr, 
                                                            ubuf + bytes_written, 
                                                            curr_size, 
                                                            error);
            bytes_written += curr_bytes_written;
            if (curr_bytes_written != curr_size)
            {
                // We weren't able to write all of the requested bytes, we
                // are done looping and will return the number of bytes that
                // we have written so far.
                break;
            }
        }

        // Now write any bytes that would cover up any software breakpoints
        // directly into the breakpoint opcode buffer
        ::memcpy(bp->GetSavedOpcodeBytes() + opcode_offset, ubuf + bytes_written, intersect_size);
        bytes_written += intersect_size;
    }

    // Write any remaining bytes after the last breakpoint if we have any left
    if (bytes_written < size)
        bytes_written += WriteMemoryPrivate (addr + bytes_written, 
                                             ubuf + bytes_written, 
                                             size - bytes_written, 
                                             error);
    
    return bytes_written;
}

addr_t
Process::AllocateMemory(size_t size, uint32_t permissions, Error &error)
{
    // Fixme: we should track the blocks we've allocated, and clean them up...
    // We could even do our own allocator here if that ends up being more efficient.
    return DoAllocateMemory (size, permissions, error);
}

Error
Process::DeallocateMemory (addr_t ptr)
{
    return DoDeallocateMemory (ptr);
}


Error
Process::EnableWatchpoint (WatchpointLocation *watchpoint)
{
    Error error;
    error.SetErrorString("watchpoints are not supported");
    return error;
}

Error
Process::DisableWatchpoint (WatchpointLocation *watchpoint)
{
    Error error;
    error.SetErrorString("watchpoints are not supported");
    return error;
}

StateType
Process::WaitForProcessStopPrivate (const TimeValue *timeout, EventSP &event_sp)
{
    StateType state;
    // Now wait for the process to launch and return control to us, and then
    // call DidLaunch:
    while (1)
    {
        // FIXME: Might want to put a timeout in here:
        state = WaitForStateChangedEventsPrivate (NULL, event_sp);
        if (state == eStateStopped || state == eStateCrashed || state == eStateExited)
            break;
        else
            HandlePrivateEvent (event_sp);
    }
    return state;
}

Error
Process::Launch
(
    char const *argv[],
    char const *envp[],
    uint32_t launch_flags,
    const char *stdin_path,
    const char *stdout_path,
    const char *stderr_path
)
{
    Error error;
    m_target_triple.Clear();
    m_abi_sp.reset();

    Module *exe_module = m_target.GetExecutableModule().get();
    if (exe_module)
    {
        char exec_file_path[PATH_MAX];
        exe_module->GetFileSpec().GetPath(exec_file_path, sizeof(exec_file_path));
        if (exe_module->GetFileSpec().Exists())
        {
            error = WillLaunch (exe_module);
            if (error.Success())
            {
                SetPublicState (eStateLaunching);
                // The args coming in should not contain the application name, the
                // lldb_private::Process class will add this in case the executable
                // gets resolved to a different file than was given on the command
                // line (like when an applicaiton bundle is specified and will
                // resolve to the contained exectuable file, or the file given was
                // a symlink or other file system link that resolves to a different
                // file).

                // Get the resolved exectuable path

                // Make a new argument vector
                std::vector<const char *> exec_path_plus_argv;
                // Append the resolved executable path
                exec_path_plus_argv.push_back (exec_file_path);

                // Push all args if there are any
                if (argv)
                {
                    for (int i = 0; argv[i]; ++i)
                        exec_path_plus_argv.push_back(argv[i]);
                }

                // Push a NULL to terminate the args.
                exec_path_plus_argv.push_back(NULL);

                // Now launch using these arguments.
                error = DoLaunch (exe_module, 
                                  exec_path_plus_argv.empty() ? NULL : &exec_path_plus_argv.front(), 
                                  envp, 
                                  launch_flags,
                                  stdin_path, 
                                  stdout_path, 
                                  stderr_path);

                if (error.Fail())
                {
                    if (GetID() != LLDB_INVALID_PROCESS_ID)
                    {
                        SetID (LLDB_INVALID_PROCESS_ID);
                        const char *error_string = error.AsCString();
                        if (error_string == NULL)
                            error_string = "launch failed";
                        SetExitStatus (-1, error_string);
                    }
                }
                else
                {
                    EventSP event_sp;
                    StateType state = WaitForProcessStopPrivate(NULL, event_sp);

                    if (state == eStateStopped || state == eStateCrashed)
                    {
                        DidLaunch ();

                        // This delays passing the stopped event to listeners till DidLaunch gets
                        // a chance to complete...
                        HandlePrivateEvent (event_sp);
                        StartPrivateStateThread ();
                    }
                    else if (state == eStateExited)
                    {
                        // We exited while trying to launch somehow.  Don't call DidLaunch as that's
                        // not likely to work, and return an invalid pid.
                        HandlePrivateEvent (event_sp);
                    }
                }
            }
        }
        else
        {
            error.SetErrorStringWithFormat("File doesn't exist: '%s'.\n", exec_file_path);
        }
    }
    return error;
}

Error
Process::CompleteAttach ()
{
    Error error;

    if (GetID() == LLDB_INVALID_PROCESS_ID)
    {
        error.SetErrorString("no process");
    }

    EventSP event_sp;
    StateType state = WaitForProcessStopPrivate(NULL, event_sp);
    if (state == eStateStopped || state == eStateCrashed)
    {
        DidAttach ();
        // Figure out which one is the executable, and set that in our target:
        ModuleList &modules = GetTarget().GetImages();
    
        size_t num_modules = modules.GetSize();
        for (int i = 0; i < num_modules; i++)
        {
            ModuleSP module_sp = modules.GetModuleAtIndex(i);
            if (module_sp->IsExecutable())
            {
                ModuleSP exec_module = GetTarget().GetExecutableModule();
                if (!exec_module || exec_module != module_sp)
                {
                    
                    GetTarget().SetExecutableModule (module_sp, false);
                }
                break;
            }
        }

        // This delays passing the stopped event to listeners till DidLaunch gets
        // a chance to complete...
        HandlePrivateEvent(event_sp);
        StartPrivateStateThread();
    }
    else
    {
        // We exited while trying to launch somehow.  Don't call DidLaunch as that's
        // not likely to work, and return an invalid pid.
        if (state == eStateExited)
            HandlePrivateEvent (event_sp);
        error.SetErrorStringWithFormat("invalid state after attach: %s", 
                                        lldb_private::StateAsCString(state));
    }
    return error;
}

Error
Process::Attach (lldb::pid_t attach_pid)
{

    m_target_triple.Clear();
    m_abi_sp.reset();

    // Find the process and its architecture.  Make sure it matches the architecture
    // of the current Target, and if not adjust it.
    
    ArchSpec attach_spec = GetArchSpecForExistingProcess (attach_pid);
    if (attach_spec != GetTarget().GetArchitecture())
    {
        // Set the architecture on the target.
        GetTarget().SetArchitecture(attach_spec);
    }

    Error error (WillAttachToProcessWithID(attach_pid));
    if (error.Success())
    {
        SetPublicState (eStateAttaching);

        error = DoAttachToProcessWithID (attach_pid);
        if (error.Success())
        {
            error = CompleteAttach();
        }
        else
        {
            if (GetID() != LLDB_INVALID_PROCESS_ID)
            {
                SetID (LLDB_INVALID_PROCESS_ID);
                const char *error_string = error.AsCString();
                if (error_string == NULL)
                    error_string = "attach failed";

                SetExitStatus(-1, error_string);
            }
        }
    }
    return error;
}

Error
Process::Attach (const char *process_name, bool wait_for_launch)
{
    m_target_triple.Clear();
    m_abi_sp.reset();
    
    // Find the process and its architecture.  Make sure it matches the architecture
    // of the current Target, and if not adjust it.
    
    if (!wait_for_launch)
    {
        ArchSpec attach_spec = GetArchSpecForExistingProcess (process_name);
        if (attach_spec.IsValid() && attach_spec != GetTarget().GetArchitecture())
        {
            // Set the architecture on the target.
            GetTarget().SetArchitecture(attach_spec);
        }
    }
    
    Error error (WillAttachToProcessWithName(process_name, wait_for_launch));
    if (error.Success())
    {
        SetPublicState (eStateAttaching);
        error = DoAttachToProcessWithName (process_name, wait_for_launch);
        if (error.Fail())
        {
            if (GetID() != LLDB_INVALID_PROCESS_ID)
            {
                SetID (LLDB_INVALID_PROCESS_ID);
                const char *error_string = error.AsCString();
                if (error_string == NULL)
                    error_string = "attach failed";

                SetExitStatus(-1, error_string);
            }
        }
        else
        {
            error = CompleteAttach();
        }
    }
    return error;
}

Error
Process::Resume ()
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf("Process::Resume() m_stop_id = %u", m_stop_id);

    Error error (WillResume());
    // Tell the process it is about to resume before the thread list
    if (error.Success())
    {
        // Now let the thread list know we are about to resume to it
        // can let all of our threads know that they are about to be
        // resumed. Threads will each be called with
        // Thread::WillResume(StateType) where StateType contains the state
        // that they are supposed to have when the process is resumed
        // (suspended/running/stepping). Threads should also check
        // their resume signal in lldb::Thread::GetResumeSignal()
        // to see if they are suppoed to start back up with a signal.
        if (m_thread_list.WillResume())
        {
            error = DoResume();
            if (error.Success())
            {
                DidResume();
                m_thread_list.DidResume();
            }
        }
        else
        {
            error.SetErrorStringWithFormat("thread list returned flase after WillResume");
        }
    }
    return error;
}

Error
Process::Halt ()
{
    Error error (WillHalt());
    
    if (error.Success())
    {
        error = DoHalt();
        if (error.Success())
            DidHalt();
    }
    return error;
}

Error
Process::Detach ()
{
    Error error (WillDetach());

    if (error.Success())
    {
        DisableAllBreakpointSites();
        error = DoDetach(); 
        if (error.Success())
        {
            DidDetach();
            StopPrivateStateThread();
        }
    }
    return error;
}

Error
Process::Destroy ()
{
    Error error (WillDestroy());
    if (error.Success())
    {
        DisableAllBreakpointSites();
        error = DoDestroy();
        if (error.Success())
        {
            DidDestroy();
            StopPrivateStateThread();
        }
    }
    return error;
}

Error
Process::Signal (int signal)
{
    Error error (WillSignal());
    if (error.Success())
    {
        error = DoSignal(signal);
        if (error.Success())
            DidSignal();
    }
    return error;
}

UnixSignals &
Process::GetUnixSignals ()
{
    return m_unix_signals;
}

Target &
Process::GetTarget ()
{
    return m_target;
}

const Target &
Process::GetTarget () const
{
    return m_target;
}

uint32_t
Process::GetAddressByteSize()
{
    return m_target.GetArchitecture().GetAddressByteSize();
}

bool
Process::ShouldBroadcastEvent (Event *event_ptr)
{
    const StateType state = Process::ProcessEventData::GetStateFromEvent (event_ptr);
    bool return_value = true;
    LogSP log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EVENTS));

    switch (state)
    {
        case eStateAttaching:
        case eStateLaunching:
        case eStateDetached:
        case eStateExited:
        case eStateUnloaded:
            // These events indicate changes in the state of the debugging session, always report them.
            return_value = true;
            break;
        case eStateInvalid:
            // We stopped for no apparent reason, don't report it.
            return_value = false;
            break;
        case eStateRunning:
        case eStateStepping:
            // If we've started the target running, we handle the cases where we
            // are already running and where there is a transition from stopped to
            // running differently.
            // running -> running: Automatically suppress extra running events
            // stopped -> running: Report except when there is one or more no votes
            //     and no yes votes.
            SynchronouslyNotifyStateChanged (state);
            switch (m_public_state.GetValue())
            {
                case eStateRunning:
                case eStateStepping:
                    // We always suppress multiple runnings with no PUBLIC stop in between.
                    return_value = false;
                    break;
                default:
                    // TODO: make this work correctly. For now always report
                    // run if we aren't running so we don't miss any runnning
                    // events. If I run the lldb/test/thread/a.out file and
                    // break at main.cpp:58, run and hit the breakpoints on
                    // multiple threads, then somehow during the stepping over
                    // of all breakpoints no run gets reported.
                    return_value = true;

                    // This is a transition from stop to run.
                    switch (m_thread_list.ShouldReportRun (event_ptr))
                    {
                        case eVoteYes:
                        case eVoteNoOpinion:
                            return_value = true;
                            break;
                        case eVoteNo:
                            return_value = false;
                            break;
                    }
                    break;
            }
            break;
        case eStateStopped:
        case eStateCrashed:
        case eStateSuspended:
        {
            // We've stopped.  First see if we're going to restart the target.
            // If we are going to stop, then we always broadcast the event.
            // If we aren't going to stop, let the thread plans decide if we're going to report this event.
            // If no thread has an opinion, we don't report it.
            if (state != eStateInvalid)
            {

                RefreshStateAfterStop ();

                if (m_thread_list.ShouldStop (event_ptr) == false)
                {
                    switch (m_thread_list.ShouldReportStop (event_ptr))
                    {
                        case eVoteYes:
                            Process::ProcessEventData::SetRestartedInEvent (event_ptr, true);
                            // Intentional fall-through here.
                        case eVoteNoOpinion:
                        case eVoteNo:
                            return_value = false;
                            break;
                    }

                    if (log)
                        log->Printf ("Process::ShouldBroadcastEvent (%p) Restarting process", event_ptr, StateAsCString(state));
                    Resume ();
                }
                else
                {
                    return_value = true;
                    SynchronouslyNotifyStateChanged (state);
                }
            }
        }
    }

    if (log)
        log->Printf ("Process::ShouldBroadcastEvent (%p) => %s", event_ptr, StateAsCString(state), return_value ? "YES" : "NO");
    return return_value;
}

//------------------------------------------------------------------
// Thread Queries
//------------------------------------------------------------------

ThreadList &
Process::GetThreadList ()
{
    return m_thread_list;
}

const ThreadList &
Process::GetThreadList () const
{
    return m_thread_list;
}


bool
Process::StartPrivateStateThread ()
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EVENTS));

    if (log)
        log->Printf ("Process::%s ( )", __FUNCTION__);

    // Create a thread that watches our internal state and controls which
    // events make it to clients (into the DCProcess event queue).
    m_private_state_thread = Host::ThreadCreate ("<lldb.process.internal-state>", Process::PrivateStateThread, this, NULL);
    return m_private_state_thread != LLDB_INVALID_HOST_THREAD;
}

void
Process::PausePrivateStateThread ()
{
    ControlPrivateStateThread (eBroadcastInternalStateControlPause);
}

void
Process::ResumePrivateStateThread ()
{
    ControlPrivateStateThread (eBroadcastInternalStateControlResume);
}

void
Process::StopPrivateStateThread ()
{
    ControlPrivateStateThread (eBroadcastInternalStateControlStop);
}

void
Process::ControlPrivateStateThread (uint32_t signal)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EVENTS));

    assert (signal == eBroadcastInternalStateControlStop ||
            signal == eBroadcastInternalStateControlPause ||
            signal == eBroadcastInternalStateControlResume);

    if (log)
        log->Printf ("Process::%s ( ) - signal: %d", __FUNCTION__, signal);

    // Signal the private state thread
    if (m_private_state_thread != LLDB_INVALID_HOST_THREAD)
    {
        TimeValue timeout_time;
        bool timed_out;

        m_private_state_control_broadcaster.BroadcastEvent (signal, NULL);

        timeout_time = TimeValue::Now();
        timeout_time.OffsetWithSeconds(2);
        m_private_state_control_wait.WaitForValueEqualTo (true, &timeout_time, &timed_out);
        m_private_state_control_wait.SetValue (false, eBroadcastNever);

        if (signal == eBroadcastInternalStateControlStop)
        {
            if (timed_out)
                Host::ThreadCancel (m_private_state_thread, NULL);

            thread_result_t result = NULL;
            Host::ThreadJoin (m_private_state_thread, &result, NULL);
            m_private_state_thread = LLDB_INVALID_HOST_THREAD;
        }
    }
}

void
Process::HandlePrivateEvent (EventSP &event_sp)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));
    const StateType internal_state = Process::ProcessEventData::GetStateFromEvent(event_sp.get());
    // See if we should broadcast this state to external clients?
    const bool should_broadcast = ShouldBroadcastEvent (event_sp.get());
    if (log)
        log->Printf ("Process::%s (arg = %p, pid = %i) got event '%s' broadcast = %s", __FUNCTION__, this, GetID(), StateAsCString(internal_state), should_broadcast ? "yes" : "no");

    if (should_broadcast)
    {
        if (log)
        {
            log->Printf ("\tChanging public state from: %s to %s", StateAsCString(GetState ()), StateAsCString (internal_state));
        }
        Process::ProcessEventData::SetUpdateStateOnRemoval(event_sp.get());
        BroadcastEvent (event_sp);
    }
    else
    {
        if (log)
        {
            log->Printf ("\tNot changing public state with event: %s", StateAsCString (internal_state));
        }
    }
}

void *
Process::PrivateStateThread (void *arg)
{
    Process *proc = static_cast<Process*> (arg);
    void *result = proc->RunPrivateStateThread ();
    return result;
}

void *
Process::RunPrivateStateThread ()
{
    bool control_only = false;
    m_private_state_control_wait.SetValue (false, eBroadcastNever);

    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS));
    if (log)
        log->Printf ("Process::%s (arg = %p, pid = %i) thread starting...", __FUNCTION__, this, GetID());

    bool exit_now = false;
    while (!exit_now)
    {
        EventSP event_sp;
        WaitForEventsPrivate (NULL, event_sp, control_only);
        if (event_sp->BroadcasterIs(&m_private_state_control_broadcaster))
        {
            switch (event_sp->GetType())
            {
            case eBroadcastInternalStateControlStop:
                exit_now = true;
                continue;   // Go to next loop iteration so we exit without
                break;      // doing any internal state managment below

            case eBroadcastInternalStateControlPause:
                control_only = true;
                break;

            case eBroadcastInternalStateControlResume:
                control_only = false;
                break;
            }
            m_private_state_control_wait.SetValue (true, eBroadcastAlways);
        }


        const StateType internal_state = Process::ProcessEventData::GetStateFromEvent(event_sp.get());

        if (internal_state != eStateInvalid)
        {
            HandlePrivateEvent (event_sp);
        }

        if (internal_state == eStateInvalid || 
            internal_state == eStateExited  ||
            internal_state == eStateDetached )
            break;
    }

    // Verify log is still enabled before attempting to write to it...
    log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_PROCESS);
    if (log)
        log->Printf ("Process::%s (arg = %p, pid = %i) thread exiting...", __FUNCTION__, this, GetID());

    m_private_state_thread = LLDB_INVALID_HOST_THREAD;
    return NULL;
}

//------------------------------------------------------------------
// Process Event Data
//------------------------------------------------------------------

Process::ProcessEventData::ProcessEventData () :
    EventData (),
    m_process_sp (),
    m_state (eStateInvalid),
    m_restarted (false),
    m_update_state (false)
{
}

Process::ProcessEventData::ProcessEventData (const ProcessSP &process_sp, StateType state) :
    EventData (),
    m_process_sp (process_sp),
    m_state (state),
    m_restarted (false),
    m_update_state (false)
{
}

Process::ProcessEventData::~ProcessEventData()
{
}

const ConstString &
Process::ProcessEventData::GetFlavorString ()
{
    static ConstString g_flavor ("Process::ProcessEventData");
    return g_flavor;
}

const ConstString &
Process::ProcessEventData::GetFlavor () const
{
    return ProcessEventData::GetFlavorString ();
}

const ProcessSP &
Process::ProcessEventData::GetProcessSP () const
{
    return m_process_sp;
}

StateType
Process::ProcessEventData::GetState () const
{
    return m_state;
}

bool
Process::ProcessEventData::GetRestarted () const
{
    return m_restarted;
}

void
Process::ProcessEventData::SetRestarted (bool new_value)
{
    m_restarted = new_value;
}

void
Process::ProcessEventData::DoOnRemoval (Event *event_ptr)
{
    // This function gets called twice for each event, once when the event gets pulled 
    // off of the private process event queue, and once when it gets pulled off of
    // the public event queue.  m_update_state is used to distinguish these
    // two cases; it is false when we're just pulling it off for private handling, 
    // and we don't want to do the breakpoint command handling then.
    
    if (!m_update_state)
        return;
        
    m_process_sp->SetPublicState (m_state);
        
    // If we're stopped and haven't restarted, then do the breakpoint commands here:
    if (m_state == eStateStopped && ! m_restarted)
    {
        int num_threads = m_process_sp->GetThreadList().GetSize();
        int idx;

        for (idx = 0; idx < num_threads; ++idx)
        {
            lldb::ThreadSP thread_sp = m_process_sp->GetThreadList().GetThreadAtIndex(idx);

            StopInfoSP stop_info_sp = thread_sp->GetStopInfo ();
            if (stop_info_sp)
            {
                stop_info_sp->PerformAction(event_ptr);
            }
        }
        
        // The stop action might restart the target.  If it does, then we want to mark that in the
        // event so that whoever is receiving it will know to wait for the running event and reflect
        // that state appropriately.

        if (m_process_sp->GetPrivateState() == eStateRunning)
            SetRestarted(true);
    }
}

void
Process::ProcessEventData::Dump (Stream *s) const
{
    if (m_process_sp)
        s->Printf(" process = %p (pid = %u), ", m_process_sp.get(), m_process_sp->GetID());

    s->Printf("state = %s", StateAsCString(GetState()));;
}

const Process::ProcessEventData *
Process::ProcessEventData::GetEventDataFromEvent (const Event *event_ptr)
{
    if (event_ptr)
    {
        const EventData *event_data = event_ptr->GetData();
        if (event_data && event_data->GetFlavor() == ProcessEventData::GetFlavorString())
            return static_cast <const ProcessEventData *> (event_ptr->GetData());
    }
    return NULL;
}

ProcessSP
Process::ProcessEventData::GetProcessFromEvent (const Event *event_ptr)
{
    ProcessSP process_sp;
    const ProcessEventData *data = GetEventDataFromEvent (event_ptr);
    if (data)
        process_sp = data->GetProcessSP();
    return process_sp;
}

StateType
Process::ProcessEventData::GetStateFromEvent (const Event *event_ptr)
{
    const ProcessEventData *data = GetEventDataFromEvent (event_ptr);
    if (data == NULL)
        return eStateInvalid;
    else
        return data->GetState();
}

bool
Process::ProcessEventData::GetRestartedFromEvent (const Event *event_ptr)
{
    const ProcessEventData *data = GetEventDataFromEvent (event_ptr);
    if (data == NULL)
        return false;
    else
        return data->GetRestarted();
}

void
Process::ProcessEventData::SetRestartedInEvent (Event *event_ptr, bool new_value)
{
    ProcessEventData *data = const_cast<ProcessEventData *>(GetEventDataFromEvent (event_ptr));
    if (data != NULL)
        data->SetRestarted(new_value);
}

bool
Process::ProcessEventData::SetUpdateStateOnRemoval (Event *event_ptr)
{
    ProcessEventData *data = const_cast<ProcessEventData *>(GetEventDataFromEvent (event_ptr));
    if (data)
    {
        data->SetUpdateStateOnRemoval();
        return true;
    }
    return false;
}

void
Process::ProcessEventData::SetUpdateStateOnRemoval()
{
    m_update_state = true;
}

Target *
Process::CalculateTarget ()
{
    return &m_target;
}

Process *
Process::CalculateProcess ()
{
    return this;
}

Thread *
Process::CalculateThread ()
{
    return NULL;
}

StackFrame *
Process::CalculateStackFrame ()
{
    return NULL;
}

void
Process::CalculateExecutionContext (ExecutionContext &exe_ctx)
{
    exe_ctx.target = &m_target;
    exe_ctx.process = this;
    exe_ctx.thread = NULL;
    exe_ctx.frame = NULL;
}

lldb::ProcessSP
Process::GetSP ()
{
    return GetTarget().GetProcessSP();
}

ClangPersistentVariables &
Process::GetPersistentVariables()
{
    return m_persistent_vars;
}

uint32_t
Process::ListProcessesMatchingName (const char *name, StringList &matches, std::vector<lldb::pid_t> &pids)
{
    return 0;
}
    
ArchSpec
Process::GetArchSpecForExistingProcess (lldb::pid_t pid)
{
    return Host::GetArchSpecForExistingProcess (pid);
}

ArchSpec
Process::GetArchSpecForExistingProcess (const char *process_name)
{
    return Host::GetArchSpecForExistingProcess (process_name);
}

lldb::UserSettingsControllerSP
Process::GetSettingsController (bool finish)
{
    static UserSettingsControllerSP g_settings_controller (new SettingsController);
    static bool initialized = false;

    if (!initialized)
    {
        initialized = UserSettingsController::InitializeSettingsController (g_settings_controller,
                                                             Process::SettingsController::global_settings_table,
                                                             Process::SettingsController::instance_settings_table);
    }

    if (finish)
    {
        UserSettingsController::FinalizeSettingsController (g_settings_controller);
        g_settings_controller.reset();
        initialized = false;
    }

    return g_settings_controller;
}

void
Process::UpdateInstanceName ()
{
    ModuleSP module_sp = GetTarget().GetExecutableModule();
    if (module_sp)
    {
        StreamString sstr;
        sstr.Printf ("%s", module_sp->GetFileSpec().GetFilename().AsCString());
                    
	Process::GetSettingsController()->RenameInstanceSettings (GetInstanceName().AsCString(),
                                                                  sstr.GetData());
    }
}

//--------------------------------------------------------------
// class Process::SettingsController
//--------------------------------------------------------------

Process::SettingsController::SettingsController () :
    UserSettingsController ("process", Target::GetSettingsController())
{
    m_default_settings.reset (new ProcessInstanceSettings (*this, false,
                                                           InstanceSettings::GetDefaultName().AsCString()));
}

Process::SettingsController::~SettingsController ()
{
}

lldb::InstanceSettingsSP
Process::SettingsController::CreateInstanceSettings (const char *instance_name)
{
    ProcessInstanceSettings *new_settings = new ProcessInstanceSettings (*(Process::GetSettingsController().get()),
                                                                         false, instance_name);
    lldb::InstanceSettingsSP new_settings_sp (new_settings);
    return new_settings_sp;
}

//--------------------------------------------------------------
// class ProcessInstanceSettings
//--------------------------------------------------------------

ProcessInstanceSettings::ProcessInstanceSettings (UserSettingsController &owner, bool live_instance, 
                                                  const char *name) :
    InstanceSettings (owner, (name == NULL ? InstanceSettings::InvalidName().AsCString() : name), live_instance), 
    m_run_args (),
    m_env_vars (),
    m_input_path (),
    m_output_path (),
    m_error_path (),
    m_plugin (),
    m_disable_aslr (true)
{
    // CopyInstanceSettings is a pure virtual function in InstanceSettings; it therefore cannot be called
    // until the vtables for ProcessInstanceSettings are properly set up, i.e. AFTER all the initializers.
    // For this reason it has to be called here, rather than in the initializer or in the parent constructor.
    // This is true for CreateInstanceName() too.

    if (GetInstanceName () == InstanceSettings::InvalidName())
    {
        ChangeInstanceName (std::string (CreateInstanceName().AsCString()));
        m_owner.RegisterInstanceSettings (this);
    }

    if (live_instance)
    {
        const lldb::InstanceSettingsSP &pending_settings = m_owner.FindPendingSettings (m_instance_name);
        CopyInstanceSettings (pending_settings,false);
        //m_owner.RemovePendingSettings (m_instance_name);
    }
}

ProcessInstanceSettings::ProcessInstanceSettings (const ProcessInstanceSettings &rhs) :
    InstanceSettings (*(Process::GetSettingsController().get()), CreateInstanceName().AsCString()),
    m_run_args (rhs.m_run_args),
    m_env_vars (rhs.m_env_vars),
    m_input_path (rhs.m_input_path),
    m_output_path (rhs.m_output_path),
    m_error_path (rhs.m_error_path),
    m_plugin (rhs.m_plugin),
    m_disable_aslr (rhs.m_disable_aslr)
{
    if (m_instance_name != InstanceSettings::GetDefaultName())
    {
        const lldb::InstanceSettingsSP &pending_settings = m_owner.FindPendingSettings (m_instance_name);
        CopyInstanceSettings (pending_settings,false);
        m_owner.RemovePendingSettings (m_instance_name);
    }
}

ProcessInstanceSettings::~ProcessInstanceSettings ()
{
}

ProcessInstanceSettings&
ProcessInstanceSettings::operator= (const ProcessInstanceSettings &rhs)
{
    if (this != &rhs)
    {
        m_run_args = rhs.m_run_args;
        m_env_vars = rhs.m_env_vars;
        m_input_path = rhs.m_input_path;
        m_output_path = rhs.m_output_path;
        m_error_path = rhs.m_error_path;
        m_plugin = rhs.m_plugin;
        m_disable_aslr = rhs.m_disable_aslr;
    }

    return *this;
}


void
ProcessInstanceSettings::UpdateInstanceSettingsVariable (const ConstString &var_name,
                                                         const char *index_value,
                                                         const char *value,
                                                         const ConstString &instance_name,
                                                         const SettingEntry &entry,
                                                         lldb::VarSetOperationType op,
                                                         Error &err,
                                                         bool pending)
{
    if (var_name == RunArgsVarName())
        UserSettingsController::UpdateStringArrayVariable (op, index_value, m_run_args, value, err);
    else if (var_name == EnvVarsVarName())
        UserSettingsController::UpdateDictionaryVariable (op, index_value, m_env_vars, value, err);
    else if (var_name == InputPathVarName())
        UserSettingsController::UpdateStringVariable (op, m_input_path, value, err);
    else if (var_name == OutputPathVarName())
        UserSettingsController::UpdateStringVariable (op, m_output_path, value, err);
    else if (var_name == ErrorPathVarName())
        UserSettingsController::UpdateStringVariable (op, m_error_path, value, err);
    else if (var_name == PluginVarName())
        UserSettingsController::UpdateEnumVariable (entry.enum_values, (int *) &m_plugin, value, err);
    else if (var_name == DisableASLRVarName())
        UserSettingsController::UpdateBooleanVariable (op, m_disable_aslr, value, err);
}

void
ProcessInstanceSettings::CopyInstanceSettings (const lldb::InstanceSettingsSP &new_settings,
                                               bool pending)
{
    if (new_settings.get() == NULL)
        return;

    ProcessInstanceSettings *new_process_settings = (ProcessInstanceSettings *) new_settings.get();

    m_run_args = new_process_settings->m_run_args;
    m_env_vars = new_process_settings->m_env_vars;
    m_input_path = new_process_settings->m_input_path;
    m_output_path = new_process_settings->m_output_path;
    m_error_path = new_process_settings->m_error_path;
    m_plugin = new_process_settings->m_plugin;
    m_disable_aslr = new_process_settings->m_disable_aslr;
}

bool
ProcessInstanceSettings::GetInstanceSettingsValue (const SettingEntry &entry,
                                                   const ConstString &var_name,
                                                   StringList &value,
                                                   Error *err)
{
    if (var_name == RunArgsVarName())
    {
        if (m_run_args.GetArgumentCount() > 0)
        {
            for (int i = 0; i < m_run_args.GetArgumentCount(); ++i)
                value.AppendString (m_run_args.GetArgumentAtIndex (i));
        }
    }
    else if (var_name == EnvVarsVarName())
    {
        if (m_env_vars.size() > 0)
        {
            std::map<std::string, std::string>::iterator pos;
            for (pos = m_env_vars.begin(); pos != m_env_vars.end(); ++pos)
            {
                StreamString value_str;
                value_str.Printf ("%s=%s", pos->first.c_str(), pos->second.c_str());
                value.AppendString (value_str.GetData());
            }
        }
    }
    else if (var_name == InputPathVarName())
    {
        value.AppendString (m_input_path.c_str());
    }
    else if (var_name == OutputPathVarName())
    {
        value.AppendString (m_output_path.c_str());
    }
    else if (var_name == ErrorPathVarName())
    {
        value.AppendString (m_error_path.c_str());
    }
    else if (var_name == PluginVarName())
    {
        value.AppendString (UserSettingsController::EnumToString (entry.enum_values, (int) m_plugin));
    }
    else if (var_name == DisableASLRVarName())
    {
        if (m_disable_aslr)
            value.AppendString ("true");
        else
            value.AppendString ("false");
    }
    else
    {
        if (err)
            err->SetErrorStringWithFormat ("unrecognized variable name '%s'", var_name.AsCString());
        return false;
    }
    return true;
}

const ConstString
ProcessInstanceSettings::CreateInstanceName ()
{
    static int instance_count = 1;
    StreamString sstr;

    sstr.Printf ("process_%d", instance_count);
    ++instance_count;

    const ConstString ret_val (sstr.GetData());
    return ret_val;
}

const ConstString &
ProcessInstanceSettings::RunArgsVarName ()
{
    static ConstString run_args_var_name ("run-args");

    return run_args_var_name;
}

const ConstString &
ProcessInstanceSettings::EnvVarsVarName ()
{
    static ConstString env_vars_var_name ("env-vars");

    return env_vars_var_name;
}

const ConstString &
ProcessInstanceSettings::InputPathVarName ()
{
  static ConstString input_path_var_name ("input-path");

    return input_path_var_name;
}

const ConstString &
ProcessInstanceSettings::OutputPathVarName ()
{
    static ConstString output_path_var_name ("output-path");

    return output_path_var_name;
}

const ConstString &
ProcessInstanceSettings::ErrorPathVarName ()
{
    static ConstString error_path_var_name ("error-path");

    return error_path_var_name;
}

const ConstString &
ProcessInstanceSettings::PluginVarName ()
{
    static ConstString plugin_var_name ("plugin");

    return plugin_var_name;
}


const ConstString &
ProcessInstanceSettings::DisableASLRVarName ()
{
    static ConstString disable_aslr_var_name ("disable-aslr");

    return disable_aslr_var_name;
}


//--------------------------------------------------
// SettingsController Variable Tables
//--------------------------------------------------

SettingEntry
Process::SettingsController::global_settings_table[] =
{
  //{ "var-name",    var-type  ,        "default", enum-table, init'd, hidden, "help-text"},
    {  NULL, eSetVarTypeNone, NULL, NULL, 0, 0, NULL }
};


lldb::OptionEnumValueElement
Process::SettingsController::g_plugins[] =
{
    { eMacosx, "process.macosx", "Use the native MacOSX debugger plugin" },
    { eRemoteDebugger, "process.gdb-remote" , "Use the GDB Remote protocol based debugger plugin" },
    { 0, NULL, NULL }
};

SettingEntry
Process::SettingsController::instance_settings_table[] =
{
  //{ "var-name",    var-type,              "default",      enum-table, init'd, hidden, "help-text"},
    { "run-args",    eSetVarTypeArray,       NULL,          NULL,       false,  false,  "A list containing all the arguments to be passed to the executable when it is run." },
    { "env-vars",    eSetVarTypeDictionary,  NULL,          NULL,       false,  false,  "A list of all the environment variables to be passed to the executable's environment, and their values." },
    { "input-path",  eSetVarTypeString,      "/dev/stdin",  NULL,       false,  false,  "The file/path to be used by the executable program for reading its input." },
    { "output-path", eSetVarTypeString,      "/dev/stdout", NULL,       false,  false,  "The file/path to be used by the executable program for writing its output." },
    { "error-path",  eSetVarTypeString,      "/dev/stderr", NULL,       false,  false,  "The file/path to be used by the executable program for writings its error messages." },
    { "plugin",      eSetVarTypeEnum,        NULL         , g_plugins,  false,  false,  "The plugin to be used to run the process." },
    { "disable-aslr", eSetVarTypeBool,       "true",        NULL,       false,  false, "Disable Address Space Layout Randomization (ASLR)" },
    {  NULL, eSetVarTypeNone, NULL, NULL, 0, 0, NULL }
};



