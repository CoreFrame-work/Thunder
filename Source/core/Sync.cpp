 /*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
// ===========================================================================
//
// Filename:    sync.cpp
//
// Description: Implementation file for for the CriticalSection,
//              CBinarySemahore, CountingSemaphore and the Event
//              synchronisation classes.
//
// History
//
// Author        Reason                                             Date
// ---------------------------------------------------------------------------
// P. Wielders   Initial creation                                   2002/05/24
// M. Fransen    Switched to monotonic clock where possible         2018/08/22
//
// ===========================================================================

#include "Sync.h"
#include "ProcessInfo.h"
#include "Trace.h"

#ifdef __CORE_CRITICAL_SECTION_LOG__
#include "Thread.h"
#endif

#if defined(__LINUX__) && !defined(__APPLE__)
#include <asm/errno.h>
#include <time.h>
#include <unistd.h>
#include <semaphore.h>
#endif
#ifdef __APPLE__
#include <semaphore.h>
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/clock.h>
#endif
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
// GLOBAL INTERLOCKED METHODS
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

#if defined(BUILD_TESTS) && !defined(__APPLE__)
// TODO: What is going on here??
//  https://github.com/google/googletest/issues/2328
#include <cxxabi.h>
__gnu_cxx::recursive_init_error::~recursive_init_error()
{
}
#endif

namespace Thunder {
namespace Core {

#ifdef __WINDOWS__
    uint32_t
    InterlockedIncrement(
        volatile uint32_t& a_Number)
    {
        return (_InterlockedIncrement(&a_Number));
    }

    uint32_t
    InterlockedDecrement(
        volatile uint32_t& a_Number)
    {
        return (_InterlockedDecrement(&a_Number));
    }

    uint32_t
    InterlockedIncrement(
        volatile int& a_Number)
    {
        return (_InterlockedIncrement(reinterpret_cast<volatile unsigned int*>(&a_Number)));
    }

    uint32_t
    InterlockedDecrement(
        volatile int& a_Number)
    {
        return (_InterlockedDecrement(reinterpret_cast<volatile unsigned int*>(&a_Number)));
    }

#else

    uint32_t
    InterlockedIncrement(
        volatile uint32_t& a_Number)
    {
        return (__sync_fetch_and_add(&a_Number, 1) + 1);
    }

    uint32_t
    InterlockedDecrement(
        volatile uint32_t& a_Number)
    {
        return (__sync_fetch_and_sub(&a_Number, 1) - 1);
    }

    uint32_t
    InterlockedIncrement(
        volatile int& a_Number)
    {
        return (__sync_fetch_and_add(&a_Number, 1) + 1);
    }

    uint32_t
    InterlockedDecrement(
        volatile int& a_Number)
    {
        return (__sync_fetch_and_sub(&a_Number, 1) - 1);
    }

#endif

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------
// CriticalSection class
//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
// CONSTRUCTOR & DESTRUCTOR
//----------------------------------------------------------------------------
#ifdef __WINDOWS__
    CriticalSection::CriticalSection()
    {
        TRACE_L5("Constructor CriticalSection <%p>", (this));

        ::InitializeCriticalSection(&m_syncMutex);
    }
#endif

#ifdef __POSIX__
    CriticalSection::CriticalSection()
    {
        TRACE_L5("Constructor CriticalSection <%p>", (this));

        pthread_mutexattr_t structAttributes;

        // Create a recursive mutex for this process (no named version, use semaphore)
        if (pthread_mutexattr_init(&structAttributes) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        } else if (pthread_mutexattr_settype(&structAttributes, PTHREAD_MUTEX_RECURSIVE) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        } else if (pthread_mutex_init(&m_syncMutex, &structAttributes) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }
#ifdef __CORE_CRITICAL_SECTION_LOG__
        memset(_UsedStackEntries, 0, sizeof(_UsedStackEntries));
        memset(_LockingStack, 0, sizeof(_LockingStack));
#endif // __CORE_CRITICAL_SECTION_LOG__
    }

#ifdef __CORE_CRITICAL_SECTION_LOG__
    void CriticalSection::TryLock()
    {
        // Wait time in seconds.
        const int nTimeSecs = 5;
        timespec structTime = {0,0};

        clock_gettime(CLOCK_MONOTONIC, &structTime);
        structTime.tv_sec += nTimeSecs;

        // MF2018 please note: sem_timedwait is not compatible with CLOCK_MONOTONIC.
        int result = pthread_mutex_timedlock(&m_syncMutex, &structTime);
        if (result != 0) {
            void* addresses[_AllocatedStackEntries];

            int addressCount = backtrace(addresses, _AllocatedStackEntries);

            // Remove top two frames because we are not interested in Lock+TryLock.
            addressCount = StripStackTop(addresses, addressCount, 2);

            // Lock mutex guarding stderr so no other critical section can dump its deadlock info
            _StdErrDumpMutex.Lock();

            TRACE_L1("Issue on process: <%d>", Core::ProcessInfo().Id());
            TRACE_L1("Probably creating a deadlock situation. <%d>", result);

            fprintf(stderr, "Failing lock:\n");
            backtrace_symbols_fd(addresses, addressCount, fileno(stderr));

            // Only print last entry, use debugger to read all entries to find unmatched lock
            int stackArrayIndex = m_syncMutex.__data.__count - 1;
            ASSERT(stackArrayIndex >= 0);

            fprintf(stderr, "\nLocked location:\n");
            backtrace_symbols_fd(_LockingStack[stackArrayIndex], _UsedStackEntries[stackArrayIndex], fileno(stderr));

            #if defined(THUNDER_BACKTRACE)
            fprintf(stderr, "\nCurrent stack of locking thread:\n");
            addressCount = ::GetCallStack(_LockingThread, addresses, _AllocatedStackEntries);
            backtrace_symbols_fd(addresses, _AllocatedStackEntries, fileno(stderr));
            #endif

            _StdErrDumpMutex.Unlock();

            if (result == ETIMEDOUT && ((result = pthread_mutex_lock(&m_syncMutex)) != 0)) {
                TRACE_L1("After detection, continued to wait. Wait failed with error: <%d>", result);
            }
        } else {
            _LockingThread = pthread_self();

            int stackArrayIndex = m_syncMutex.__data.__count - 1;
            if (stackArrayIndex < _AllocatedStacks) {
                _UsedStackEntries[stackArrayIndex] = backtrace(_LockingStack[stackArrayIndex], _AllocatedStackEntries);

                // Remove top two frames because we are not interested in Lock+TryLock.
                _UsedStackEntries[stackArrayIndex] = StripStackTop(_LockingStack[stackArrayIndex], _UsedStackEntries[0], 2);
            }
        }
    }

    int CriticalSection::StripStackTop(void** stack, int stackEntries, int stripped)
    {
        int newEntryCount = stackEntries - stripped;
        if (newEntryCount < 0) {
            newEntryCount = 0;
            stripped = stackEntries;
        }

        memmove(stack, stack + stripped, sizeof(void*) * newEntryCount);

        // Set rest of buffer to zeroes.
        memset(stack + newEntryCount, 0, sizeof(void*) * stripped);

        return newEntryCount;
    }
#endif // __CORE_CRITICAL_SECTION_LOG__

#endif

    CriticalSection::~CriticalSection()
    {
        TRACE_L5("Destructor CriticalSection <%p>", (this));

#ifdef __POSIX__
        int result = pthread_mutex_destroy(&m_syncMutex);
        if (result != 0) {
            TRACE_L1("Probably trying to delete a used CriticalSection <%d>.", result);
        }
#endif
#ifdef __WINDOWS__
        ::DeleteCriticalSection(&m_syncMutex);
#endif
    }

    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------
    // BinarySemaphore class
    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------

    //----------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    //----------------------------------------------------------------------------

    // This constructor is to be compatible with the WIN32 CSemaphore class which
    // sets the inital count an the maximum count. This way, on platform changes,
    // only the declaration/definition of the synchronisation object has to be defined
    // as being Binairy, not the coding.
    BinarySemaphore::BinarySemaphore(unsigned int nInitialCount, unsigned int nMaxCount)
    {
        DEBUG_VARIABLE(nMaxCount);
        ASSERT((nInitialCount == 0) || (nInitialCount == 1));

        TRACE_L5("Constructor BinarySemaphore (int, int)  <%p>", (this));

#ifdef __POSIX__
        m_blLocked = (nInitialCount == 0);

        pthread_condattr_t attr;

        if (0 != pthread_condattr_init(&attr)) {
            ASSERT(false);
        }

#ifndef __APPLE__
        if (0 != pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
            ASSERT(false);
        }
#endif

        if (pthread_mutex_init(&m_syncAdminLock, nullptr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }

        if (pthread_cond_init(&m_syncCondition, &attr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }
#endif

#ifdef __WINDOWS__
        m_syncMutex = ::CreateMutex(nullptr, (nInitialCount == 0), nullptr);

        ASSERT(m_syncMutex != nullptr);
#endif
    }

    BinarySemaphore::BinarySemaphore(bool blLocked)
#ifdef __POSIX__
        : m_blLocked(blLocked)
#endif
    {
        TRACE_L5("Constructor BinarySemaphore <%p>", (this));

#ifdef __POSIX__
        pthread_condattr_t attr;

        if (0 != pthread_condattr_init(&attr)) {
            ASSERT(false);
        }

#ifndef __APPLE__
        if (0 != pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
            ASSERT(false);
        }
#endif
        if (pthread_mutex_init(&m_syncAdminLock, nullptr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }

        if (pthread_cond_init(&m_syncCondition, &attr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }
#endif

#ifdef __WINDOWS__
        m_syncMutex = ::CreateMutex(nullptr, blLocked, nullptr);

        ASSERT(m_syncMutex != nullptr);
#endif
    }

    BinarySemaphore::~BinarySemaphore()
    {
        TRACE_L5("Destructor BinarySemaphore <%p>", (this));

#ifdef __POSIX__
        // If we really create it, we really have to destroy it.
        pthread_mutex_destroy(&m_syncAdminLock);
        pthread_cond_destroy(&m_syncCondition);
#endif

#ifdef __WINDOWS__
        ::CloseHandle(m_syncMutex);
#endif
    }

    //----------------------------------------------------------------------------
    // PUBLIC METHODS
    //----------------------------------------------------------------------------

    uint32_t
    BinarySemaphore::Lock()
    {

#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncMutex, Core::infinite, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_GENERAL);
#else
        int nResult = Core::ERROR_NONE;

        // See if we can check the state.
        pthread_mutex_lock(&m_syncAdminLock);

        // We are not busy Setting the flag, so we can check it.
        if (m_blLocked != false) {
            do {
                // Oops it seems that we are not allowed to pass.
                nResult = pthread_cond_wait(&m_syncCondition, &m_syncAdminLock);

                if (nResult != 0) {
                    // Something went wrong, so assume...
                    TRACE_L5("Error waiting for event <%d>.", nResult);
                    nResult = Core::ERROR_GENERAL;
                }

                // For some reason the documentation says that we have to double check on
                // the condition variable to see if we are allowed to fall through, so we
                // do (Guide to DEC threads, March 1996 ,page pthread-56, paragraph 4)
            } while ((m_blLocked == true) && (nResult == Core::ERROR_NONE));
        }

        if (nResult == Core::ERROR_NONE) {
            // Seems like we have the token, So the object is locked now.
            m_blLocked = true;
        }

        // Done with the internals of the binairy semphore, everyone can access it again.
        pthread_mutex_unlock(&m_syncAdminLock);

        // Wait forever so...
        return (nResult);
#endif
    }

    uint32_t
    BinarySemaphore::Lock(unsigned int nTime)
    {
#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncMutex, nTime, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_TIMEDOUT);
#else
        uint32_t nResult = Core::ERROR_NONE;
        if (nTime == Core::infinite) {
            return (Lock());
        } else {
            // See if we can check the state.
            pthread_mutex_lock(&m_syncAdminLock);

            // We are not busy Setting the flag, so we can check it.
            if (m_blLocked == true) {
                struct timespec structTime;

#ifdef __APPLE__
                clock_gettime(CLOCK_REALTIME, &structTime);
#elif defined(__LINUX__)
                clock_gettime(CLOCK_MONOTONIC, &structTime);
#endif
                structTime.tv_nsec += ((nTime % 1000) * 1000 * 1000); /* remainder, milliseconds to nanoseconds */
                structTime.tv_sec += (nTime / 1000) + (structTime.tv_nsec / 1000000000); /* milliseconds to seconds */
                structTime.tv_nsec = structTime.tv_nsec % 1000000000;

                do {
                    // Oops it seems that we are not allowed to pass.
                    nResult = pthread_cond_timedwait(&m_syncCondition, &m_syncAdminLock, &structTime);

                    if (nResult == ETIMEDOUT) {
                        // Som/ething went wrong, so assume...
                        TRACE_L5("Timed out waiting for event <%d>.", nTime);
                        nResult = Core::ERROR_TIMEDOUT;
                    } else if (nResult != 0) {
                        // Something went wrong, so assume...
                        TRACE_L5("Waiting on semaphore failed. Error code <%d>", nResult);
                        nResult = Core::ERROR_GENERAL;
                    }

                    // For some reason the documentation says that we have to double check on
                    // the condition variable to see if we are allowed to fall through, so we
                    // do (Guide to DEC threads, March 1996 ,page pthread-56, paragraph 4)
                } while ((m_blLocked == true) && (nResult == Core::ERROR_NONE));
            }

            if (nResult == Core::ERROR_NONE) {
                // Seems like we have the token, So the object is locked now.
                m_blLocked = true;
            }

            // Done with the internals of the binairy semphore, everyone can access it again.
            pthread_mutex_unlock(&m_syncAdminLock);
        }

        // Timed out or did we get the token ?
        return (nResult);
