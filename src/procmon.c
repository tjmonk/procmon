/*==============================================================================
MIT License

Copyright (c) 2023 Trevor Monk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
==============================================================================*/

/*!
 * @defgroup procmon procmon
 * @brief Process Monitor
 * @{
 */

/*============================================================================*/
/*!
@file procmon.c

    Process Monitor

    The procmon application is a process monitor which can be used as a
    single point of startup for a system, or as a service which receives
    notifications from processes which wish to be monitored or both.
    It is also capable of monitoring itself.

    If a monitored process exits for any reason, procmon can restart
    it (after an optional delay) and/or perform cleanup tasks before
    restarting it.


*/
/*============================================================================*/

/*==============================================================================
        Includes
==============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <tjson/json.h>
#include <sys/wait.h>

/*==============================================================================
       Type Definitions
==============================================================================*/

typedef enum _procState
{
    PROCSTATE_eINIT = 0,
    PROCSTATE_eSTARTED = 1,
    PROCSTATE_eRUNNING = 2,
    PROCSTATE_eTERMINATED = 3,
    PROCSTATE_eWAITING = 4
} ProcState;

/*! the LockData object contains the runtime
 * state of a process and is used to detect process death
 * and to terminate the process on demand */
typedef struct _lockData
{
    /*! current process ID of the monitored process */
    pid_t pid;

    /*! terminate command used to force the process to exit */
    uint32_t terminate;

    /*! process run counter */
    size_t runcount;

    /*! last process start time */
    time_t starttime;

} LockData;

/*! the Process structure defines a process to be monitored */
typedef struct _process
{
    /*! name or unique identifier for the process */
    char *id;

    /*! current operating state of the process */
    ProcState state;

    /*! pointer to the command line associated with this process */
    char *exec;

    /*! length of time (in seconds) to wait after starting the process
     * before starting any of its dependencies */
    int wait;

    /*! length of time we should wait before restarting the process
     * after it has died */
    int restart_delay;

    /*! indicate that this process should be restarted if its parent restarts */
    bool restart_on_parent_death;

    /*! indicates if this process is monitored or runs-to-exit */
    bool monitored;

    /*! enable verbose output for process management */
    bool verbose;

    /*! indicates that this process should be skipped and not started */
    bool skip;

    /*! pid of the process */
    pid_t pid;

    /*! number of times the process has been started */
    size_t runcount;

    /*! reference to the process' monitoring thread */
    pthread_t thread;

    /*! list of the processes dependencies, using during config file parsing */
    JArray *pDepends;

    /*! pointer to a list of the process' parents */
    struct _procNode *pParents;

    /*! pointer to a list of the process' dependent processes */
    struct _procNode *pChildren;

} Process;

/*! The ProcessNode structure is used to chain Process objects
 * together in a list */
typedef struct _procNode
{
    /*! pointer to the process object */
    Process *pProcess;

    /*! pointer to the next process object in the list */
    struct _procNode *pNext;
} ProcessNode;

/*! the ProcmonState object contains the operating state of the
 *  process monitor, and stores configuration data read from
 *  the command line inputs */
typedef struct _procmonState
{
    /*! verbose output setting */
    bool verbose;

    /*! pointer to the process monitor name: argV[0] */
    char *argv0;

    /*! pointer to the configuration file name */
    char *configFile;

    /*! pointer to the requested output format */
    char *outputFormat;

    /*! indicate primary or secondary (monitoring) process monitor */
    bool primary;

    /*! pointer to the process monitor process information */
    Process *pProcess;

    /*! pointer to the monitored process information */
    Process *pMonitoredProcess;

    /*! pointer to the first ProcessNode in the process configuration list */
    ProcessNode *pFirst;

    /*! pointer to the last ProcessNode in the process configuration list */
    ProcessNode *pLast;

} ProcmonState;

/*==============================================================================
       Function declarations
==============================================================================*/
static int makelock( Process *pProcess );
static int waitlock( int fd );
static int lock( int fd, int cmd );
static int unlock( int fd );

static int open_lockfile( char *name );
static int create_lockfile( Process *pProcess );
static int remove_lockfile( char *name );
static pid_t get_pid_from_lockfile( char *name );

static void Monitor( char *name, int fd );


static void usage( char *cmdname );
static int ProcessOptions( int argC,
                           char *argV[],
                           ProcmonState *pProcmonState );

static int ProcessConfigFile( ProcmonState *pProcmonState );

static int SetupProcess( JNode *pNode, void *arg );

Process *FindProcess( char *id, ProcmonState *pProcmonState );

static int BuildDependencyLists( ProcmonState *pProcmonState );

static int AddParents( ProcmonState *pProcmonState, Process *pProcess );

static int AddChild( Process *pParent, Process *pChild );

static int AddParent( Process *pChild, Process *pParent );

static int DisplayConfig( ProcmonState *pProcmonState );

static int DisplayProcess( Process *pProcess );

static void DisplayProcessIds( ProcessNode *pProcessNode );

static int DisplayProcessId( Process *pProcess );

static int DisplayState( Process *pProcess );

static int RunProcesses( ProcmonState *pProcmonState );

static int Runnable( Process *pProcess );

static int Run( Process *pProcess );

static void SetupTerminationHandler( void );

static void TerminationHandler( int signum, siginfo_t *info, void *ptr );

static int InitProcess( Process *pProcess );
static void WaitProcess( Process *pProcess );
static int RunProcess( Process *pProcess );

static void *MonitorThread( void *arg );

static size_t GetParentRuncount( Process *pProcess );

static int terminate( char *name );
static int terminate_and_stop_monitoring( char *name );
static int terminate_command( char *name, uint32_t cmd );
static int start( char *name );
static int restart( char *name );
static int ResetStartTime( int fd );

static int ListProcesses( ProcmonState *pProcmonState );
static int ShutdownAllProcesses( ProcmonState *pProcmonState );

static int DisplayProcessInfo( ProcmonState *pProcmonState, char *name, int n );
static int GetProcessTime( long runtime, char *buf, size_t len );

static int IncrementRestartCount( char *name );

static int RestartDependents( Process *pProcess );
static int RestartDependent( Process *pProcess, int wait );

static int MonitorProcmon( ProcmonState *pProcmonState );

static int MakeOwnLock( ProcmonState *pProcmonState );

int main( int argC, char *argV[] );

/*==============================================================================
       Definitions
==============================================================================*/

#ifndef EOK
/*! success result */
#define EOK 0
#endif

/*==============================================================================
       File Scoped Variables
==============================================================================*/

/*! pointer to the state machine context */
ProcmonState *pProcmonState;

char *ProcessStates[] =
{
    "INIT", "STARTED", "RUNNING", "TERMINATED", "WAITING"
};

/*==============================================================================
       Function definitions
==============================================================================*/

/*============================================================================*/
/*  main                                                                      */
/*!
    Main entry point for the process monitor application

    @param[in]
        argc
            number of arguments on the command line
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @return none

==============================================================================*/
int main( int argC, char *argV[] )
{
    pProcmonState = NULL;

    /* create the state machine instance */
    pProcmonState = (ProcmonState *)calloc(1, sizeof( ProcmonState ) );
    if ( pProcmonState != NULL )
    {
        /* Process Options */
        ProcessOptions( argC, argV, pProcmonState );

        if ( pProcmonState->configFile != NULL )
        {
            /* create a lockfile used to monitor the running status
             * of the process monitor */
            MakeOwnLock(pProcmonState);

            /* start/monitor either the primary or secondary process monitor */
            MonitorProcmon(pProcmonState);

            if( pProcmonState->primary )
            {
                if ( pProcmonState->verbose == true )
                {
                    printf("Processing the config file \n");
                }
                
                /* the primary process monitor processes the config file */
                ProcessConfigFile( pProcmonState );
            }

            /* let our threads do all the work while we take a nap */
            while( 1 )
            {
                sleep(10);
            }
        }
    }

    return 0;
}

/*============================================================================*/
/*  usage                                                                     */
/*!
    Display the process monitor usage

    The usage function dumps the application usage message
    to stderr.

    @param[in]
       cmdname
            pointer to the invoked command name

    @return none

==============================================================================*/
static void usage( char *cmdname )
{
    if( cmdname != NULL )
    {
        fprintf(stderr,
                "usage: %s [-v] [-h] [-l] [-x]"
                " [-s <proc>] [-r <proc>] [-k <proc>] [-d <proc>] [-o <fmt>]"
                " [-f|F <filename>]\n"
                " [-h] : display this help\n"
                " [-l] : list all the monitored processes\n"
                " [-o fmt] : list the monitored processes using fmt. eg json\n"
                " [-x] : remove all monitored processes\n"
                " [-k] : kill process and suspend monitoring\n"
                " [-r] : restart process\n"
                " [-s] : start monitoring a previously stopped process\n"
                " [-d] : stop processs and delete monitoring\n"
                " [-v] : verbose output\n"
                " [-f|F <filename>] : start processes as per configuration\n",
                cmdname );
    }
}

