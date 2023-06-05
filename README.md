# procmon
Process Monitor Service

## Overview

The Process Monitor Service provide a mechanism to start a selection
of applications/services as defined in a process configuration file.
The applications/services can have specified dependencies.

The Process Monitor Service will start and then monitor the specified
processes.  If any process stops for any reason, the process monitor
will restart it, and potentially any of its dependent processes as well.

## Configuration file

The Process Configuration file is a JSON file which specifies a list
of processes.  Each process can have a number of specified attributes:

|||
|---|---|
| Attribute | Description |
| id | Process identification string ( must be unique ) |
| exec | Command to execute to start/restart the process |
| restart_delay | Delay after process terminates before it is restarted |
| depends | array of process ids that the specified process depends on |
| restart_on_parent_death | flag indicating to restart the specified process if its parent dies |
| wait | wait time in seconds after starting this process before moving to the next one |
| skip | ignore the process if skip is true |
| monitored | flag to indicated if the process is monitored or not |

### Example Configuration File

An example configuration file is shown below:

```
{
    "processes":[
        {
            "id":"varserver",
            "exec":"varserver",
            "wait":"1",
            "monitored" : true
        },
        {
            "id":"corevars",
            "exec":"varcreate /etc/vars/vars.json",
            "restart_delay" : 1,
            "depends":["varserver"],
            "restart_on_parent_death" : true,
            "wait":"1"
        },
        {
            "id":"filevars",
            "exec":"filevars -f /etc/config/filevars.json",
            "depends":["corevars"],
            "restart_on_parent_death" : true,
            "wait":"1",
            "monitored" : true
        },
        {
            "id":"execvars",
            "exec":"execvars -f /etc/config/execvars.json",
            "depends":["corevars"],
            "restart_on_parent_death" : true,
            "wait":"1",
            "monitored" : true
        }
    ]
}
```

Once parsed the process monitor will start all the processes that have
no dependencies.  Then start each process which has its dependencies
satisfied.  So in the example above, the processes will be started in the
following order:

- varserver
- corevars ( runs once and is not monitored for process death )
- filevars
- execvars

The process monitor will then monitor varserver, filevars and execvars.
If filevars or execvars terminates it will be restarted.  If varserver
terminates, then execvars and filevars will also be terminated and everything
will be restarted.

### Wait attribute

Note that due to the wait attributes, there will be a delay of 1 second
after starting each process before any dependent processes are started.
Note that there is no mechanism for procmon to tell that a process is fully
running.  It can only tell if the process is started.  The wait time allows
the process to start and be fully initialized before starting any dependent
processes.  Wait times will vary depending on the application.  The wait
attribute is optional and may be omitted if it is not necessary in your
application.

### Execute attribute

The exec attribute tells procmon how to invoke the process.  There is only
one exec attribute so there is no way to differentiate when
starting vs restarting the process.

### Depends attribute

The depends attribute is a list of the process ids that the process
is dependent on.  Multiple process ids may be specified.

### Restarting on parent death

The restart_on_parent_death attribute allows the user to specify if the
process should be restarted when its parent is restarted.  In some cases
it may not be necessary to restart the process when its parent dies, so
this attribute can be set to false or omitted entirely.

### Monitored attribute

In some cases we wish to monitor the process and restart it if it dies.
In other cases this is not the case.  In the case where an application
runs to completion, eg varcreate, we do not want to re-start it when it
completes.  So in this case we can set the monitored attribute to false,
or omit it entirely.

## Starting the processes

To start up a system, you can run the procmon service and specify the
path to the JSON process definition file.

For example

```
procmon -f test/procmon.json &
```

This will start the process monitor and kick off all the processes
specified in the configuration file.

## Process Monitor Backup

On startup the process monitor creates a backup process monitor which
monitors the main process monitor.  If the main process monitor dies
it will be restarted by the backup.  If the backup dies, the main process
monitor will create a new backup.  This ensures that the process monitor
is always running.

## Process Monitor Commands

The process monitor application can be used to query the state of the
running processes and start/stop/restart them as well.  It can also be
used to terminate the process monitor gracefully and shut down all the
dependent processes.

| | |
|---|---|
| Command | Description |
| procmon -l | List all of the monitored processes |
| procmon -o json | List all processes in JSON format |
| procmon -k <process id> | kill the specified process and suspend monitoring |
| procmon -s <process id> | start monitoring a previously stopped process |
| procmon -d <process id> | stop process and delete monitoring |
| procmon -f <configfile> | start processes as per configuration |
| procmon -F <configfile> | start processes as per configuration |

Note the only difference between the -f and -F is that one starts the
primary process monitor and the other starts the backup process monitor.
Both have the equivalent end result: 2 processes monitors running.

## List running processes

When listing monitored processes using procmon -l or procmon -o commands
the following information is listed for each monitored process:

- Process Name : The name (id) of the process
- pid: The process pid
- Restarts: The number of restarts of the process (so you can tell if it has
been crashing while you weren't watching)
- Since: the duration the process has been in the current state running/stopped
- Status: the process state: running or stopped
- Command: the exec command used to (re)start the process



