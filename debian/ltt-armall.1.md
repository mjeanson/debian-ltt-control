ltt-armall(1) -- manipulate system-wide markers
===============================================

## SYNOPSIS

`ltt-armall` [<options>]...

## OPTIONS

  * `-h`:
    Show summary of options.

  * `-l`:
    Also activate locking markers (high traffic).

  * `-n`:
    Also activate detailed network markers (large size).

  * `-i`:
    Also activate input subsystem events (security implication: records
    keyboard inputs).

  * `-q`:
    Quiet mode, suppress output.

## AUTHOR

`ltt-armall` was written by Benjamin Poirier and Mathieu Desnoyers
&lt;mathieu.desnoyers@efficios.com&gt;

This manual page was written by Jon Bernard &lt;jbernard@debian.org&gt;, for
the Debian project (and may be used by others).