/*============================================================================*/
/*  ProcessOptions                                                            */
/*!
    Process the command line options

    The ProcessOptions function processes the command line options and
    populates the ProcmonState object

    @param[in]
        argC
            number of arguments
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @param[in]
        pState
            pointer to the process monitor state object

    @return 0

============================================================================*/
static int ProcessOptions( int argC,
                           char *argV[],
                           ProcmonState *pProcmonState )
{
    int c;
    int result = EINVAL;
    const char *options = "lhvF:f:k:r:s:d:xo:";

    if( ( pProcmonState != NULL ) &&
        ( argV != NULL ) )
    {
        /* save the name of the procmon executable */
        pProcmonState->argv0 = argV[0];

        while( ( c = getopt( argC, argV, options ) ) != -1 )
        {
            switch( c )
            {
                case 'v':
                    pProcmonState->verbose = true;
                    break;

                case 'F':
                    pProcmonState->configFile = optarg;
                    pProcmonState->primary = true;

                case 'f':
                    pProcmonState->configFile = optarg;
                    break;

                case 'h':
                    usage( argV[0] );
                    exit( 0 );
                    break;

                case 'd':
                    result = terminate_and_stop_monitoring( optarg );
                    if ( result != EOK )
                    {
                        fprintf( stderr,
                                 "Failed to terminate %s (%s)\n",
                                 optarg,
                                 strerror( result ) );
                    }
                    exit(result);
                    break;

                case 'k':
                    result = terminate( optarg );
                    if ( result != EOK )
                    {
                        fprintf( stderr,
                                 "Failed to terminate %s (%s)\n",
                                 optarg,
                                 strerror( result ) );
                    }
                    exit(result);
                    break;

                case 'r':
                    result = restart( optarg );
                    if ( result != EOK )
                    {
                        fprintf( stderr,
                                 "Failed to restart %s (%s)\n",
                                 optarg,
                                 strerror( result ) );
                    }
                    exit(result);
                    break;

                case 's':
                    result = start( optarg );
                    if ( result != EOK )
                    {
                        fprintf( stderr,
                                 "Failed to start %s (%s)\n",
                                 optarg,
                                 strerror( result ) );
                    }
                    exit(result);
                    break;

                case 'x':
                    ShutdownAllProcesses(pProcmonState);
                    exit( 0 );

                case 'l':
                    ListProcesses(pProcmonState);
                    exit( 0 );
                    break;

                case 'o':
                    pProcmonState->outputFormat = optarg;
                    ListProcesses(pProcmonState);
                    exit(0);
                    break;

                default:
                    break;

            }
        }
    }

    return 0;
}

/*============================================================================*/
/*  SetupTerminationHandler                                                   */
/*!
    Set up an abnormal termination handler

    The SetupTerminationHandler function registers a termination handler
    function with the kernel in case of an abnormal termination of this
    process.

==============================================================================*/
static void SetupTerminationHandler( void )
{
    static struct sigaction sigact;

    memset( &sigact, 0, sizeof(sigact) );

    sigact.sa_sigaction = TerminationHandler;
    sigact.sa_flags = SA_SIGINFO;

    sigaction( SIGTERM, &sigact, NULL );
    sigaction( SIGINT, &sigact, NULL );

}

/*============================================================================*/
/*  TerminationHandler                                                        */
/*!
    Abnormal termination handler

    The TerminationHandler function will be invoked in case of an abnormal
    termination of this process.  The termination handler closes
    the connection with the variable server and cleans up its VARFP shared
    memory.

@param[in]
    signum
        The signal which caused the abnormal termination (unused)

@param[in]
    info
        pointer to a siginfo_t object (unused)

@param[in]
    ptr
        signal context information (ucontext_t) (unused)

==============================================================================*/
static void TerminationHandler( int signum, siginfo_t *info, void *ptr )
{
    syslog( LOG_ERR, "Abnormal termination of process monitor\n" );

    exit( 1 );
}

