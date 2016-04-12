/*
Copyright 2015 refractionPOINT

Licensed under the Apache License, Version 2.0 ( the "License" );
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http ://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <rpal/rpal.h>
#include <librpcm/librpcm.h>
#include "collectors.h"
#include <notificationsLib/notificationsLib.h>
#include <rpHostCommonPlatformLib/rTags.h>
#include <processLib/processLib.h>
#include <libOs/libOs.h>
#include <kernelAcquisitionLib/kernelAcquisitionLib.h>
#include <kernelAcquisitionLib/common.h>

#ifdef RPAL_PLATFORM_WINDOWS
    #include <windows_undocumented.h>
    #include <TlHelp32.h>
#elif defined( RPAL_PLATFORM_MACOSX )
    #include <sys/types.h>
    #include <sys/sysctl.h>
#endif

#define RPAL_FILE_ID       63

#define MAX_SNAPSHOT_SIZE 1536

typedef struct
{
    RU32 pid;
    RU32 ppid;
} processEntry;

static RBOOL g_is_kernel_failure = FALSE;  // Kernel acquisition failed for this method


static RBOOL
    getSnapshot
    (
        processEntry* toSnapshot,
        RU32* nElem
    )
{
    RBOOL isSuccess = FALSE;
    RU32 i = 0;

    if( NULL != toSnapshot )
    {
        rpal_memory_zero( toSnapshot, sizeof( processEntry ) * MAX_SNAPSHOT_SIZE );
    }

    if( NULL != toSnapshot &&
        NULL != nElem )
    {
#ifdef RPAL_PLATFORM_WINDOWS
        HANDLE hSnapshot = NULL;
        PROCESSENTRY32W procEntry = { 0 };
        procEntry.dwSize = sizeof( procEntry );

        if( INVALID_HANDLE_VALUE != ( hSnapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 ) ) )
        {
            if( Process32FirstW( hSnapshot, &procEntry ) )
            {
                isSuccess = TRUE;

                do
                {
                    if( 0 == procEntry.th32ProcessID )
                    {
                        continue;
                    }

                    toSnapshot[ i ].pid = procEntry.th32ProcessID;
                    toSnapshot[ i ].ppid = procEntry.th32ParentProcessID;
                    i++;
                } while( Process32NextW( hSnapshot, &procEntry ) &&
                         MAX_SNAPSHOT_SIZE > i );
            }

            CloseHandle( hSnapshot );
        }
#elif defined( RPAL_PLATFORM_LINUX )
        RWCHAR procDir[] = _WCH( "/proc/" );
        rDir hProcDir = NULL;
        rFileInfo finfo = {0};

        if( rDir_open( (RPWCHAR)&procDir, &hProcDir ) )
        {
            isSuccess = TRUE;

            while( rDir_next( hProcDir, &finfo ) &&
                   MAX_SNAPSHOT_SIZE > i )
            {
                if( rpal_string_wtoi( (RPWCHAR)finfo.fileName, &( toSnapshot[ i ].pid ) )
                    && 0 != toSnapshot[ i ].pid )
                {
                    i++;
                }
            }

            rDir_close( hProcDir );
        }
#elif defined( RPAL_PLATFORM_MACOSX )
        int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
        struct kinfo_proc* infos = NULL;
        size_t size = 0;
        int ret = 0;

        if( 0 == ( ret = sysctl( mib, ARRAY_N_ELEM( mib ), infos, &size, NULL, 0 ) ) )
        {
            if( NULL != ( infos = rpal_memory_alloc( size ) ) )
            {
                while( 0 != ( ret = sysctl( mib, ARRAY_N_ELEM( mib ), infos, &size, NULL, 0 ) ) && ENOMEM == errno )
                {
                    if( NULL == ( infos = rpal_memory_realloc( infos, size ) ) )
                    {
                        break;
                    }
                }
            }
        }

        if( 0 == ret && NULL != infos )
        {
            isSuccess = TRUE;
            size = size / sizeof( struct kinfo_proc );
            for( i = 0; i < size && MAX_SNAPSHOT_SIZE > i; i++ )
            {
                toSnapshot[ i ].pid = infos[ i ].kp_proc.p_pid;
                toSnapshot[ i ].ppid = infos[ i ].kp_eproc.e_ppid;
            }

            if( NULL != infos )
            {
                rpal_memory_free( infos );
                infos = NULL;
            }
        }
#endif

        rpal_sort_array( toSnapshot, 
                         i, 
                         sizeof( processEntry ), 
                         (rpal_ordering_func)rpal_order_RU32 );
        *nElem = i;
    }

    return isSuccess;
}

static RBOOL
    notifyOfProcess
    (
        RU32 pid,
        RU32 ppid,
        RBOOL isStarting,
        RNATIVESTR optFilePath,
        RNATIVESTR optCmdLine,
        RU32 optUserId,
        RU64 optTs
    )
{
    RBOOL isSuccess = FALSE;
    rSequence info = NULL;
    rSequence parentInfo = NULL;
    RU32 tmpUid = 0;

    // We prime the information with whatever was provided
    // to us by the kernel acquisition. If not available
    // we generate using the UM only way.
    if( 0 != rpal_string_strlenn( optFilePath ) &&
        ( NULL != info ||
          NULL != ( info = rSequence_new() ) ) )
    {
        rSequence_addSTRINGN( info, RP_TAGS_FILE_PATH, optFilePath );
    }

    if( 0 != rpal_string_strlenn( optCmdLine ) &&
        ( NULL != info ||
          NULL != ( info = rSequence_new() ) ) )
    {
        rSequence_addSTRINGN( info, RP_TAGS_FILE_PATH, optCmdLine );
    }

    if( NULL != info )
    {
        info = processLib_getProcessInfo( pid, info );
    }
    else if( !isStarting ||
             NULL == ( info = processLib_getProcessInfo( pid, NULL ) ) )
    {
        info = rSequence_new();
    }

    if( rpal_memory_isValid( info ) )
    {
        rSequence_addRU32( info, RP_TAGS_PROCESS_ID, pid );
        rSequence_addRU32( info, RP_TAGS_PARENT_PROCESS_ID, ppid );
        if( 0 != optTs )
        {
            rSequence_addTIMESTAMP( info, RP_TAGS_TIMESTAMP, rpal_time_getGlobalFromLocal( optTs ) );
        }
        else
        {
            rSequence_addTIMESTAMP( info, RP_TAGS_TIMESTAMP, rpal_time_getGlobal() );
        }

        if( isStarting )
        {
            if( NULL != ( parentInfo = processLib_getProcessInfo( ppid, NULL ) ) &&
                !rSequence_addSEQUENCE( info, RP_TAGS_PARENT, parentInfo ) )
            {
                rSequence_free( parentInfo );
            }
        }

        if( isStarting )
        {
            if( KERNEL_ACQ_NO_USER_ID != optUserId &&
                !rSequence_getRU32( info, RP_TAGS_USER_ID, &tmpUid ) )
            {
                rSequence_addRU32( info, RP_TAGS_USER_ID, optUserId );
            }

            if( notifications_publish( RP_TAGS_NOTIFICATION_NEW_PROCESS, info ) )
            {
                isSuccess = TRUE;
                rpal_debug_info( "new process starting: %d", pid );
            }
        }
        else
        {
            if( notifications_publish( RP_TAGS_NOTIFICATION_TERMINATE_PROCESS, info ) )
            {
                isSuccess = TRUE;
                rpal_debug_info( "new process terminating: %d", pid );
            }
        }

        rSequence_free( info );
    }

    return isSuccess;
}

static RVOID
    userModeDiff
    (
        rEvent isTimeToStop
    )
{
    processEntry snapshot_1[ MAX_SNAPSHOT_SIZE ] = { 0 };
    processEntry snapshot_2[ MAX_SNAPSHOT_SIZE ] = { 0 };
    processEntry* currentSnapshot = snapshot_1;
    processEntry* previousSnapshot = snapshot_2;
    processEntry* tmpSnapshot = NULL;
    RBOOL isFirstSnapshots = TRUE;
    RU32 i = 0;
    RBOOL isFound = FALSE;
    RU32 nThLoop = 0;
    RU32 nTmpElem = 0;
    RU32 nCurElem = 0;
    RU32 nPrevElem = 0;
    RU32 currentTimeout = 0;

    while( !rEvent_wait( isTimeToStop, currentTimeout ) &&
           ( !kAcq_isAvailable() ||
             g_is_kernel_failure ) )
    {
        tmpSnapshot = currentSnapshot;
        currentSnapshot = previousSnapshot;
        previousSnapshot = tmpSnapshot;

        nTmpElem = nCurElem;
        nCurElem = nPrevElem;
        nPrevElem = nTmpElem;

        if( getSnapshot( currentSnapshot, &nCurElem ) )
        {
            if( isFirstSnapshots )
            {
                isFirstSnapshots = FALSE;
                continue;
            }

            // Diff to find new processes
            for( i = 0; i < nCurElem; i++ )
            {
                isFound = FALSE;

                if( (RU32)( -1 ) != rpal_binsearch_array( previousSnapshot,
                                                          nPrevElem,
                                                          sizeof( processEntry ),
                                                          &(currentSnapshot[ i ].pid),
                                                          (rpal_ordering_func)rpal_order_RU32 ) )
                {
                    isFound = TRUE;
                }

                if( !isFound )
                {
                    if( !notifyOfProcess( currentSnapshot[ i ].pid,
                                          currentSnapshot[ i ].ppid,
                                          TRUE,
                                          NULL,
                                          NULL,
                                          KERNEL_ACQ_NO_USER_ID,
                                          0 ) )
                    {
                        rpal_debug_warning( "error reporting new process: %d",
                                            currentSnapshot[ i ].pid );
                    }
                }
            }

            // Diff to find terminated processes
            for( i = 0; i < nPrevElem; i++ )
            {
                isFound = FALSE;

                if( (RU32)( -1 ) != rpal_binsearch_array( currentSnapshot,
                                                          nCurElem,
                                                          sizeof( processEntry ),
                                                          &(previousSnapshot[ i ].pid),
                                                          (rpal_ordering_func)rpal_order_RU32 ) )
                {
                    isFound = TRUE;
                }

                if( !isFound )
                {
                    if( !notifyOfProcess( previousSnapshot[ i ].pid,
                                          previousSnapshot[ i ].ppid,
                                          FALSE,
                                          NULL,
                                          NULL,
                                          KERNEL_ACQ_NO_USER_ID,
                                          0 ) )
                    {
                        rpal_debug_warning( "error reporting terminated process: %d",
                                            previousSnapshot[ i ].pid );
                    }
                }
            }
        }

        nThLoop++;
        if( 0 == nThLoop % 20 )
        {
#ifdef RPAL_PLATFORM_WINDOWS
            // The Windows API is much more efficient than on Nix so we can affort
            // going faster between our diffs.
            currentTimeout = libOs_getUsageProportionalTimeout( 500 ) + 100;
#else
            currentTimeout = libOs_getUsageProportionalTimeout( 800 ) + 200;
#endif
        }
    }
}

static RVOID
    kernelModeDiff
    (
        rEvent isTimeToStop
    )
{
    RU32 i = 0;
    RU32 nScratch = 0;
    RU32 nProcessEntries = 0;
    KernelAcqProcess new_from_kernel[ 200 ] = { 0 };
    processEntry tracking_user[ MAX_SNAPSHOT_SIZE ] = { 0 };
    RU32 nPhase = 0;

    while( !rEvent_wait( isTimeToStop, 1000 ) )
    {
        nScratch = ARRAY_N_ELEM( new_from_kernel );
        rpal_memory_zero( new_from_kernel, sizeof( new_from_kernel ) );
        if( !kAcq_getNewProcesses( new_from_kernel, &nScratch ) )
        {
            rpal_debug_warning( "kernel acquisition for new processes failed" );
            g_is_kernel_failure = TRUE;
            break;
        }

        for( i = 0; i < nScratch; i++ )
        {
            notifyOfProcess( new_from_kernel[ i ].pid,
                             new_from_kernel[ i ].ppid,
                             TRUE,
                             new_from_kernel[ i ].path,
                             new_from_kernel[ i ].cmdline,
                             new_from_kernel[ i ].uid,
                             new_from_kernel[ i ].ts );

            if( nProcessEntries >= ARRAY_N_ELEM( tracking_user ) - 1 )
            {
                continue;
            }

            tracking_user[ nProcessEntries ].pid = new_from_kernel[ i ].pid;
            tracking_user[ nProcessEntries ].ppid = new_from_kernel[ i ].ppid;
            nProcessEntries++;
        }

        // We do garbage collection and checking of dead processes
        // only every X sweeps from the kernel, this is to prioritize
        // the timely processing of new processes vs dead ones
        nPhase++;
        if( nPhase < 10 )
        {
            continue;
        }

        for( i = 0; i < nProcessEntries; i++ )
        {
            if( !processLib_isPidInUse( tracking_user[ i ].pid ) )
            {
                notifyOfProcess( tracking_user[ i ].pid, 
                                 tracking_user[ i ].ppid, 
                                 FALSE, 
                                 NULL, 
                                 NULL,
                                 KERNEL_ACQ_NO_USER_ID,
                                 0 );
                if( nProcessEntries != i + 1 )
                {
                    rpal_memory_memmove( &(tracking_user[ i ]), &(tracking_user[ i + 1 ]), nProcessEntries - i + 1 );
                }
                nProcessEntries--;
            }
        }
    }
}

static RPVOID
    processDiffThread
    (
        rEvent isTimeToStop,
        RPVOID ctx
    )
{
    UNREFERENCED_PARAMETER( ctx );

    while( !rEvent_wait( isTimeToStop, 0 ) )
    {
        if( kAcq_isAvailable() &&
            !g_is_kernel_failure )
        {
            // We first attempt to get new processes through
            // the kernel mode acquisition driver
            rpal_debug_info( "running kernel acquisition process notification" );
            kernelModeDiff( isTimeToStop );
        }
        // If the kernel mode fails, or is not available, try
        // to revert to user mode
        else if( !rEvent_wait( isTimeToStop, 0 ) )
        {
            rpal_debug_info( "running usermode acquisition process notification" );
            userModeDiff( isTimeToStop );
        }
    }

    return NULL;
}

//=============================================================================
// COLLECTOR INTERFACE
//=============================================================================

rpcm_tag collector_1_events[] = { RP_TAGS_NOTIFICATION_NEW_PROCESS,
                                  RP_TAGS_NOTIFICATION_TERMINATE_PROCESS,
                                  0 };

RBOOL
    collector_1_init
    (
        HbsState* hbsState,
        rSequence config
    )
{
    RBOOL isSuccess = FALSE;
    UNREFERENCED_PARAMETER( config );

    if( NULL != hbsState )
    {
        g_is_kernel_failure = FALSE;

        if( rThreadPool_task( hbsState->hThreadPool, processDiffThread, NULL ) )
        {
            isSuccess = TRUE;
        }
    }

    return isSuccess;
}

RBOOL
    collector_1_cleanup
    (
        HbsState* hbsState,
        rSequence config
    )
{
    RBOOL isSuccess = FALSE;

    if( NULL != hbsState &&
        rpal_memory_isValid( config ) )
    {
        isSuccess = TRUE;
    }

    return isSuccess;
}