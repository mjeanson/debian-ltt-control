lttd(1) -- linux trace toolkit daemon
=====================================

## SYNOPSIS

`lttd` [<options>]...

## DESCRIPTION

`lttd` is a simple daemon that reads a few LTTng debugfs channels and saves
them in a trace on the virtual file system.

CPU hot-plugging is supported using inotify.

## OPTIONS

  * `-t` directory:
    Directory name of the trace to write to. It will be created.

  * `-c` directory:
    Root directory of the debugfs trace channels.

  * `-d`:
    Run in background (daemon).

  * `-a`:
    Append to an possibly existing trace.

  * `-N`:
    Number of threads to start.

  * `-f`:
    Dump only flight recorder channels.

  * `-n`:
    Dump only normal channels.

  * `-v`:
    Verbose mode.

## AUTHOR

`lttd` was written by Mathieu Desnoyers
&lt;mathieu.desnoyers@efficios.com&gt;, Michael Sills-Lavoie
&lt;michael.sills-lavoie@polymtl.ca&gt;, and Oumarou Dicko
&lt;oumarou.dicko@polymtl.ca&gt;.

This manual page was written by Jon Bernard &lt;jbernard@debian.org&gt;, for
the Debian project (and may be used by others).