#endif
    }

    uint32_t
    BinarySemaphore::Unlock()
    {

#ifdef __POSIX__
        // See if we can get access to the data members of this object.
        pthread_mutex_lock(&m_syncAdminLock);

        // Yep, that's it we are no longer locked. Signal the change.
        m_blLocked = false;

        // O.K. that is arranged, Now we should at least signal the first
        // waiting process that is waiting for this condition to occur.
        pthread_cond_signal(&m_syncCondition);

        // Now that we are done with the variablegive other threads access
        // to the object again.
        pthread_mutex_unlock(&m_syncAdminLock);
#endif

#ifdef __WINDOWS__
        ::ReleaseMutex(m_syncMutex);
#endif
        return ERROR_NONE;
    }

    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------
    // CountingSemaphore class
    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------

    //----------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    //----------------------------------------------------------------------------

    CountingSemaphore::CountingSemaphore(
        unsigned int nInitialCount,
        unsigned int nMaxCount)
#ifdef __POSIX__
        : m_syncMinLimit(false)
        , m_syncMaxLimit(false)
        , m_nCounter(nInitialCount)
        , m_nMaxCount(nMaxCount)
#endif
    {
        TRACE_L5("Constructor CountingSemaphore <%p>", (this));

#ifdef __POSIX__

        if (pthread_mutex_init(&m_syncAdminLock, nullptr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }

        // Well that is it, see if one of the Limit locks should be taken ?
        if (m_nCounter == 0) {
            // This should be possible since we created them Not Locked.
            m_syncMinLimit.Lock();
        }

        // Or maybe we are at the upper limit ?
        if (m_nCounter == m_nMaxCount) {
            // This should be possible since we created them Not Locked.
            m_syncMaxLimit.Lock();
        }
#endif

#ifdef __WINDOWS__
        m_syncSemaphore = ::CreateSemaphore(nullptr, nInitialCount, nMaxCount, nullptr);

        ASSERT(m_syncSemaphore != nullptr);
#endif
    }

    CountingSemaphore::~CountingSemaphore()
    {
        TRACE_L5("Destructor CountingSemaphore <%p>", (this));

#ifdef __POSIX__
        // O.K. Destroy all the semaphores used by this class.
        pthread_mutex_destroy(&m_syncAdminLock);
#endif

#ifdef __WINDOWS__
        ::CloseHandle(m_syncSemaphore);
#endif
    }

    //----------------------------------------------------------------------------
    // PUBLIC METHODS
    //----------------------------------------------------------------------------

    uint32_t
    CountingSemaphore::Lock()
    {
#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncSemaphore, Core::infinite, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_GENERAL);
#else
        // First see if we could still decrease the count..
        uint32_t nResult = m_syncMinLimit.Lock();

        // See that we were able to get the semaphore.
        if (nResult == Core::ERROR_NONE) {
            // If we have this semaphore, no Other lock can take place
            // now make sure that the counter is handled atomic. Get the
            // administration lock (unlock can still access it).
            pthread_mutex_lock(&m_syncAdminLock);

            // Now we are in the clear, Lock cannot access this (blocked
            // on MinLimit) and unlock cannot access the counter (blocked on
            // m_syncAdminLock). Work the Semaphore counter. It's safe.

            // If we leave the absolute max position, make sure we release
            // the MaxLimit synchronisation.
            if (m_nCounter == m_nMaxCount) {
                m_syncMaxLimit.Unlock();
            }

            // Now update the counter.
            m_nCounter--;

            // See if the counter can still be decreased.
            if (m_nCounter != 0) {
                m_syncMinLimit.Unlock();
            }

            // Now we are completely done with the counter and it's logic. Free all
            // waiting threads for this resource.
            pthread_mutex_unlock(&m_syncAdminLock);
        }

        return (nResult);
#endif
    }

    uint32_t
    CountingSemaphore::Lock(unsigned int nMilliSeconds)
    {
#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncSemaphore, nMilliSeconds, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_TIMEDOUT);
#else
        // First see if we could still decrease the count..
        uint32_t nResult = m_syncMinLimit.Lock(nMilliSeconds);

        // See that we were able to get the semaphore.
        if (nResult == Core::ERROR_NONE) {
            // If we have this semaphore, no Other lock can take place
            // now make sure that the counter is handled atomic. Get the
            // administration lock (unlock can still access it).
            pthread_mutex_lock(&m_syncAdminLock);

            // Now we are in the clear, Lock cannot access this (blocked
            // on MinLimit) and unlock cannot access the counter (blocked on
            // m_syncAdminLock). Work the Semaphore counter. It's safe.

            // If we leave the absolute max position, make sure we release
            // the MaxLimit synchronisation.
            if (m_nCounter == m_nMaxCount) {
                m_syncMaxLimit.Unlock();
            }

            // Now update the counter.
            m_nCounter--;

            // See if the counter can still be decreased.
            if (m_nCounter != 0) {
                m_syncMinLimit.Unlock();
            }

            // Now we are completely done with the counter and it's logic. Free all
            // waiting threads for this resource.
            pthread_mutex_unlock(&m_syncAdminLock);
        }

        return (nResult);
#endif
    }

    uint32_t
    CountingSemaphore::Unlock(unsigned int nCount)
    {
        ASSERT(nCount != 0);

#ifdef __POSIX__
        // First see if we could still increase the count..
        uint32_t nResult = m_syncMaxLimit.Lock(Core::infinite);

        // See that we were able to get the semaphore.
        if (nResult == Core::ERROR_NONE) {
            // If we have this semaphore, no Other lock can take place
            // now make sure that the counter is handled atomic. Get the
            // administration lock (unlock can still access it).
            pthread_mutex_lock(&m_syncAdminLock);

            // Now we are in the clear, Unlock cannot access this (blocked on
            // MaxLimit) and Lock cannot access the counter (blocked on
            // m_syncAdminLock). Work the Semaphore counter. It's safe.

            // If we leave the absolute min position (0), make sure we signal
            // the Lock proCess, give the MinLimit synchronisation free.
            if (m_nCounter == 0) {
                m_syncMinLimit.Unlock();
            }

            // See if the given count
            m_nCounter += nCount;

            // See if we reached or overshot the max ?
            if (m_nCounter > m_nMaxCount) {
                // Release the Admin Semephore so the Lock on the max limit
                //  can proceed.
                pthread_mutex_unlock(&m_syncAdminLock);

                // Seems like we added more than allowed, so wait till the Max
                // mutex get's unlocked by the Lock process.
                m_syncMaxLimit.Lock(Core::infinite);

                // Before we continue processing, Get the administrative lock
                // again.
                pthread_mutex_lock(&m_syncAdminLock);
            }

            // See if we are still allowed to increase the counter.
            if (m_nCounter != m_nMaxCount) {
                m_syncMaxLimit.Unlock();
            }

            // Now we are completely done with the counter and it's logic. Free all
            // waiting threads for this resource.
            pthread_mutex_unlock(&m_syncAdminLock);
        }
#endif

#ifdef __WINDOWS__
        uint32_t nResult = Core::ERROR_NONE;

        if (::ReleaseSemaphore(m_syncSemaphore, nCount, nullptr) == FALSE) {
            // Could not give all tokens.
            ASSERT(false);
        }
#endif

        return (nResult);
    }

#ifdef __POSIX__
    uint32_t CountingSemaphore::TryUnlock(unsigned int nMilliSeconds)
#else
    uint32_t CountingSemaphore::TryUnlock(unsigned int /* nMilliSeconds */)
#endif
    {
#ifdef __POSIX__
        // First see if we could still increase the count..
        uint32_t nResult = m_syncMaxLimit.Lock(nMilliSeconds);

        // See that we were able to get the semaphore.
        if (nResult == Core::ERROR_NONE) {
            // If we have this semaphore, no Other lock can take place
            // now make sure that the counter is handled atomic. Get the
            // administration lock (unlock can still access it).
            pthread_mutex_lock(&m_syncAdminLock);

            // Now we are in the clear, Unlock cannot access this (blocked on
            // MaxLimit) and Lock cannot access the counter (blocked on
            // m_syncAdminLock). Work the Semaphore counter. It's safe.

            // If we leave the absolute min position (0), make sure we signal
            // the Lock process, give the MinLimit synchronisation free.
            if (m_nCounter == 0) {
                m_syncMinLimit.Unlock();
            }

            // Now update the counter.
            m_nCounter++;

            // See if we are still allowed to increase the counter.
            if (m_nCounter != m_nMaxCount) {
                m_syncMaxLimit.Unlock();
            }

            // Now we are completely done with the counter and it's logic. Free all
            // waiting threads for this resource.
            pthread_mutex_unlock(&m_syncAdminLock);
        }
#endif

#ifdef __WINDOWS__
        uint32_t nResult = Core::ERROR_NONE;

        if (::ReleaseSemaphore(m_syncSemaphore, 1, nullptr) == FALSE) {
            // Wait for the given time to see if we can "give" the lock.
            // To be Coded.
            ASSERT(false);
        }

#endif

        return (nResult);
    }

    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------
    // SharedSemaphore class
    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------

    //----------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    //----------------------------------------------------------------------------

#ifdef __WINDOWS__
    class WindowsAPI {
    public:
        WindowsAPI(WindowsAPI&&) = delete;
        WindowsAPI(const WindowsAPI&) = delete;
        WindowsAPI& operator=(WindowsAPI&&) = delete;
        WindowsAPI& operator=(const WindowsAPI&) = delete;

        ~WindowsAPI() = default;
        WindowsAPI() {
            _ntQuerySemaphore = reinterpret_cast<_NTQuerySemaphore>(GetProcAddress(GetModuleHandle(_T("ntdll.dll")), "NtQuerySemaphore"));
            ASSERT (_ntQuerySemaphore != nullptr);
        }

        uint32_t GetSemaphoreCount(HANDLE parameter) const {
            SEMAPHORE_BASIC_INFORMATION basicInfo;
            NTSTATUS status;
            status = _ntQuerySemaphore(parameter, 0, &basicInfo, sizeof(SEMAPHORE_BASIC_INFORMATION), nullptr);
            return (status == ERROR_SUCCESS) ? basicInfo.CurrentCount : 0;
        }
    private:
        _NTQuerySemaphore _ntQuerySemaphore;
    };

    static WindowsAPI _windowsAPI;
#endif

    SharedSemaphore::SharedSemaphore(const TCHAR sourceName[], const uint32_t initCount, VARIABLE_IS_NOT_USED const uint32_t maxCount)
    {
        ASSERT(initCount <= 1);
        ASSERT(maxCount == 1);
#ifdef __WINDOWS__
        _semaphore = (::CreateSemaphore(nullptr, initCount, maxCount, sourceName));
        ASSERT(_semaphore != nullptr);
#else
        _name = "/" + string(sourceName);
        _semaphore = sem_open(_name.c_str(), O_CREAT | O_RDWR | O_EXCL, 0644,  initCount);
        ASSERT(_semaphore != SEM_FAILED);
#endif
    }

    SharedSemaphore::SharedSemaphore(const TCHAR sourceName[])
    {
#ifdef __WINDOWS__
        _semaphore = ::OpenSemaphore(SEMAPHORE_ALL_ACCESS, FALSE, sourceName);
        ASSERT(_semaphore != nullptr);
#else
        _semaphore = sem_open(sourceName, 0);
        ASSERT(_semaphore != nullptr);
#endif
    }

#ifndef __WINDOWS__
    SharedSemaphore::SharedSemaphore(void* storage, const uint32_t initCount, VARIABLE_IS_NOT_USED const uint32_t maxCount)
        : _semaphore(storage), _name("")
    {
        ASSERT(storage != nullptr);
        ASSERT(initCount <= 1);
        ASSERT(maxCount == 1);
        memset(_semaphore, 0, sizeof(sem_t));
        VARIABLE_IS_NOT_USED int result = sem_init(static_cast<sem_t*>(_semaphore), 1, initCount); 
        ASSERT(result != -1);
    }

    SharedSemaphore::SharedSemaphore(void* storage) 
    : _semaphore(storage), _name("")
    {
        ASSERT(storage != nullptr);
    }
    
#endif

    SharedSemaphore::~SharedSemaphore()
    {
#ifdef __WINDOWS__
        if (_semaphore != nullptr) {
            ::CloseHandle(_semaphore);
        }
#else   
        if(_name.size() != 0) {
            sem_close(static_cast<sem_t*>(_semaphore));
            sem_unlink(_name.c_str());
        }
        else { 
            sem_destroy(static_cast<sem_t*>(_semaphore));
        }
#endif
    }

    size_t SharedSemaphore::Size()
    {
#ifdef __WINDOWS__
        return sizeof(HANDLE);
#else
        return sizeof(sem_t);
#endif
    }

    uint32_t SharedSemaphore::MaxCount() const
    {
        // Currently max count is 1, as it is implemented as a binary shared semaphore
        return 1;
    }

    //----------------------------------------------------------------------------
    // PUBLIC METHODS
    //----------------------------------------------------------------------------

    uint32_t 
    SharedSemaphore::Unlock()
    {
#ifdef __WINDOWS__
        if (_semaphore != nullptr) {
            BOOL result = ::ReleaseSemaphore(_semaphore, 1, nullptr);
            ASSERT(result != FALSE);
        }
#else
        VARIABLE_IS_NOT_USED int result = sem_post(static_cast<sem_t*>(_semaphore));
        ASSERT((result == 0) || (errno == EOVERFLOW));
#endif
        return ERROR_NONE;
    }

    uint32_t 
    SharedSemaphore::Count() const
    {
#ifdef __WINDOWS__
        return (_windowsAPI.GetSemaphoreCount(_semaphore));
#else
        int semValue = 0;
        sem_getvalue(static_cast<sem_t*>(_semaphore), &semValue);
        return semValue;
#endif
    }

    uint32_t 
    SharedSemaphore::Lock(const uint32_t waitTime)
    {
        uint32_t result = Core::ERROR_GENERAL;
#ifdef __WINDOWS__
        if (_semaphore != nullptr) {
            return (::WaitForSingleObjectEx(_semaphore, waitTime, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_TIMEDOUT);
        }
#elif defined(__APPLE__)

        uint32_t timeLeft = waitTime;
        int semResult;
        while (((semResult = sem_trywait(static_cast<sem_t*>(_semaphore))) != 0) && timeLeft > 0) {
            ::SleepMs(100);
            if (timeLeft != Core::infinite) {
                timeLeft -= (timeLeft > 100 ? 100 : timeLeft);
            }
        }
        result = semResult == 0 ? Core::ERROR_NONE : Core::ERROR_TIMEDOUT;

#elif defined(__MUSL__) || defined(__UCLIBC__)
        struct timespec referenceTime = {0,0};
        clock_gettime(CLOCK_MONOTONIC, &referenceTime);
        referenceTime.tv_nsec += ((waitTime % 1000) * 1000 * 1000); /* remainder, milliseconds to nanoseconds */
        referenceTime.tv_sec += (waitTime / 1000) + (referenceTime.tv_nsec / 1000000000); /* milliseconds to seconds */
        referenceTime.tv_nsec = referenceTime.tv_nsec % 1000000000;
        do {
             
            struct timespec structTime = {0,0};
            clock_gettime(CLOCK_REALTIME, &structTime);
            structTime.tv_nsec += ((waitTime % 1000) * 1000 * 1000); /* remainder, milliseconds to nanoseconds */
            structTime.tv_sec += (waitTime / 1000) + (structTime.tv_nsec / 1000000000); /* milliseconds to seconds */
            structTime.tv_nsec = structTime.tv_nsec % 1000000000;

            if (sem_timedwait(static_cast<sem_t*>(_semaphore), &structTime) == 0) {
                result = Core::ERROR_NONE;
            }
            else if ( errno == EINTR ) {
                continue;
            }
            else if ( errno == ETIMEDOUT ) {
                struct timespec currentMonoTime;
                clock_gettime(CLOCK_MONOTONIC, &currentMonoTime);

                struct timespec jumpTime;
                jumpTime.tv_sec = currentMonoTime.tv_sec - referenceTime.tv_sec;

                if(jumpTime.tv_sec != 0) {
                    result = Core::ERROR_TIMEDOUT;
                    break;
                }

                if(referenceTime.tv_sec < currentMonoTime.tv_sec || (referenceTime.tv_sec == currentMonoTime.tv_sec && 
                   referenceTime.tv_nsec < currentMonoTime.tv_nsec))
                {
                    result = Core::ERROR_TIMEDOUT;
                    break;            
                }
            }
            else {
                ASSERT(false);
            }
            break;
        } while (true);
#else
        struct timespec structTime = {0,0};
        clock_gettime(CLOCK_MONOTONIC, &structTime);
        structTime.tv_nsec += ((waitTime % 1000) * 1000 * 1000); /* remainder, milliseconds to nanoseconds */
        structTime.tv_sec += (waitTime / 1000) + (structTime.tv_nsec / 1000000000); /* milliseconds to seconds */
        structTime.tv_nsec = structTime.tv_nsec % 1000000000;

        do {
            if (sem_clockwait(static_cast<sem_t*>(_semaphore), CLOCK_MONOTONIC, &structTime) == 0) {
                result = Core::ERROR_NONE;
            }
            else if ( errno == EINTR ) {
                continue;
            }
            else if ( errno == ETIMEDOUT ) {
                result = Core::ERROR_TIMEDOUT;
            }
            else {
                ASSERT(false);
            }
            break;
        } while (true);
#endif
        return (result);
    }

    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------
    // Event class (AVAILABLE WITHIN PROCESS SPACE)
    //----------------------------------------------------------------------------
    //----------------------------------------------------------------------------

    //----------------------------------------------------------------------------
    // CONSTRUCTOR & DESTRUCTOR
    //----------------------------------------------------------------------------

    Event::Event(bool blSet, bool blManualReset)
        : m_blManualReset(blManualReset)
#ifdef __POSIX__
        , m_blCondition(blSet)
#endif
    {
        TRACE_L5("Constructor Event <%p>", (this));

#ifdef __POSIX__
        pthread_condattr_t attr;

        if (0 != pthread_condattr_init(&attr)) {
            ASSERT(false);
        }
#ifndef __APPLE__
        if (0 != pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)) {
            ASSERT(false);
        }
#endif

        if (pthread_mutex_init(&m_syncAdminLock, nullptr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }

        if (pthread_cond_init(&m_syncCondition, &attr) != 0) {
            // That will be the day, if this fails...
            ASSERT(false);
        }
#endif

#ifdef __WINDOWS__
        m_syncEvent = ::CreateEvent(nullptr, TRUE, blSet, nullptr);

        ASSERT(m_syncEvent != nullptr);
#endif
    }

    Event::~Event()
    {

#ifdef __POSIX__
        TRACE_L5("Destructor Event <%p>", (this));

        // If we really create it, we really have to destroy it.
        pthread_mutex_destroy(&m_syncAdminLock);
        pthread_cond_destroy(&m_syncCondition);
#endif

#ifdef __WINDOWS__
        ::CloseHandle(m_syncEvent);
#endif
    }

    //----------------------------------------------------------------------------
    // PUBLIC METHODS
    //----------------------------------------------------------------------------

    uint32_t
    Event::Lock()
    {
#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncEvent, Core::infinite, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_GENERAL);
#else
        int nResult = Core::ERROR_NONE;
        // See if we can check the state.
        pthread_mutex_lock(&m_syncAdminLock);

        // We are not busy Setting the flag, so we can check it.
        if (m_blCondition == false) {
            do {
                // Oops it seems that we are not allowed to pass.
                nResult = (pthread_cond_wait(&m_syncCondition, &m_syncAdminLock) == 0 ? Core::ERROR_NONE : Core::ERROR_GENERAL);

                // For some reason the documentation says that we have to double check on
                // the condition variable to see if we are allowed to fall through, so we
                // do (Guide to DEC threads, March 1996 ,page pthread-56, paragraph 4)
            } while ((m_blCondition == false) && (nResult == Core::ERROR_NONE));

            if (nResult != 0) {
                // Something went wrong, so assume...
                TRACE_L5("Error waiting for event <%d>.", nResult);
            }
        }

        // Seems that the event is triggered, lets continue. but
        // do not forget to give back the flag..
        pthread_mutex_unlock(&m_syncAdminLock);

        // Wait forever so...
        return (nResult);
#endif
    }

    bool
    Event::IsSet() const
    {
#ifdef __POSIX__
        return (m_blCondition);
#endif

#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncEvent, 0, FALSE) == WAIT_OBJECT_0);
#endif
    }

    uint32_t
    Event::Lock(unsigned int nTime)
    {
#ifdef __WINDOWS__
        return (::WaitForSingleObjectEx(m_syncEvent, nTime, FALSE) == WAIT_OBJECT_0 ? Core::ERROR_NONE : Core::ERROR_TIMEDOUT);
#else
        if (nTime == Core::infinite) {
            return (Lock());
        } else {
            int nResult = Core::ERROR_NONE;

            // See if we can check the state.
            pthread_mutex_lock(&m_syncAdminLock);

            // We are not busy Setting the flag, so we can check it.
            if (m_blCondition == false) {
                struct timespec structTime;

#ifdef __APPLE__
                clock_gettime(CLOCK_REALTIME, &structTime);
#elif defined(__LINUX__)
                clock_gettime(CLOCK_MONOTONIC, &structTime);
#endif
                structTime.tv_nsec += ((nTime % 1000) * 1000 * 1000); /* remainder, milliseconds to nanoseconds */
                structTime.tv_sec += (nTime / 1000) + (structTime.tv_nsec / 1000000000); /* milliseconds to seconds */
                structTime.tv_nsec = structTime.tv_nsec % 1000000000;

                do {
                    // Oops it seems that we are not allowed to pass.
                    nResult = pthread_cond_timedwait(&m_syncCondition, &m_syncAdminLock, &structTime);

                    if (nResult == ETIMEDOUT) {
                        // Something went wrong, so assume...
                        TRACE_L5("Timed out waiting for event <%d>.", nTime);
                        nResult = Core::ERROR_TIMEDOUT;
                    } else if (nResult != 0) {
                        // Something went wrong, so assume...
                        TRACE_L5("Waiting on semaphore failed. Error code <%d>", nResult);
                        nResult = Core::ERROR_GENERAL;
                    }

                    // For some reason the documentation says that we have to double check on
                    // the condition variable to see if we are allowed to fall through, so we
                    // do (Guide to DEC threads, March 1996 ,page pthread-56, paragraph 4)
                } while ((m_blCondition == false) && (nResult == Core::ERROR_NONE));

               if (nResult != 0) {
                    // Something went wrong, so assume...
                    TRACE_L5("Timed out waiting for event <%d>!", nResult);
                }
            }
            // Seems that the event is triggered, lets continue. but
            // do not forget to give back the flag..
            pthread_mutex_unlock(&m_syncAdminLock);

            return (nResult);
        }
#endif
    }

    uint32_t
    Event::Unlock()
    {
        uint32_t nResult = Core::ERROR_NONE;

#ifdef __POSIX__
        // See if we can get access to the data members of this object.
        pthread_mutex_lock(&m_syncAdminLock);

        // Yep, that's it we are no longer locked. Signal the change.
        m_blCondition = true;

        // O.K. that is arranged, Now we should at least signal the first
        // waiting process that is waiting for this condition to occur.
        pthread_cond_signal(&m_syncCondition);

        // Now that we are done with the variablegive other threads access
        // to the object again.
        pthread_mutex_unlock(&m_syncAdminLock);
#endif

#ifdef __WINDOWS__
        ::SetEvent(m_syncEvent);
#endif

        return (nResult);
    }

    void
    Event::ResetEvent()
    {
#ifdef __POSIX__
        // See if we can check the state.
        pthread_mutex_lock(&m_syncAdminLock);

        // We are the onlyones who can access the data, time to update it.
        m_blCondition = false;

        // Done changing the data, free other threads so the can use this
        // object again
        pthread_mutex_unlock(&m_syncAdminLock);
#endif

#ifdef __WINDOWS__
        ::ResetEvent(m_syncEvent);
#endif
    }

    void
    Event::SetEvent()
    {
#ifdef __POSIX__
        // See if we can get access to the data members of this object.
        pthread_mutex_lock(&m_syncAdminLock);

        // Yep, that's it we are signalled, Broadcast the change.
        m_blCondition = true;

        // O.K. that is arranged, Now we should at least signal waiting
        // process that the event has occured.
        pthread_cond_broadcast(&m_syncCondition);

        // All waiting threads are now in the running mode again. See
        // if the event should be cleared manually again.
        if (m_blManualReset == false) {
            // Make sure all threads are in running mode, place our request
            // for sync at the end of the FIFO-queue for syncConditionMutex.
            pthread_mutex_unlock(&m_syncAdminLock);
            std::this_thread::yield();
            pthread_mutex_lock(&m_syncAdminLock);

            // They all had a change to continue so, now it is over, we can
            // not wait forever......
            m_blCondition = false;
        }

        // Now that we are done with the variablegive other threads access
        // to the object again.
        pthread_mutex_unlock(&m_syncAdminLock);
#endif

#ifdef __WINDOWS__
        if (m_blManualReset == true) {
            ::SetEvent(m_syncEvent);
        }
        else {
            ::PulseEvent(m_syncEvent);
        }
#endif
    }

#ifndef __WINDOWS__
#if defined(__CORE_CRITICAL_SECTION_LOG__)
    CriticalSection CriticalSection::_StdErrDumpMutex;
#endif
#endif

}
} // namespace Solution::Core