/*============================================================================*/
/*  ProcessConfigFile                                                         */
/*!
    Process the process monitor configuration file

    The ProcessConfigFile function will process the procmon configuration
    file and create a datastructure containing the list of processes
    to be started and monitored

@param[in]
    pProcmonState
        pointer to the process monitor state

==============================================================================*/
static int ProcessConfigFile( ProcmonState *pProcmonState )
{
    JNode *config;
    JArray *processes;
    int result = EINVAL;

    if ( pProcmonState != NULL )
    {
        if ( pProcmonState->configFile != NULL )
        {
            config = JSON_Process( pProcmonState->configFile );
            processes = (JArray *)JSON_Find( config, "processes" );
            JSON_Iterate( processes, SetupProcess, (void *)pProcmonState );
            if ( BuildDependencyLists( pProcmonState ) == EOK )
            {
                result = DisplayConfig( pProcmonState );
                RunProcesses( pProcmonState );
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  SetupProcess                                                              */
/*!
    Set up a process object

    The SetupProcess function is a callback function for the JSON_Iterate
    function which sets up a process object from the JSON configuration.
    The process variable definition object is expected to look as follows:

    {
        "id":"<process reference name (must be unique)>",
        "exec":"<command to execute to start the process>",
        "wait": <wait time in seconds>,
        "depends": ["<process dependency name>","<process dependency name>"]
    }

    @param[in]
       pNode
            pointer to the process definition node

    @param[in]
        arg
            opaque pointer argument used for the ProcMon state object

    @retval EOK - the process object was set up successfully
    @retval EINVAL - the process object could not be set up

==============================================================================*/
static int SetupProcess( JNode *pNode, void *arg )
{
    ProcmonState *pProcmonState = (ProcmonState *)arg;
    ProcessNode *pProcessNode;
    int result = EINVAL;
    Process *p;
    JNode *pDepends;
    char *waitstr;
    int i;

    if( pProcmonState != NULL )
    {
        /* allocate memory for the process node */
        pProcessNode = calloc( 1, sizeof( ProcessNode ) );
        if ( pProcessNode != NULL )
        {
            /* allocate memory for the process object */
            p = calloc( 1, sizeof( Process ) );
            if ( p != NULL )
            {
                p->id = JSON_GetStr( pNode, "id" );
                p->exec = JSON_GetStr( pNode, "exec" );
                waitstr = JSON_GetStr( pNode, "wait" );
                if ( waitstr != NULL )
                {
                    p->wait = atoi(waitstr);
                }

                p->monitored = JSON_GetBool( pNode, "monitored" );
                p->verbose = JSON_GetBool( pNode, "verbose" );
                p->skip = JSON_GetBool( pNode, "skip" );
                p->restart_on_parent_death = JSON_GetBool( pNode,
                                                "restart_on_parent_death" );

                pDepends = JSON_Attribute( (JObject *)pNode, "depends" );
                if ( ( pDepends != NULL ) && ( pDepends->type == JSON_ARRAY ) )
                {
                    p->pDepends = (JArray *)pDepends;
                }

                pProcessNode->pProcess = p;

                if ( pProcmonState->pLast == NULL )
                {
                    pProcmonState->pFirst = pProcessNode;
                    pProcmonState->pLast = pProcessNode;
                }
                else
                {
                    pProcmonState->pLast->pNext = pProcessNode;
                    pProcmonState->pLast = pProcessNode;
                }

                result = EOK;
            }
            else
            {
                free( pProcessNode );
                pProcessNode = NULL;
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  FindProcess                                                               */
/*!
    Find a process object

    The FindProcess function iterates through the process list
    looking for a process which exactly matches the specified process
    identifier.

    @param[in]
       id
            pointer to the name of the process to search for

    @param[in]
       pProcmonState
            pointer to the Process Monitor state object which contains the
            process list to search in

    @retval pointer to the found Process
    @retval NULL - if the process object could not be found

==============================================================================*/
Process *FindProcess( char *id, ProcmonState *pProcmonState )
{
    ProcessNode *pProcessNode;
    Process *pProcess = NULL;

    if ( ( pProcmonState != NULL ) &&
         ( id != NULL ) )
    {
        pProcessNode = pProcmonState->pFirst;
        while ( pProcessNode != NULL )
        {
            pProcess = pProcessNode->pProcess;
            if ( pProcess != NULL )
            {
                if ( pProcess->id != NULL )
                {
                    if ( strcmp( id, pProcess->id ) == 0 )
                    {
                        break;
                    }
                }
            }

            pProcess = NULL;
            pProcessNode = pProcessNode->pNext;
        }
    }

    return pProcess;
}

/*============================================================================*/
/*  BuildDependencyLists                                                      */
/*!
    Build parent/child dependency lists for all processes

    The BuildDependencyLists function iterates through the process list
    and builds the relationships between parents and children based
    on the dependency JSON object retrieved from the process configuration
    file.

    @param[in]
       pProcmonState
            pointer to the process monitor state object which
            contains the list of processes to analyze

    @retval EOK - updated all process dependencies
    @retval ENOMEM - memory allocation failure
    @retval ENOENT - parent dependency not found
    @retval EINVAL - invalid arguments

==============================================================================*/
static int BuildDependencyLists( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    ProcessNode *pProcessNode;
    Process *pProcess;
    int rc;

    if ( pProcmonState != NULL )
    {
        result = EOK;

        pProcessNode = pProcmonState->pFirst;
        while ( pProcessNode != NULL )
        {
            pProcess = pProcessNode->pProcess;
            rc = AddParents( pProcmonState, pProcess );
            if ( rc != EOK )
            {
                fprintf( stderr,
                         "Failed to add parents for %s\n",
                         pProcess->id );
                result = rc;
            }

            pProcessNode = pProcessNode->pNext;
        }
    }

    return result;
}

/*============================================================================*/
/*  AddParents                                                                */
/*!
    Add parent dependencies for the specified process

    The AddParents function iterates through the JSON dependency list
    for the specified process, searches for the parent processes,
    and adds references to the parent processes to the process parent
    dependency list.

    @param[in]
       pProcmonState
            pointer to the process monitor state object which
            contains the list of processes to analyze

    @retval EOK - updated all process dependencies
    @retval ENOMEM - memory allocation failure
    @retval ENOENT - parent dependency not found
    @retval EINVAL - invalid arguments

==============================================================================*/
static int AddParents( ProcmonState *pProcmonState, Process *pProcess )
{
    JArray *pDepends;
    JNode *pName;
    JVar *pVar;
    int i = 0;
    int result = EINVAL;
    Process *p;
    char *id;
    int rc;

    if ( pProcess != NULL )
    {
        pDepends = pProcess->pDepends;
        result = EOK;

        do
        {
            /* iterate through the JSON dependency list */
            pName = JSON_Index( pDepends, i++ );
            if( ( pName != NULL ) && ( pName->type == JSON_VAR ) )
            {
                pVar = (JVar *)pName;
                if ( pVar->var.type = JVARTYPE_STR )
                {
                    id = pVar->var.val.str;
                    p = FindProcess( id, pProcmonState );
                    if ( p != NULL )
                    {
                        rc = AddParent( pProcess, p );
                        if ( rc != EOK )
                        {
                            result = rc;
                        }
                    }
                    else
                    {
                        fprintf( stderr,
                                 "Cannot find parent %s for process %s\n",
                                 id,
                                 pProcess->id );
                        result = ENOENT;
                        break;
                    }
                }
            }
        } while( pName != NULL );
    }

    return result;
}

/*============================================================================*/
/*  AddChild                                                                  */
/*!
    Add child dependencies for the specified process

    The AddChild function adds the parent/child relationship for the
    specified parent and child processes.  It adds the child process
    to the parent's child dependency list

    @param[in]
        pParent
            pointer to the parent process containing the child list to update

    @param[in]
        pChild
            pointer to the child process to add to the parent's child list.

    @retval EOK - parent/child relationship successfully updated
    @retval ENOMEM - memory allocation failure
    @retval EINVAL - invalid arguments

==============================================================================*/
static int AddChild( Process *pParent, Process *pChild )
{
    int result = EINVAL;
    ProcessNode *pNode;

    if ( ( pParent != NULL ) && ( pChild != NULL ) )
    {
        if ( pParent != pChild )
        {
            result = ENOMEM;
            pNode = calloc( 1, sizeof( ProcessNode ) );
            if ( pNode != NULL )
            {
                pNode->pProcess = pChild;
                pNode->pNext = pParent->pChildren;
                pParent->pChildren = pNode;

                result = EOK;
            }
        }
        else
        {
            fprintf( stderr,
                     "Process  %s cannot be a child of itself\n",
                     pParent->id );
        }
    }

    return result;
}

/*============================================================================*/
/*  AddParent                                                                 */
/*!
    Add parent dependencies for the specified child process

    The AddParent function adds the parent/child relationship for the
    specified parent and child processes.  It adds the parent process
    to the child's parent dependency list

    @param[in]
        pChild
            pointer to the child process containing the parent list to update

    @param[in]
        pParent
            pointer to the parent process to add to the child's parent list.

    @retval EOK - parent/child relationship successfully updated
    @retval ENOMEM - memory allocation failure
    @retval EINVAL - invalid arguments

==============================================================================*/
static int AddParent( Process *pChild, Process *pParent )
{
    int result = EINVAL;
    ProcessNode *pNode;

    if ( ( pParent != NULL ) && ( pChild != NULL ) )
    {
        if ( pParent != pChild )
        {
            result = ENOMEM;
            pNode = calloc( 1, sizeof( ProcessNode ) );
            if ( pNode != NULL )
            {
                pNode->pProcess = pParent;
                pNode->pNext = pChild->pParents;
                pChild->pParents = pNode;

                result = AddChild( pParent, pChild );
            }
        }
        else
        {
            fprintf( stderr,
                     "Process  %s cannot be a parent of itself\n",
                     pParent->id );
        }
    }

    return result;
}

/*============================================================================*/
/*  DisplayConfig                                                             */
/*!
    Display the Process configuration managed by the process monitor

    The DisplayConfig function displays the state of each process
    monitored by the process monitor

    The configuration will not be displayed unless the process monitor's
    verbose flag has been set via the command line arguments

    @param[in]
        pProcmonState
            pointer to the process monitor state object containing the
            process configuration to display

    @retval EOK - configuration displayed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int DisplayConfig( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    ProcessNode *pProcessNode;
    Process *pProcess;
    int rc;

    if ( pProcmonState != NULL )
    {
        result = EOK;

        if ( pProcmonState->verbose == true )
        {
            pProcessNode = pProcmonState->pFirst;
            while ( pProcessNode != NULL )
            {
                pProcess = pProcessNode->pProcess;
                rc = DisplayProcess( pProcess );
                if ( rc != EOK )
                {
                    result = rc;
                }

                pProcessNode = pProcessNode->pNext;
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  DisplayProcess                                                            */
/*!
    Display the Process configuration for the specified process

    The DisplayProcess function displays the state of the specified process
    including:

    - process id
    - process execution statement
    - wait time
    - list of dependencies
    - current operating state

    @param[in]
        pProcess
            pointer to the process to display

    @retval EOK - process status displayed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int DisplayProcess( Process *pProcess )
{
    int result = EINVAL;
    ProcessNode *pProcessNode;

    if ( pProcess != NULL )
    {
        printf("process: %s\n", pProcess->id );

        DisplayState( pProcess );

        printf("\texec: %s\n", pProcess->exec );

        if ( pProcess->wait > 0 )
        {
            printf("\twait: %d\n", pProcess->wait );
        }

        printf( "\tmonitored: %s\n",
                ( pProcess->monitored == true ) ? "yes" : "no" );

        printf("\tDepends on: [");
        DisplayProcessIds( pProcess->pParents );
        printf("]\n");

        printf("\tDependency of: [");
        DisplayProcessIds( pProcess->pChildren );
        printf("]\n");

        printf("\n");

        result = EOK;
    }

    return result;
}

/*============================================================================*/
/*  DisplayState                                                              */
/*!
    Display the operating state of the specified process

    The DisplayState function displays the operating state of
    the specified process.  This can be one of:

    - UNKNOWN
    - INIT
    - STARTED
    - RUNNING
    - TERMINATED
    - WAITING

    @param[in]
        pProcess
            pointer to the process to display the operating state for

    @retval EOK - process operating state displayed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int DisplayState( Process *pProcess )
{
    int result = EINVAL;

    if ( pProcess != NULL )
    {
        if( pProcess->state > ( sizeof( ProcessStates )/sizeof (char *)) )
        {
            printf("\tstate: UNKNOWN");
        }
        else
        {
            printf("\tstate: %s\n", ProcessStates[pProcess->state]);
            result = EOK;
        }
    }

    return result;
}

/*============================================================================*/
/*  DisplayProcessIds                                                         */
/*!
    Display the identifiers of the processes in the process node list

    The DisplayProcessIds function displays the names of processes
    in the specified process node list.

    @param[in]
        pProcessNode
            pointer to a process node list containing a list of processes
            to display the names for.

==============================================================================*/
static void DisplayProcessIds( ProcessNode *pProcessNode )
{
    int count = 0;
    while( pProcessNode != NULL )
    {
        if ( ++count > 1 )
        {
            printf(",");
        }

        (void)DisplayProcessId( pProcessNode->pProcess );
        pProcessNode = pProcessNode->pNext;
    }
}

/*============================================================================*/
/*  DisplayProcessId                                                          */
/*!
    Display the process identifier for the specified process

    The DisplayProcessId function displays the process identifier for
    the specified process.

    @param[in]
        pProcess
            pointer to the process to display the process identifier for

    @retval EOK - process identifier displayed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int DisplayProcessId( Process *pProcess )
{
    int result = EINVAL;

    if ( ( pProcess != NULL ) && ( pProcess->id != NULL ) )
    {
        printf("%s", pProcess->id );
        result = EOK;
    }

    return result;
}

/*============================================================================*/
/*  RunProcesses                                                              */
/*!
    Run all processes managed by the process monitor

    The RunProcesses function ierates the through all the processes
    managed by the process monitor and attempds to run them based on their
    specified dependencies.  A process can only be run if
        1) it has no dependencies
        2) all its dependencies are already running
        3) it is not already running

    @param[in]
        pProcmonState
            pointer to the process monitor to run the processes for

    @retval EOK - all processes successfully running
    @retval EINVAL - invalid arguments

==============================================================================*/
static int RunProcesses( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    bool running = false;
    Process *pProcess;
    ProcessNode *pProcessNode;

    if ( pProcmonState != NULL )
    {
        while( running == false )
        {
            running = true;
            pProcessNode = pProcmonState->pFirst;
            while( pProcessNode != NULL )
            {
                pProcess = pProcessNode->pProcess;
                if ( Runnable( pProcess ) == EOK )
                {
                    Run( pProcess );
                }
                else
                {
                    running = false;
                }

                pProcessNode = pProcessNode->pNext;
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  Runnable                                                                  */
/*!
    Check if a process is runnable

    The Runnable function checks if the specified process is runnable.
    A process can only be run if
        1) it has no dependencies
        2) all its dependencies are already running
        3) it is not already running

    @param[in]
        pProcess
            pointer to the process to check the runnable state for

    @retval EOK - this process is runnable
    @retval EINVAL - invalid arguments

==============================================================================*/
static int Runnable( Process *pProcess )
{
    ProcessNode *pProcessNode;
    int result = EINVAL;

    if ( pProcess != NULL )
    {
        pProcessNode = pProcess->pParents;

        result = EOK;

        while ( pProcessNode != NULL )
        {
            if ( pProcessNode->pProcess != NULL )
            {
                if ( pProcessNode->pProcess->state != PROCSTATE_eRUNNING )
                {
                    result = EAGAIN;
                    break;
                }
            }

            pProcessNode = pProcessNode->pNext;
        }
    }

    return result;
}

/*============================================================================*/
/*  Run                                                                       */
/*!
    Start running a process

    The Run function initiates a process and sets
    its state to PROCSTATE_eRUNNING once it is successfully
    running.

    @param[in]
        pProcess
            pointer to the process to start

    @retval EOK - this process is running
    @retval EINVAL - invalid arguments

==============================================================================*/
static int Run( Process *pProcess )
{
    int result = EINVAL;

    if ( pProcess != NULL )
    {
        if( pProcess->skip == false )
        {
            InitProcess( pProcess );
            WaitProcess( pProcess );
        }

        pProcess->state = PROCSTATE_eRUNNING;

        result = EOK;
    }

    return result;
}

/*============================================================================*/
/*  WaitProcess                                                               */
/*!
    Wait for a process to start up

    The WaitProcess function waits for the process to start up
    under the following conditions:

    - it has a defined wait time
    - it is not already a monitored running process
    - it is an unmonitored process which has an execution count less than
      the smallest runcount of its parent(s)

    @param[in]
        pProcess
            pointer to the process to wait for

==============================================================================*/
static void WaitProcess( Process *pProcess )
{
    pid_t pid;

    if ( pProcess != NULL )
    {
        if ( pProcess->wait > 0 )
        {
            pid = get_pid_from_lockfile( pProcess->id );
            if ( pid == 0 )
            {
                if ( ( pProcess->monitored == true ) ||
                     ( pProcess->runcount < GetParentRuncount( pProcess ) ) )
                {
                    pProcess->state = PROCSTATE_eWAITING;
                    sleep( pProcess->wait );
                }
            }
        }
    }
}

/*============================================================================*/
/*  InitProcess                                                               */
/*!
    Initialize a process monitoring instance and start running the process

    The InitProcess function initiates a process monitoring thread
    to make sure the process remains running, and starts the process
    running.

    @param[in]
        pProcess
            pointer to the process to start

    @retval EOK - the monitoring thread was successfully created and is running
    @retval EINVAL - invalid arguments
    @retval other - error from pthread_create

==============================================================================*/
static int InitProcess( Process *pProcess )
{
    int result = EINVAL;

    if ( pProcess != NULL )
    {
        if ( pProcess->verbose == true )
        {
            printf("Creating thread for process %s\n", pProcess->id );
        }

        result = pthread_create( &pProcess->thread,
                                 NULL,
                                 &MonitorThread,
                                 pProcess );

        if ( result == EOK )
        {
            pProcess->state = PROCSTATE_eSTARTED;
        }
    }

    return result;
}

/*============================================================================*/
/*  RunProcess                                                                */
/*!
    Run the process

    The RunProcess function runs the process specified in the "exec"
    attribute of the process object using the execvp system call.
    It first decomposes the command string into the command and
    its arguments and creates an arg vector.

    This function should not return unless an error has occurred.

    @param[in]
        pProcess
            pointer to the process to run

    @retval EINVAL - invalid arguments
    @retval other - error from pthread_create

==============================================================================*/
static int RunProcess( Process *pProcess )
{
    char *argv[40];
    char *saveptr;
    char *exec;
    int i = 0;
    int result = EINVAL;

    if ( pProcess != NULL )
    {
        if ( pProcess->verbose == true )
        {
            printf("RunProcess: %s\n", pProcess->id );
        }

        exec = strdup( pProcess->exec );
        if ( exec != NULL )
        {
            argv[0] = strtok_r( exec, " ", &saveptr );
            do
            {
                i++;
                argv[i] = strtok_r( NULL, " ", &saveptr );
            } while( argv[i] != NULL );
        }

        /* create a lock file if this process is to be monitored */
        if ( pProcess->monitored == true )
        {
            /* get the process id of this process */
            pProcess->pid = getpid();

            if ( makelock( pProcess ) != EOK )
            {
                fprintf( stderr,
                         "Failed to make lock for %s\n",
                         pProcess->id );
            }
        }

        /* spawn the new process, replacing this process with the new one */
        if ( pProcess->verbose == true )
        {
            printf( "running %s\n", pProcess->exec );
        }

        execvp( argv[0], argv );

        /* should not get here unless the process failed to start */
        result = errno;

        fprintf( stderr,
                 "Failed to execute: %s\nProcess failed with error %s\n",
                 pProcess->exec,
                 strerror(result));

    }

    return result;
}

/*============================================================================*/
/*  MonitorThread                                                             */
/*!
    Thread to start and optionally monitor a single process for process death

    The MonitorThread function forks the current process and then replaces
    the new child process with the specified process to be run.
    If the child process is to be monitored, the parent process will then
    monitor the child process and re-start it if it terminates for any reason.

    @param[in]
        arg
            pointer to a process info (ProcInfo) object containing information
            about the process to be monitored

    @retval NULL - process completed (or invalid argument)

==============================================================================*/
static void *MonitorThread( void *arg )
{
    Process *pProcess = (Process *)arg;
    pid_t pid = 0;
    int wstatus;
    bool run = true;

    if ( pProcess != NULL )
    {
        if ( ( pProcess->monitored == false ) &&
             ( pProcess->runcount >= GetParentRuncount( pProcess ) ) )
        {
            run = false;
        }

        while ( run )
        {
            /* check if the process is already running */
            pid = get_pid_from_lockfile( pProcess->id );

            if ( pid == -2 )
            {
                /* terminate all monitoring and remove lockfile */
                remove_lockfile( pProcess->id );
                run = false;
                break;
            }

            if ( pid == -1 )
            {
                /* monitoring has been suspended */
                /* periodically check to see if the process has
                 * been (re)started */
                sleep(1);
                continue;
            }

            if ( pid == 0 )
            {
                /* process is not running */
                pProcess->runcount++;

                if ( pProcess->restart_delay != 0 )
                {
                    /* wait before restarting the process */
                    sleep( pProcess->restart_delay );
                }

                /* fork a new process which will run the specified process */
                pid = fork();
            }

            if ( pid == 0 )
            {
                /* child */
                /* detach from parent */
                pid = setsid();
                if ( pid == -1 )
                {
                    fprintf( stderr, "error: %s\n", strerror(errno));
                }

                /* run the process */
                RunProcess( pProcess );

                /* if we get here the exec failed */
                fprintf( stderr, "Failed to execute: %s\n", pProcess->exec );
                break;
            }
            else
            {
                if( pProcess->monitored == true )
                {
                    /* kick off all dependents */
                    RestartDependents( pProcess );

                    /* wait for child to start up */
                    usleep(500000);

                    /* monitor the process to detect process death */
                    Monitor( pProcess->id, -1 );

                    /* wait for child death */
                    waitpid( pid, &wstatus, 0 );

                    if ( pProcess->verbose == true )
                    {
                        fprintf( stderr,
                                "Process %s terminated (wstatus=%d)\n",
                                pProcess->id,
                                wstatus );
                    }
                }
                else
                {
                    /* processes which are not monitored */
                    if ( pProcess->verbose == true )
                    {
                        printf("%s will not be monitored\n", pProcess->id );
                    }

                    waitpid( pid, &wstatus, 0 );

                    if ( pProcess->verbose == true )
                    {
                        printf("%s terminated\n", pProcess->id );
                    }

                    RestartDependents( pProcess );
                    break;
                }
            }
        }
    }

    return NULL;
}

/*============================================================================*/
/*  RestartDependents                                                         */
/*!
    Restart all dependent processes (if applicable)

    The RestartDependents function checks and restarts each of the
    dependent processes of the specified process if they meet the
    following conditions:

        - the dependent process has the "restart_on_parent_death" flag set
        - the dependent process does not have the "skip" flag set
        - the dependent process has been initialized

    @param[in]
        pProcess
            pointer to the process whose dependents should be restarted

    @retval EOK - all dependent processes were sent a restart signal
    @retval EINVAL - invalid arguments
    @retval other - error if dependent's lockfile could not be accessed

==============================================================================*/
static int RestartDependents( Process *pProcess )
{
    int result = EINVAL;
    ProcessNode *pProcessNode;
    int rc;

    if ( pProcess != NULL )
    {
        result = EOK;

        /* iterate through all dependent processes and restart them
         * if necessary */
        pProcessNode = pProcess->pChildren;
        while( pProcessNode != NULL )
        {
            rc = RestartDependent( pProcessNode->pProcess, pProcess->wait );
            if ( rc != EOK )
            {
                result = EOK;
            }

            pProcessNode = pProcessNode->pNext;
        }
    }

    return result;
}

/*============================================================================*/
/*  RestartDependent                                                          */
/*!
    Restart a dependent process

    The RestartDependent function checks and restarts a dependent
    process if it meets the following conditions:

        - the dependent process has the "restart_on_parent_death" flag set
        - the dependent process does not have the "skip" flag set
        - the dependent process has been initialized

    @param[in]
        pProcess
            pointer to the dependent process to restart

    @param[in]
        wait
            optional time to wait after the processes death is detected
            before the new process is spawned.  0=no wait

    @retval EOK - the dependent process was sent a restart signal
    @retval EINVAL - invalid arguments
    @retval other - error if dependent process lockfile could not be accessed

==============================================================================*/
static int RestartDependent( Process *pProcess, int wait )
{
   int result = EINVAL;

   if ( pProcess != NULL )
   {
       result = EOK;

       if ( pProcess->restart_on_parent_death &&
            pProcess->skip == false &&
            ( pProcess->state != PROCSTATE_eINIT ) )
       {
           /* set delay after process death detection before process will
            * be initiated */
           pProcess->restart_delay = wait;

           /* restart the process */
           if ( pProcess->monitored )
           {
               result = restart( pProcess->id );
           }
           else
           {
               result = InitProcess( pProcess );
           }
       }
   }

   return result;
}

/*============================================================================*/
/*  GetParentRuncount                                                         */
/*!
    Get the parent runcount

    The GetParentRuncount function gets the largest runcount from all of the
    processes parents.

    @param[in]
        pProcess
            pointer to a process to get the parent runcount for

    @retval count of number of times this processes parents have been run

==============================================================================*/
static size_t GetParentRuncount( Process *pProcess )
{
    size_t runcount = 0;
    ProcessNode *pNode;
    Process *pParent;

    if ( pProcess != NULL )
    {
        pNode = pProcess->pParents;
        while( pNode != NULL )
        {
            pParent = pNode->pProcess;
            if ( pParent != NULL )
            {
                if( pParent->runcount > runcount )
                {
                    runcount = pParent->runcount;
                }
            }

            pNode = pNode->pNext;
        }
    }

    return runcount;
}

/*============================================================================*/
/*  Monitor                                                                   */
/*!
    Monitor a process and wait for process death

    The Monitor function opens the process lockfile and then tries to
    create a write lock on the file, which it will not be able to do
    while the process is alive.  This monitor function will then block
    until it CAN place a write lock on the file, indicating that the
    process which owns the lockfile has terminated (died).  It will
    then remove its lock on the file and delete the lock file and exit
    allowing the calling function to restart the process if necessary.

    @param[in]
        pid
            process identifier of the process to monitor

    @param[in]
        fd
            file descriptor for the process lockfile

==============================================================================*/
static void Monitor( char *name, int fd )
{
    int rc;

    if ( name != NULL )
    {
        if ( fd == -1 )
        {
            /* try to open the lock file */
            fd = open_lockfile( name );
        }

        if ( fd != -1 )
        {
            /* wait for process to exit */
            while ( 1 )
            {
                rc = waitlock( fd );
                if ( rc == EOK )
                {
                    /* process exited */
                    unlock( fd );
                    close( fd );
                    break;
                }
                else if ( rc == EDEADLK )
                {
                    /* we need to check for deadlock in the case
                     * where the procmon primary and secondary processes
                     * are trying to lock each other.  In this case, we
                     * revert to polling once/second */
                    sleep(1);
                }
                else
                {
                    fprintf( stderr,
                             "Error getting lockfile: %s\n",
                             strerror( rc ) );
                    break;
                }
            }
        }
        else
        {
            fprintf( stderr,
                     "Failed to start monitoring on process %s\n",
                     name );
        }
    }
}

/*============================================================================*/
/*  makelock                                                                  */
/*!
    Create a process lock

    The makelock function creates a process lock for the
    specified process.  This lock is used to detect process death.

    @param[in]
        pProcess
            pointer to the process to make a lock for

    @retval EOK - this process is running
    @retval EINVAL - invalid arguments
    @retval other error returned by fcntl

==============================================================================*/
static int makelock( Process *pProcess )
{
    int fd;
    LockData ldata;
    int rc = EINVAL;
    pid_t pid;
    char *name;
    int n;

    if ( pProcess != NULL )
    {
        rc = EOK;

        pid = pProcess->pid;
        name = pProcess->id;

        if ( pProcess->verbose == true )
        {
            printf("makelock: %s (%d)\n", name, pid );
        }

        /* clear the lockdata object */
        memset( (void *)&ldata, 0, sizeof(LockData) );

        /* create a lock file to associate with the monitored process */
        fd = open_lockfile( name );
        if ( fd == -1 )
        {
            /* create the lock file for this process */
            fd = create_lockfile( pProcess );
        }
        else
        {
            /* get the previous lock data */
            n = read( fd, &ldata, sizeof(LockData) );
            if ( n == sizeof( LockData ) )
            {
                /* increment the run count */
                ldata.runcount++;

                /* set the process identifier */
                ldata.pid = pid;

                /* set the start time */
                ldata.starttime = time(NULL);

                /* seek to the start of the lock file */
                if ( lseek( fd, 0, SEEK_SET ) != -1 )
                {
                    /* write back the updated lock data */
                    n = write(fd, &ldata, sizeof(LockData) );
                    if ( n != sizeof(LockData) )
                    {
                        rc = errno;
                    }
                }
            }
        }

        /* establish a lock on the file */
        if ( rc == EOK )
        {
            rc = lock( fd, F_SETLK );
        }
    }

    return rc;
}

/*============================================================================*/
/*  waitlock                                                                  */
/*!
    Try to set a write lock for the specified file descriptor

    The waitlock function will try to set a write lock for the
    specified file descriptor associated with a running process.
    If the process is healthy, IT will have a write lock on the
    file descriptor.  If the process which owns the file descriptor
    is dead, then the waitlock will complete indicating that the
    process can be restarted.

    This function will block until process death

    @param[in]
        fd
            file descriptor of process to monitor for death

    @retval EOK - this process has died
    @retval EINVAL - invalid arguments
    @retval other error returned by fcntl

==============================================================================*/
static int waitlock( int fd )
{
    return lock( fd, F_SETLKW );
}

/*============================================================================*/
/*  lock                                                                      */
/*!
    Perform a lock operation on the specified file descriptor

    The lock function will try to perform the specified lock command
    on the specified file descriptor associated with a single
    monitored process.

    Valid commands are:

    - F_SETLKW - try to acquire a lock and wait if the lock is already held
    - F_SETLK - try to acquire a lock
    - F_UNLCK - release a lock

    @param[in]
        fd
            file descriptor of process to monitor for death

    @param[in]
        cmd
            type of lock operation to perform

    @retval EOK - lock action was successful
    @retval EINVAL - invalid arguments
    @retval other error returned by fcntl

==============================================================================*/
static int lock( int fd, int cmd )
{
    char lockfile[64];
    int result = EINVAL;
    struct flock lock;
    pid_t pidf;

    if ( fd != -1 )
    {
        if ( ( cmd == F_SETLKW ) ||
             ( cmd == F_SETLK ) ||
             ( cmd == F_UNLCK ) )
        {
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            lock.l_start = 0;
            lock.l_len = 1;

            result = fcntl( fd, cmd, &lock );
            if ( result == -1 )
            {
                result = errno;
            }
        }
        else
        {
            result = ENOTSUP;
        }
    }

    return result;
}

/*============================================================================*/
/*  unlock                                                                    */
/*!
    Unlock the process lockfile

    The unlock function will try to unlock the lockfile associated
    with the monitored process.

    @param[in]
        fd
            file descriptor of the process lockfile

    @param[in]
        cmd
            type of lock operation to perform

    @retval EOK - lock action was successful
    @retval EINVAL - invalid arguments
    @retval other error returned by fcntl

==============================================================================*/
static int unlock( int fd )
{
    return lock( fd, F_UNLCK );
}


/*============================================================================*/
/*  create_lockfile                                                           */
/*!
    Create a process lockfile

    The create_lockfile process will create a lockfile associated with
    a process to be monitored for death.

    The lockfile will be constructed using /tmp.procmon. followed
    by the process identifier of the monitored process.

    @param[in]
        pProcess
            pointer to the process to create the lockfile for

    @retval fd - file descriptor for the lockfile of the monitored process
    @retval -1 - unable to create lockfile

==============================================================================*/
static int create_lockfile( Process *pProcess )
{
    char lockfile[256];
    int fd = -1;
    int result;
    struct flock lock;
    LockData ldata;
    char *name;
    char *exec;
    int n;
    int len;

    if ( pProcess != NULL )
    {
        memset( (void *)&ldata, 0, sizeof(LockData) );

        name =  pProcess->id;

        if ( name != NULL )
        {
            /* build the process lockfile name */
            sprintf( lockfile, "/tmp/procmon.%s", name );

            /* create the process lockfile */
            fd = open( lockfile, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR );
            if ( fd != -1 )
            {
                ldata.pid = pProcess->pid;
                ldata.runcount = 1;
                ldata.starttime = time(NULL);
                ldata.terminate = 0x0000;

                /* write the lockdata object */
                n = write(fd, &ldata, sizeof(LockData) );
                if ( n == sizeof(LockData) )
                {
                    exec = pProcess->exec;
                    if ( exec != NULL )
                    {
                        len = strlen( exec );

                        /* write the executable name/args */
                        write( fd, exec, strlen(exec) );
                    }
                }
            }
        }
    }

    return fd;
}

/*============================================================================*/
/*  open_lockfile                                                             */
/*!
    Open a process lockfile

    The open_lockfile function tries to open the process lockfile
    associated with the specified process id for read/write access.
    It will try up to 5 times, waiting for 0.1 seconds each time.

    @param[in]
        pid
            monitored process

    @retval fd - file descriptor for the open lockfile of the monitored process
    @retval -1 - unable to open lockfile

==============================================================================*/
static int open_lockfile( char *name )
{
    char lockfile[256];
    int fd = -1;
    int result;
    struct flock lock;
    int tries = 0;

    if ( name != NULL )
    {
        sprintf( lockfile, "/tmp/procmon.%s", name );
        while ( ( tries++ < 5 ) &&
                ( fd == -1 ) )
        {
            fd = open( lockfile, O_RDWR );
            if ( fd == -1 )
            {
                usleep( 100000 );
            }
        }
    }

    return fd;

}

/*============================================================================*/
/*  get_pid_from_lockfile                                                     */
/*!
    read process id from lockfile

    The get_pid_from_lockfile function tries to open the process lockfile
    associated with the specified process name for read access, and then
    read the process id from the lockfile.

    The pid is then checked to see if the process exists by invoking
    kill(pid, 0)

    @param[in]
        name
            name of the process to get the process id for

    @retval pid - process id contained within the process lockfile
    @retval 0 - unable to open/read lockfile
    @retval -1 - process is actively terminated

==============================================================================*/
static pid_t get_pid_from_lockfile( char *name )
{
    char lockfile[256];
    int fd = -1;
    pid_t pid = 0;
    LockData ldata;

    if ( name != NULL )
    {
        sprintf( lockfile, "/tmp/procmon.%s", name );
        fd = open( lockfile, O_RDWR );
        if ( fd != -1 )
        {
            read( fd, &ldata, sizeof( LockData ) );
            pid = ldata.pid;
            close( fd );
        }

        if ( ldata.terminate == 0xDEADBEEF )
        {
            /* suspend monitoring */
            pid = -1;
        }

        if ( ldata.terminate == 0xDEAFBABE )
        {
            /* abort monitoring */
            pid = -2;
        }

        if ( pid > 0 )
        {
            /* check if the PID exists */
            if ( kill( pid, 0 ) == -1 )
            {
                if ( errno == ESRCH )
                {
                    pid = 0;
                }
            }
        }

    }

    return pid;
}

/*============================================================================*/
/*  start                                                                     */
/*!
    start the specified process

    The start function tries to start the named monitored process.
    Only processes which have a /tmp/procmon.<name> and are known
    to the process monitor (via the configuration file) can be started.

    To start a monitored process, we remove the special terminate
    instruction from the lockfile, allowing the process to be re-started
    by the Monitoring thread.

    @param[in]
        name
            name of the process to start

    @retval EOK - the process will be restarted by the monitoring thread
    @retval EINVAL - invalid arguments

==============================================================================*/
static int start( char *name )
{
    int fd;
    int result = EINVAL;
    LockData ldata;
    off_t pos;
    size_t len;
    void *p;
    pid_t pid;

    /* calculate the offset and length of the terminate field
     * in the LockData structure */
    p = (void *)&(ldata.terminate);
    pos = p - (void *)&ldata;
    len = sizeof( ldata.terminate );

    if ( name != NULL )
    {
        fd = open_lockfile( name );
        if ( fd != -1 )
        {
            /* clear the terminate instruction */
            ldata.terminate = 0;

            /* write the terminate command into the LockData object */
            lseek( fd, pos, SEEK_SET );
            if ( write( fd, p, len ) == len )
            {
                result = EOK;
            }
            else
            {
                result = errno;
            }

            /* close the lockfile */
            close( fd );
        }
        else
        {
            result = errno;
        }
    }

    return result;
}

/*============================================================================*/
/*  restart                                                                   */
/*!
    restart the specified process

    The restart function tries to restart the named monitored process
    by sending it a SIGKILL signal and letting the process monitoring
    thread restart it.

    @param[in]
        name
            name of the process to restart

    @retval EOK - the process will be restarted by the monitoring thread
    @retval EINVAL - invalid arguments

==============================================================================*/
static int restart( char *name )
{
    int fd;
    int result = EINVAL;
    LockData ldata;
    pid_t pid;

    if ( name != NULL )
    {
        printf("restarting %s\n", name );

        /* open the process lock file */
        fd = open_lockfile( name );
        if ( fd != -1 )
        {
            /* read the Lock Data */
            read( fd, &ldata, sizeof( LockData ) );

            /* get the process id from the LockData */
            pid = ldata.pid;

            /* close the lockfile */
            close( fd );

            /* terminate the process */
            result = kill( pid, SIGKILL );
            if ( result != EOK )
            {
                result = errno;
            }
        }
        else
        {
            result = errno;
        }
    }

    return result;
}

/*============================================================================*/
/*  terminate_and_stop_monitoring                                             */
/*!
    terminate the specified process

    The terminate function tries to terminate the named
    monitored process. Only processes which have a /tmp/procmon.<name>
    can be terminated.

    To terminate a monitored process, we write a special terminate
    instruction into the lockfile and then send a SIGKILL signal
    to the process to terminate it.  The monitoring thread will notice
    the termination instruction and not try to restart the process.

    After the process has been terminated, it's lockfile will be
    permanently deleted so it cannot be restarted.

    @param[in]
        name
            name of the process to terminate

    @retval EOK - kill signal was successfully sent to the process
    @retval error code indicating inability to kill the process

==============================================================================*/
static int terminate_and_stop_monitoring( char *name )
{
    return terminate_command( name, 0xDEAFBABE );
}

/*============================================================================*/
/*  terminate                                                                 */
/*!
    terminate the specified process

    The terminate function tries to terminate the named
    monitored process. Only processes which have a /tmp/procmon.<name>
    can be terminated.

    To terminate a monitored process, we write a special terminate
    instruction into the lockfile and then send a SIGKILL signal
    to the process to terminate it.  The monitoring thread will notice
    the termination instruction and not try to restart the process.

    @param[in]
        name
            name of the process to terminate

    @retval EOK - kill signal was successfully sent to the process
    @retval error code indicating inability to kill the process

==============================================================================*/
static int terminate( char *name )
{
    return terminate_command( name, 0xDEADBEEF );
}

/*============================================================================*/
/*  terminate_command                                                         */
/*!
    terminate the specified process

    The terminate_command function tries to terminate the named
    monitored process. Only processes which have a /tmp/procmon.<name>
    can be terminated.

    To terminate a monitored process, we write a special terminate
    instruction into the lockfile and then send a SIGKILL signal
    to the process to terminate it.  The monitoring thread will notice
    the termination instruction and not try to restart the process.

    See the cmd argument for a description of the termination modes.

    @param[in]
        name
            name of the process to terminate

    @param[in]
        cmd
            the terminate command to use:

            - 0xDEADBEEF = terminate and suspend monitoring
            - 0xDEAFBABE = terminate and stop monitoring

    @retval EOK - kill signal was successfully sent to the process
    @retval error code indicating inability to kill the process

==============================================================================*/
static int terminate_command( char *name, uint32_t cmd )
{
    int fd;
    int result = EINVAL;
    LockData ldata;
    off_t pos;
    size_t len;
    void *p;
    pid_t pid;

    /* calculate the offset and length of the terminate field
     * in the LockData structure */
    p = (void *)&(ldata.terminate);
    pos = p - (void *)&ldata;
    len = sizeof( ldata.terminate );

    if ( name != NULL )
    {
        fd = open_lockfile( name );
        if ( fd != -1 )
        {
            /* read the Lock Data */
            read( fd, &ldata, sizeof( LockData ) );

            /* get the process id */
            pid = ldata.pid;

            /* set the terminate instruction */
            ldata.terminate = cmd;

            /* reset the start time in the LockData object */
            ResetStartTime( fd );

            /* write the terminate command into the LockData object */
            lseek( fd, pos, SEEK_SET );
            if ( write( fd, p, len ) == len )
            {
                /* terminate the process */
                result = kill( pid, SIGKILL );
                if ( result != EOK )
                {
                    /* get the error */
                    result = errno;
                }
            }
            else
            {
                result = errno;
            }


            /* close the lockfile */
            close( fd );
        }
        else
        {
            result = errno;
        }
    }

    return result;
}

/*============================================================================*/
/*  ResetStartTime                                                            */
/*!
    reset the start/stop time the specified process

    The ResetStartTime function resets the statttime field in the
    LockData object to the current time so the start/stop time of the
    process can be calculated correctly.

    @param[in]
        fd
            file descriptor for the open lockdata file

    @retval EOK - the start/stop time was correctly written to the lockdata file
    @retval error code indicating inability to write the lockdata file

==============================================================================*/
static int ResetStartTime( int fd )
{
    LockData ldata;
    off_t pos;
    size_t len;
    void *p;

    int result = EINVAL;

    if ( fd != -1 )
    {
        result = EOK;

        /* calculate the offset and length of the terminate field
         * in the LockData structure */
        p = (void *)&(ldata.starttime);
        pos = p - (void *)&ldata;
        len = sizeof( ldata.starttime );

        /* set the starttime */
        ldata.starttime = time(NULL);

        /* move to the offset within the lockdata file to write the starttime */
        if( lseek( fd, pos, SEEK_SET ) != -1 )
        {
            /* write the starttime into the lockdata file */
            if ( write( fd, p, len ) != len )
            {
                result = errno;
            }
        }
        else
        {
            result = errno;
        }
    }

    return result;
}

/*============================================================================*/
/*  remove_lockfile                                                           */
/*!
    Remove a process lockfile

    The remove_lockfile function tries to delete the process lockfile
    associated with the specified process id.

    @param[in]
        pid
            monitored process

    @retval EOK - lock file successfully removed
    @retval errno - error returned from unlink function

==============================================================================*/
static int remove_lockfile( char *name )
{
    char lockfile[64];
    int result = EINVAL;

    if ( name != NULL )
    {
        sprintf( lockfile, "/tmp/procmon.%s", name );
        if ( unlink( lockfile ) == -1 )
        {
            result = errno;
        }
        else
        {
            result = EOK;
        }
    }

    return result;
}

/*============================================================================*/
/*  ListProcesses                                                             */
/*!
    List all monitored processes

    The ListProcesses function iterates through the /tmp/procmon.*
    entries and displays information about the monitored processes

    @param[in]
        pProcmonState
            pointer to the process monitor state

    @retval EOK - processes listed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int ListProcesses( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    DIR *dir = NULL;
    struct dirent *entry;
    char *pName;
    int rc;
    bool closeBracket = false;
    int n = 0;

    if ( pProcmonState != NULL )
    {
        result = EOK;

        if ( pProcmonState->outputFormat == NULL )
        {
            printf( "%-15s %8s %8s %16s %7s   %s\n",
                    "Process Name",
                    "pid",
                    "Restarts",
                    "Since",
                    "Status",
                    "Command" );
        }
        else if ( strcmp( pProcmonState->outputFormat, "json" ) == 0 )
        {
            printf("[");
            closeBracket = true;
        }

        dir = opendir( "/tmp" );
        if( dir != NULL )
        {
            while( entry = readdir( dir ) )
            {
                if ( strncmp( entry->d_name, "procmon.", 8 ) == 0 )
                {
                    pName = &entry->d_name[8];
                    rc = DisplayProcessInfo( pProcmonState, pName, n++ );
                    if ( rc != EOK )
                    {
                        result = rc;
                    }
                }
            }

            closedir( dir );
        }

        if ( closeBracket )
        {
            printf("]");
        }
    }

    return result;
}

/*============================================================================*/
/*  DisplayProcessInfo                                                        */
/*!
    Display information about the specified process

    The DisplayProcessInfo function displays information about a process
    obtained from the process's lockfile, including the Name, PID, and
    running state.

    @param[in]
        pProcmonState
            pointer to the process monitor state

    @param[in]
        name
            name of the process to display

    @param[in]
        n
            index of the displayed process (0=first)

    @retval EOK - process displayed successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int DisplayProcessInfo( ProcmonState *pProcmonState, char *name, int n )
{
    int result = EINVAL;
    int fd;
    LockData ldata;
    bool running;
    char exec[BUFSIZ];
    char proctime[64];
    int len;

    if ( ( pProcmonState != NULL ) &&
         ( name != NULL ) )
    {
        /* clear the lock data */
        memset( (void *)&ldata, 0, sizeof(LockData) );

        /* open the lock file */
        fd = open_lockfile( name );
        if ( fd != -1 )
        {
            /* get the lock data */
            if ( read( fd, &ldata, sizeof(LockData) ) == sizeof(LockData) )
            {
                /* check if process is running */
                running = true;

                if ( kill( ldata.pid, 0 ) == -1 )
                {
                    running = ( errno == ESRCH ) ? false : true;
                }

                /* read the execution string */
                len = read( fd, exec, BUFSIZ );

                /* calculate the process time */
                (void)GetProcessTime( time(NULL) - ldata.starttime,
                                      proctime,
                                      sizeof(proctime) );

                if ( pProcmonState->outputFormat == NULL )
                {
                    /* display the row of process data */
                    printf( "%-15s %8d %8ld %16s %s : %.*s\n",
                            name,
                            ldata.pid,
                            ldata.runcount,
                            proctime,
                            running ? "running" : "stopped",
                            len,
                            exec );
                }
                else if ( strcmp( pProcmonState->outputFormat, "json" ) == 0 )
                {
                    if ( n != 0 )
                    {
                        printf(",");
                    }

                    printf("{\"name\": \"%s\",", name );
                    printf("\"pid\": %d,", ldata.pid );
                    printf("\"runcount\": %ld,", ldata.runcount );
                    printf("\"since\": \"%s\",", proctime );
                    printf("\"state\": \"%s\",",running ? "running":"stopped" );
                    printf("\"exec\": \"%.*s\"}", len, exec );
                }

                result = EOK;
            }
            else
            {
                result = errno;
            }
        }
        else
        {
            result = errno;
        }
    }

    return result;

}

/*============================================================================*/
/*  GetProcessTime                                                            */
/*!
    Convert the process run/stop time into a human readable string

    The GetProcessTime function converts the process run/stop time into
    a string in the form <d>d<h>h<m>m<s>s where

    <d> is the process run/stop time in days
    <h> is the process run/stop time in hours
    <m> is the process run/stop time in minutes
    <s> is the process run/stop time in seconds

    So, for example if a process has been running for 3 days, 12 hours,
    45 minutes, and 37 seconds, the generated string will be: 3d12h45m37s

    @param[in]
        runtime
            the process run/stop time in seconds

    @param[in]
        buf
            pointer to a buffer to store the generated string

    @param[in]
        len
            length of the buffer to store the generated string

    @retval EOK - time string was generated successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int GetProcessTime( long runtime, char *buf, size_t len )
{
    int result = EINVAL;
    int days;
    int hours;
    int mins;
    int secs;

    if ( buf != NULL )
    {
        if ( runtime < 60 )
        {
            snprintf(buf, len, "%lds", runtime);
        }
        else if ( runtime < 3600 )
        {
            mins = runtime / 60;
            runtime -= ( mins * 60 );
            secs = runtime;
            snprintf(buf, len, "%dm%02ds", mins, secs );
        }
        else if ( runtime < 86400 )
        {
            hours = runtime / 3600;
            runtime -= ( hours * 3600 );
            mins = runtime / 60;
            runtime -= ( mins * 60 );
            secs = runtime;
            snprintf(buf, len, "%dh%02dm%02ds", hours, mins, secs );
        }
        else
        {
            days = runtime / 86400;
            runtime -= ( days * 86400 );
            hours = runtime / 3600;
            runtime -= ( hours * 3600 );
            mins = runtime / 60;
            runtime -= ( mins * 60 );
            secs = runtime;
            snprintf(buf, len, "%dd%02dh%02dm%02ds", days, hours, mins, secs );
        }

        result = EOK;
    }

    return result;
}

/*============================================================================*/
/*  ShutdownAllProcesses                                                      */
/*!
    Shut down all monitored processes

    The ShutdownAllProcesses function iterates through all the procmon lock
    files in the /tmp directory and shuts them down one by one.

    @param[in]
        pProcmonState
            pointer to the process monitor state

    @retval EOK - all processes shut down successfully
    @retval EINVAL - invalid arguments
    @retval other - one or more processes could not be shut down

==============================================================================*/
static int ShutdownAllProcesses( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    DIR *dir = NULL;
    struct dirent *entry;
    char *pName;
    int rc;

    if ( pProcmonState != NULL )
    {
        result = EOK;

        printf("shutting down all processes....\n");

        dir = opendir( "/tmp" );
        if( dir != NULL )
        {
            while( entry = readdir( dir ) )
            {
                if ( strncmp( entry->d_name, "procmon.", 8 ) == 0 )
                {
                    pName = &entry->d_name[8];

                    /* shutdown all processes except the process monitor
                     * processes to give them a chance to clean up */
                    if ( strncmp( pName, "procmon", 7 ) != 0 )
                    {
                        printf("terminating %s\n", pName );
                        rc = terminate_and_stop_monitoring( pName );
                        if ( rc != EOK )
                        {
                            fprintf( stderr,
                                    "Failed to terminate %s (%s)\n",
                                    pName,
                                    strerror(rc));
                            result = rc;

                            /* remove the lock file */
                            remove_lockfile( pName );
                        }
                    }
                }
            }

            closedir( dir );
        }

        /* give processes a chance to shut down and be properly cleaned
         * up before we shutdown the process monitor processes */
        sleep(1);

        /* terminate the process monitor primary and secondary */
        terminate_and_stop_monitoring( "procmon1" );
        terminate_and_stop_monitoring( "procmon1" );

        /* give process monitor processes a chance to shutdown */
        sleep( 1 );

        /* remove the process monitor lockfiles */
        remove_lockfile( "procmon1" );
        remove_lockfile( "procmon2" );
    }

    return result;
}

/*============================================================================*/
/*  MonitorProcmon                                                            */
/*!
    Monitor the process monitor process

    The MonitorProcmon function creates and initializes a new process
    which starts and monitors either the primary or secondary process
    monitor depending on the value of the primary attribute in the
    provided process monitor state object.

    @param[in]
        pProcmonState
            pointer to the process monitor state

    @retval EOK - the process monitor was started successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int MonitorProcmon( ProcmonState *pProcmonState )
{
    Process *p;
    char cmd[BUFSIZ];
    int result = EINVAL;
    char *fileArg;

    if ( pProcmonState != NULL )
    {
        /* allocate memory for the monitored process object */
        p = calloc( 1, sizeof( Process ) );
        if ( p != NULL )
        {
            /* if we are the primary, we will be monitoring the secondary
             * and vice-versa */
            fileArg =  pProcmonState->primary ? "-f" : "-F";

            /* build the command of the process we are starting/monitoring */
            snprintf( cmd,
                      BUFSIZ,
                      pProcmonState->verbose ? "%s -v %s %s" : "%s %s %s",
                      pProcmonState->argv0,
                      fileArg,
                      pProcmonState->configFile );

            /* build the procmonProcess object */
            p->exec = strdup(cmd);
            p->verbose = pProcmonState->verbose;
            p->monitored = true;
            p->id = pProcmonState->primary ? "procmon2" : "procmon1";

            /* store a reference to the monitored process */
            pProcmonState->pMonitoredProcess = p;

            /* start the procmon process monitor */
            result = InitProcess( p );
        }
    }

    return result;
}

/*============================================================================*/
/*  MakeOwnLock                                                               */
/*!
    Create a lockfile for the procmon process

    The MakeOwnLock function creates a new lockfile for the process monitor.
    It will create a lock file for the primary or secondary process monitor,
    depending on the value of the "primary" attribute in the ProcmonState
    object.

    @param[in]
        pProcmonState
            pointer to the process monitor state

    @retval EOK - the process monitor lock file was created successfully
    @retval ENOMEM - memory allocation failure
    @retval EINVAL - invalid arguments

==============================================================================*/
static int MakeOwnLock( ProcmonState *pProcmonState )
{
    int result = EINVAL;
    Process *p;
    char *fileArg;
    char cmd[BUFSIZ];

    if ( pProcmonState != NULL )
    {
        /* allocate the process object */
        p = calloc( 1, sizeof( Process ) );
        if ( p != NULL )
        {
            /* get the process identifier */
            p->pid = getpid();

            /* get the process name */
            p->id = pProcmonState->primary ? "procmon1" : "procmon2";

            p->verbose = pProcmonState->verbose;
            if (pProcmonState->verbose == true ) 
            {
                printf("Creating Lock for %s\n", p->id );
            }

            /* generate the command to show in the process list */
            fileArg =  pProcmonState->primary ? "-F" : "-f";
            snprintf( cmd,
                      BUFSIZ,
                      pProcmonState->verbose ? "%s -v %s %s" : "%s %s %s",
                      pProcmonState->argv0,
                      fileArg,
                      pProcmonState->configFile );

            p->exec = strdup(cmd);

            /* store a reference to the process object */
            pProcmonState->pProcess = p;

            /* create the lock file */
            result = makelock(p);
        }
        else
        {
            result = ENOMEM;
        }
    }

    return result;
}

/*! @}
 * end of procmon group */
