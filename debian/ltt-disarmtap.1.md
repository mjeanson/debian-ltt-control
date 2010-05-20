ltt-disarmtap(1) -- disconnect function markers
===============================================

## SYNOPSIS

`ltt-disarmtap` [<events>]

## DESCRIPTION

`ltt-disarmtap` is a program that will disable the system-wide tap on the
given list of events passed as parameter, and stop the tap at each other
"normal rate" events. Excluding core markers (already connected) and locking
markers (high traffic)

## AUTHOR

`ltt-disarmtap` was written by Mathieu Desnoyers
&lt;mathieu.desnoyers@efficios.com&gt;.

This manual page was written by Jon Bernard &lt;jbernard@debian.org&gt;, for
the Debian project (and may be used by others).
